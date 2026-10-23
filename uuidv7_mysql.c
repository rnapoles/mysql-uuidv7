/*
** uuidv7_mysql.c - MySQL / MariaDB UDF (User Defined Function) plugin
** providing UUIDv7 and UUIDv4 generation, formatting, and parsing.
**
** This mirrors the SQLite uuidv7 extension's function set. See the
** accompanying README.md for build, install, and usage instructions.
**
** RFC 9562 UUIDv7 layout (128 bits, big-endian on the wire):
**
**   0                   1                   2                   3
**   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
**  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
**  |                           unix_ts_ms                         |
**  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
**  |          unix_ts_ms          |  ver  |       rand_a          |
**  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
**  |var|                        rand_b                            |
**  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
**  |                            rand_b                            |
**  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
**
** SQL functions registered (see README.md for CREATE FUNCTION statements):
**
**   uuidv7()              -> CHAR(36), canonical lowercase UUIDv7
**   uuidv7(ts_ms)          -> CHAR(36), UUIDv7 for a given unix-ms BIGINT
**   uuidv7_blob()           -> BINARY(16), raw bytes of a fresh UUIDv7
**   uuidv4()                -> CHAR(36), canonical random UUIDv4
**   uuidv4_blob()             -> BINARY(16), raw bytes of a fresh UUIDv4
**   uuid_str(x)               -> CHAR(36), formats a 16-byte value (or
**                               reformats text) as canonical lowercase text
**   uuid_blob(x)                -> BINARY(16), parses UUID text (or passes
**                               a 16-byte value through) into raw bytes
**   uuid_version(x)              -> INT, version nibble (1-8), or NULL
**   uuidv7_timestamp(x)           -> BIGINT, unix_ts_ms embedded in a
**                               UUIDv7, or NULL if not version 7
**   uuidv7_timestamp_iso(x)        -> CHAR(24), same timestamp as an
**                               ISO-8601 UTC string
**
** Threading: MySQL/MariaDB may call a UDF concurrently from many server
** threads. All shared mutable state (the monotonic ms/sequence counter
** used to keep uuidv7() ordered within a busy millisecond) is protected
** by a mutex. Each function's own working buffer is allocated in its
** _init and freed in its _deinit, per SQL-statement invocation, which is
** the standard UDF pattern and requires no locking.
**
** Build:
**   gcc -O2 -fPIC -Wall -shared uuidv7_mysql.c -o uuidv7_mysql.so \
**       $(mysql_config --cflags) -lpthread
**
** Install: copy the .so into the server's plugin_dir, then run the
** CREATE FUNCTION statements in README.md.
*/

#include <mysql.h>

/* MySQL 8.0's client headers moved UDF_INIT/UDF_ARGS/the function
** pointer typedefs into a separate header and switched the _init /
** _deinit return type from the old `my_bool` (char) to C99 `bool`.
** MariaDB's mysql.h still defines everything itself, using `my_bool`.
** Detect which we're building against and normalize. */
#if defined(MARIADB_BASE_VERSION) || defined(MARIADB_VERSION_ID)
  /* UDF_INIT, UDF_ARGS, Item_result and my_bool all come from
  ** mariadb_com.h, pulled in by mysql.h already. */
#else
  #include <udf_registration_types.h>
  #if !defined(MYSQL_VERSION_ID) || MYSQL_VERSION_ID >= 80000
    typedef bool my_bool; /* Oracle MySQL 8.0+: init/deinit use C99 bool */
  #endif
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#endif

/* ------------------------------------------------------------------
** Cryptographically-strong random bytes, thread-safe.
** ------------------------------------------------------------------ */
static void fill_random_fallback(unsigned char *buf, size_t n){
  /* Last-resort fallback: /dev/urandom, for POSIX systems where
  ** getrandom() is unavailable or short-reads persistently. */
  FILE *f = fopen("/dev/urandom", "rb");
  size_t got = 0;
  if( f ){
    got = fread(buf, 1, n, f);
    fclose(f);
  }
  if( got < n ){
    /* Should not happen on any real system; better a degraded seed
    ** than a crash. Mix in time/address entropy as a last ditch. */
    size_t i;
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)(intptr_t)buf;
    srand(seed);
    for(i = got; i < n; i++) buf[i] = (unsigned char)(rand() & 0xFF);
  }
}

