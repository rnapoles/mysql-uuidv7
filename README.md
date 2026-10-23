# MySQL UUID Functions

A MySQL extension that provides UUID generation, parsing, formatting, and UUIDv7 timestamp extraction functions.

## Features

- Generate UUIDv7 identifiers using the current time or a supplied Unix timestamp in milliseconds.
- Generate random UUIDv4 identifiers.
- Convert UUIDs between canonical text and 16-byte binary representations.
- Validate UUID input and inspect its version.
- Extract the embedded Unix-millisecond timestamp from UUIDv7 values.
- Format a UUIDv7 timestamp as an ISO-8601 UTC string.

## Functions

| Function | Returns | Description |
|---|---|---|
| `uuidv7()` | `CHAR(36)` | Generates a canonical lowercase UUIDv7 using the current time. |
| `uuidv7(ts_ms)` | `CHAR(36)` | Generates a UUIDv7 for a supplied Unix timestamp in milliseconds (`BIGINT`). |
| `uuidv7_blob()` | `BINARY(16)` | Generates a UUIDv7 as a raw 16-byte value. |
| `uuidv4()` | `CHAR(36)` | Generates a canonical random UUIDv4. |
| `uuidv4_blob()` | `BINARY(16)` | Generates a UUIDv4 as a raw 16-byte value. |
| `uuid_str(x)` | `CHAR(36)` | Formats a 16-byte value, or reformats UUID text, as canonical lowercase text. Returns `NULL` for malformed input. |
| `uuid_blob(x)` | `BINARY(16)` | Parses UUID text, or passes through a 16-byte value, as a raw 16-byte value. Returns `NULL` for malformed input. |
| `uuid_version(x)` | `INT` | Returns the UUID version nibble (1–8), or `NULL` for invalid input. |
| `uuidv7_timestamp(x)` | `BIGINT` | Extracts the Unix timestamp in milliseconds embedded in a UUIDv7. Returns `NULL` if the input is invalid or is not version 7. |
| `uuidv7_timestamp_iso(x)` | `CHAR(24)` | Returns the UUIDv7 timestamp as an ISO-8601 UTC string. Returns `NULL` if the input is invalid or is not version 7. |

## Input formats

The parsing and inspection functions accept either:

- UUID text in canonical form, such as `018f0f5c-5e6a-7abc-8def-0123456789ab`
- UUID text without dashes
- UUID text enclosed in braces
- A 16-byte binary UUID value

The functions coerce their argument to a string internally, regardless of the source column type. For binary input, pass a value containing the raw 16 UUID bytes.

Malformed input returns `NULL` for the functions documented as nullable.

## Examples

### Generate UUIDs

```sql
SELECT uuidv7();
SELECT uuidv7(1710000000000);
SELECT uuidv7_blob();

SELECT uuidv4();
SELECT uuidv4_blob();
```

### Convert between text and binary

```sql
-- Generate canonical UUID text from a binary UUID
SELECT uuid_str(uuidv7_blob());

-- Convert UUID text to its 16-byte representation
SELECT uuid_blob('018f0f5c-5e6a-7abc-8def-0123456789ab');

-- Accept text without dashes or with braces
SELECT uuid_str('018F0F5C5E6A7ABC8DEF0123456789AB');
SELECT uuid_blob('{018f0f5c-5e6a-7abc-8def-0123456789ab}');
```

### Inspect a UUID

```sql
SELECT uuid_version('018f0f5c-5e6a-7abc-8def-0123456789ab');

SELECT uuidv7_timestamp('018f0f5c-5e6a-7abc-8def-0123456789ab');

SELECT uuidv7_timestamp_iso('018f0f5c-5e6a-7abc-8def-0123456789ab');
```

### Use binary UUIDs in a table

```sql
CREATE TABLE events (
    id BINARY(16) NOT NULL PRIMARY KEY,
    event_name VARCHAR(100) NOT NULL
);

INSERT INTO events (id, event_name)
VALUES (uuidv7_blob(), 'user.created');

SELECT uuid_str(id) AS id, event_name
FROM events;
```

## Notes

- UUID text output is canonical and lowercase.
- Binary UUID values are stored as exactly 16 bytes.
- UUIDv7 embeds a Unix timestamp in milliseconds, making its leading bytes time-ordered. This can be useful for database keys and time-based identifiers.
- UUIDv4 is random and does not encode a timestamp.
- `uuidv7(ts_ms)` expects a Unix timestamp in **milliseconds**, not seconds.
- The exact supported MySQL versions, installation steps, and build requirements depend on this project's implementation. See the repository's build and installation documentation.

## Function reference at a glance

| Need | Function |
|---|---|
| Generate a time-ordered UUID as text | `uuidv7()` |
| Generate a UUIDv7 for a known timestamp | `uuidv7(ts_ms)` |
| Generate a time-ordered UUID as binary | `uuidv7_blob()` |
| Generate a random UUID as text | `uuidv4()` |
| Generate a random UUID as binary | `uuidv4_blob()` |
| Convert binary UUID to text | `uuid_str(x)` |
| Convert text UUID to binary | `uuid_blob(x)` |
| Read the UUID version | `uuid_version(x)` |
| Extract UUIDv7 timestamp in milliseconds | `uuidv7_timestamp(x)` |
| Format UUIDv7 timestamp as ISO-8601 UTC | `uuidv7_timestamp_iso(x)` |