static void fill_random(unsigned char *buf, size_t n){
#if defined(_WIN32)
  if( !BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf, (ULONG)n,
                       BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ){
    fill_random_fallback(buf, n);
  }
#elif defined(__linux__)
  size_t off = 0;
  while( off < n ){
    ssize_t r = getrandom(buf + off, n - off, 0);
    if( r < 0 ){
      if( errno == EINTR ) continue;
      break;
    }
    off += (size_t)r;
  }
  if( off < n ) fill_random_fallback(buf + off, n - off);
#else
  fill_random_fallback(buf, n);
#endif
}

/* ------------------------------------------------------------------
** Millisecond wall-clock time.
** ------------------------------------------------------------------ */
static uint64_t unix_millis(void){
#if defined(_WIN32)
  FILETIME ft;
  ULARGE_INTEGER li;
  GetSystemTimeAsFileTime(&ft);
  li.LowPart = ft.dwLowDateTime;
  li.HighPart = ft.dwHighDateTime;
  return (uint64_t)((li.QuadPart - 116444736000000000ULL) / 10000ULL);
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* ------------------------------------------------------------------
** Shared monotonic counter, mutex-protected: keeps uuidv7() ordered
** even when many rows are generated within the same millisecond by
** possibly-concurrent server threads.
** ------------------------------------------------------------------ */
static pthread_mutex_t uuidv7_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t uuidv7_last_ms = 0;
static uint16_t uuidv7_seq = 0;

/* Fill 16 bytes with a UUIDv7 for the given millisecond timestamp.
** When useCounter is set, rand_a is drawn from a shared, mutex
** protected sequence counter so ordering is preserved across threads
** generating many UUIDs within the same millisecond. */
static void uuidv7_fill(unsigned char *out, uint64_t ms, int useCounter){
  unsigned char rnd[10];
  uint16_t rand_a;

  fill_random(rnd, sizeof(rnd));

  if( useCounter ){
    pthread_mutex_lock(&uuidv7_mutex);
    if( ms == uuidv7_last_ms ){
      uuidv7_seq = (uuidv7_seq + 1) & 0x0FFF; /* 12 bits */
      if( uuidv7_seq == 0 ){
        /* counter wrapped within this millisecond: nudge the
        ** timestamp forward so ordering is still preserved */
        ms++;
        uuidv7_last_ms = ms;
      }
    } else {
      uuidv7_last_ms = ms;
      uuidv7_seq = ((rnd[0] << 4) | (rnd[1] >> 4)) & 0x0FFF;
    }
    rand_a = uuidv7_seq;
    pthread_mutex_unlock(&uuidv7_mutex);
  } else {
    rand_a = (((uint16_t)rnd[0] << 4) | (rnd[1] >> 4)) & 0x0FFF;
  }

  out[0] = (unsigned char)(ms >> 40);
  out[1] = (unsigned char)(ms >> 32);
  out[2] = (unsigned char)(ms >> 24);
  out[3] = (unsigned char)(ms >> 16);
  out[4] = (unsigned char)(ms >> 8);
  out[5] = (unsigned char)(ms);

  out[6] = (unsigned char)(0x70 | ((rand_a >> 8) & 0x0F)); /* version 7 */
  out[7] = (unsigned char)(rand_a & 0xFF);

  out[8]  = (unsigned char)(0x80 | (rnd[2] & 0x3F)); /* variant 10 */
  out[9]  = rnd[3];
  out[10] = rnd[4];
  out[11] = rnd[5];
  out[12] = rnd[6];
  out[13] = rnd[7];
  out[14] = rnd[8];
  out[15] = rnd[9];
}

static void uuidv4_fill(unsigned char *out){
  fill_random(out, 16);
  out[6] = (unsigned char)(0x40 | (out[6] & 0x0F)); /* version 4 */
  out[8] = (unsigned char)(0x80 | (out[8] & 0x3F)); /* variant 10 */
}

/* ------------------------------------------------------------------
** Text <-> bytes helpers
** ------------------------------------------------------------------ */
static const char HEXD[] = "0123456789abcdef";

/* out must have room for 37 bytes (36 chars + NUL) */
static void uuid_bytes_to_str(const unsigned char *b, char *out){
  int si = 0, bi;
  for(bi = 0; bi < 16; bi++){
    out[si++] = HEXD[(b[bi] >> 4) & 0x0F];
    out[si++] = HEXD[b[bi] & 0x0F];
    if( bi==3 || bi==5 || bi==7 || bi==9 ) out[si++] = '-';
  }
  out[si] = '\0';
}

static int hexval(unsigned char c){
  if( c>='0' && c<='9' ) return c - '0';
  if( c>='a' && c<='f' ) return c - 'a' + 10;
  if( c>='A' && c<='F' ) return c - 'A' + 10;
  return -1;
}

/* Parse UUID text (with or without dashes/braces) into 16 bytes.
** Returns 0 on success, non-zero on malformed input. */
static int uuid_str_to_bytes(const char *z, unsigned long n, unsigned char *out){
  unsigned long i;
  int ni = 0, hi = -1;
  for(i = 0; i < n; i++){
    unsigned char c = (unsigned char)z[i];
    if( c=='-' || c=='{' || c=='}' || c==' ' ) continue;
    if( hexval(c) < 0 ) return 1;
    if( hi < 0 ){
      hi = hexval(c);
    } else {
      if( ni >= 16 ) return 1;
      out[ni++] = (unsigned char)((hi << 4) | hexval(c));
      hi = -1;
    }
  }
  if( hi >= 0 || ni != 16 ) return 1;
  return 0;
}

/* Accepts a raw 16-byte value (already binary) or UUID text and
** extracts 16 bytes. Returns 0 on success. */
static int value_to_uuid_bytes(const char *arg, unsigned long len, unsigned char *out){
  if( arg == NULL ) return 1;
  if( len == 16 ){
    memcpy(out, arg, 16);
    return 0;
  }
  return uuid_str_to_bytes(arg, len, out);
}

/* ==================================================================
** uuidv7() / uuidv7(ts_ms)
** ================================================================== */
my_bool uuidv7_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count > 1 ){
    strncpy(message, "uuidv7() takes zero or one (unix-ms) argument", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  if( args->arg_count == 1 ){
    args->arg_type[0] = INT_RESULT; /* ask MySQL to coerce to BIGINT */
  }
  initid->max_length = 36;
  initid->maybe_null = 0;
  initid->const_item = 0;
  initid->ptr = (char *)malloc(37);
  if( initid->ptr == NULL ){
    strncpy(message, "uuidv7(): out of memory", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  return 0;
}

void uuidv7_deinit(UDF_INIT *initid){
  free(initid->ptr);
}

char *uuidv7(UDF_INIT *initid, UDF_ARGS *args, char *result,
             unsigned long *length, unsigned char *is_null, unsigned char *error){
  unsigned char b[16];
  uint64_t ms;
  int useCounter = 1;
  (void)result; (void)error;

  if( args->arg_count == 1 && args->args[0] != NULL ){
    long long v = *((long long *)args->args[0]);
    if( v < 0 ){
      *is_null = 1;
      return NULL;
    }
    ms = (uint64_t)v;
    useCounter = 0;
  } else {
    ms = unix_millis();
  }

  uuidv7_fill(b, ms, useCounter);
  uuid_bytes_to_str(b, initid->ptr);
  *length = 36;
  *is_null = 0;
  return initid->ptr;
}

/* ==================================================================
** uuidv7_blob()
** ================================================================== */
my_bool uuidv7_blob_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count != 0 ){
    strncpy(message, "uuidv7_blob() takes no arguments", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  initid->max_length = 16;
  initid->maybe_null = 0;
  initid->const_item = 0;
  initid->ptr = (char *)malloc(16);
  if( initid->ptr == NULL ){
    strncpy(message, "uuidv7_blob(): out of memory", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  return 0;
}

void uuidv7_blob_deinit(UDF_INIT *initid){
  free(initid->ptr);
}

char *uuidv7_blob(UDF_INIT *initid, UDF_ARGS *args, char *result,
                   unsigned long *length, unsigned char *is_null, unsigned char *error){
  (void)args; (void)result; (void)error;
  uuidv7_fill((unsigned char *)initid->ptr, unix_millis(), 1);
  *length = 16;
  *is_null = 0;
  return initid->ptr;
}

/* ==================================================================
** uuidv4()
** ================================================================== */
my_bool uuidv4_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count != 0 ){
    strncpy(message, "uuidv4() takes no arguments", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  initid->max_length = 36;
  initid->maybe_null = 0;
  initid->const_item = 0;
  initid->ptr = (char *)malloc(37);
  if( initid->ptr == NULL ){
    strncpy(message, "uuidv4(): out of memory", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  return 0;
}

void uuidv4_deinit(UDF_INIT *initid){
  free(initid->ptr);
}

char *uuidv4(UDF_INIT *initid, UDF_ARGS *args, char *result,
             unsigned long *length, unsigned char *is_null, unsigned char *error){
  unsigned char b[16];
  (void)args; (void)result; (void)error;
  uuidv4_fill(b);
  uuid_bytes_to_str(b, initid->ptr);
  *length = 36;
  *is_null = 0;
  return initid->ptr;
}

/* ==================================================================
** uuidv4_blob()
** ================================================================== */
my_bool uuidv4_blob_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count != 0 ){
    strncpy(message, "uuidv4_blob() takes no arguments", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  initid->max_length = 16;
  initid->maybe_null = 0;
  initid->const_item = 0;
  initid->ptr = (char *)malloc(16);
  if( initid->ptr == NULL ){
    strncpy(message, "uuidv4_blob(): out of memory", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  return 0;
}

void uuidv4_blob_deinit(UDF_INIT *initid){
  free(initid->ptr);
}

char *uuidv4_blob(UDF_INIT *initid, UDF_ARGS *args, char *result,
                   unsigned long *length, unsigned char *is_null, unsigned char *error){
  (void)args; (void)result; (void)error;
  uuidv4_fill((unsigned char *)initid->ptr);
  *length = 16;
  *is_null = 0;
  return initid->ptr;
}

/* ==================================================================
** uuid_str(x)
** ================================================================== */
my_bool uuid_str_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count != 1 ){
    strncpy(message, "uuid_str() takes exactly one argument", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  args->arg_type[0] = STRING_RESULT; /* accept text or binary as-is */
  initid->max_length = 36;
  initid->maybe_null = 1; /* NULL on NULL input or malformed UUID */
  initid->const_item = 0;
  initid->ptr = (char *)malloc(37);
  if( initid->ptr == NULL ){
    strncpy(message, "uuid_str(): out of memory", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  return 0;
}

void uuid_str_deinit(UDF_INIT *initid){
  free(initid->ptr);
}

char *uuid_str(UDF_INIT *initid, UDF_ARGS *args, char *result,
               unsigned long *length, unsigned char *is_null, unsigned char *error){
  unsigned char b[16];
  (void)result; (void)error;
  if( args->args[0] == NULL
   || value_to_uuid_bytes(args->args[0], args->lengths[0], b) ){
    *is_null = 1;
    return NULL;
  }
  uuid_bytes_to_str(b, initid->ptr);
  *length = 36;
  *is_null = 0;
  return initid->ptr;
}

/* ==================================================================
** uuid_blob(x)
** ================================================================== */
my_bool uuid_blob_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count != 1 ){
    strncpy(message, "uuid_blob() takes exactly one argument", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  args->arg_type[0] = STRING_RESULT;
  initid->max_length = 16;
  initid->maybe_null = 1;
  initid->const_item = 0;
  initid->ptr = (char *)malloc(16);
  if( initid->ptr == NULL ){
    strncpy(message, "uuid_blob(): out of memory", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  return 0;
}

void uuid_blob_deinit(UDF_INIT *initid){
  free(initid->ptr);
}

char *uuid_blob(UDF_INIT *initid, UDF_ARGS *args, char *result,
                unsigned long *length, unsigned char *is_null, unsigned char *error){
  (void)result; (void)error;
  if( args->args[0] == NULL
   || value_to_uuid_bytes(args->args[0], args->lengths[0], (unsigned char *)initid->ptr) ){
    *is_null = 1;
    return NULL;
  }
  *length = 16;
  *is_null = 0;
  return initid->ptr;
}

/* ==================================================================
** uuid_version(x) -> INT
** ================================================================== */
my_bool uuid_version_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count != 1 ){
    strncpy(message, "uuid_version() takes exactly one argument", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  args->arg_type[0] = STRING_RESULT;
  initid->maybe_null = 1;
  initid->decimals = 0;
  initid->const_item = 0;
  return 0;
}

void uuid_version_deinit(UDF_INIT *initid){ (void)initid; }

long long uuid_version(UDF_INIT *initid, UDF_ARGS *args,
                        unsigned char *is_null, unsigned char *error){
  unsigned char b[16];
  (void)initid; (void)error;
  if( args->args[0] == NULL
   || value_to_uuid_bytes(args->args[0], args->lengths[0], b) ){
    *is_null = 1;
    return 0;
  }
  *is_null = 0;
  return (long long)((b[6] >> 4) & 0x0F);
}

/* ==================================================================
** uuidv7_timestamp(x) -> BIGINT
** ================================================================== */
my_bool uuidv7_timestamp_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count != 1 ){
    strncpy(message, "uuidv7_timestamp() takes exactly one argument", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  args->arg_type[0] = STRING_RESULT;
  initid->maybe_null = 1;
  initid->decimals = 0;
  initid->const_item = 0;
  return 0;
}

void uuidv7_timestamp_deinit(UDF_INIT *initid){ (void)initid; }

long long uuidv7_timestamp(UDF_INIT *initid, UDF_ARGS *args,
                            unsigned char *is_null, unsigned char *error){
  unsigned char b[16];
  uint64_t ms;
  (void)initid; (void)error;
  if( args->args[0] == NULL
   || value_to_uuid_bytes(args->args[0], args->lengths[0], b)
   || ((b[6] >> 4) & 0x0F) != 7 ){
    *is_null = 1;
    return 0;
  }
  ms = ((uint64_t)b[0] << 40) | ((uint64_t)b[1] << 32) | ((uint64_t)b[2] << 24)
     | ((uint64_t)b[3] << 16) | ((uint64_t)b[4] << 8)  | (uint64_t)b[5];
  *is_null = 0;
  return (long long)ms;
}

/* ==================================================================
** uuidv7_timestamp_iso(x) -> CHAR
** ================================================================== */
my_bool uuidv7_timestamp_iso_init(UDF_INIT *initid, UDF_ARGS *args, char *message){
  if( args->arg_count != 1 ){
    strncpy(message, "uuidv7_timestamp_iso() takes exactly one argument", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  args->arg_type[0] = STRING_RESULT;
  initid->max_length = 24; /* "YYYY-MM-DDTHH:MM:SS.sssZ" */
  initid->maybe_null = 1;
  initid->const_item = 0;
  initid->ptr = (char *)malloc(64);
  if( initid->ptr == NULL ){
    strncpy(message, "uuidv7_timestamp_iso(): out of memory", MYSQL_ERRMSG_SIZE - 1);
    return 1;
  }
  return 0;
}

void uuidv7_timestamp_iso_deinit(UDF_INIT *initid){
  free(initid->ptr);
}

char *uuidv7_timestamp_iso(UDF_INIT *initid, UDF_ARGS *args, char *result,
                            unsigned long *length, unsigned char *is_null, unsigned char *error){
  unsigned char b[16];
  uint64_t ms;
  time_t secs;
  struct tm tmv;
  (void)result; (void)error;

  if( args->args[0] == NULL
   || value_to_uuid_bytes(args->args[0], args->lengths[0], b)
   || ((b[6] >> 4) & 0x0F) != 7 ){
    *is_null = 1;
    return NULL;
  }
  ms = ((uint64_t)b[0] << 40) | ((uint64_t)b[1] << 32) | ((uint64_t)b[2] << 24)
     | ((uint64_t)b[3] << 16) | ((uint64_t)b[4] << 8)  | (uint64_t)b[5];
  secs = (time_t)(ms / 1000);
#if defined(_WIN32)
  gmtime_s(&tmv, &secs);
#else
  gmtime_r(&secs, &tmv);
#endif
  snprintf(initid->ptr, 64, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(ms % 1000));
  *length = (unsigned long)strlen(initid->ptr);
  *is_null = 0;
  return initid->ptr;
}
