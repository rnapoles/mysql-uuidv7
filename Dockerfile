# syntax=docker/dockerfile:1
#
# Builds the uuidv7 MySQL UDF (uuidv7_mysql.c) and bakes it into the
# official MySQL server image, so `docker run` gives you a server with
# uuidv7(), uuidv4(), uuid_str(), etc. already registered.
#
# Build:
#   docker build -t mysql-uuidv7 .
#   # or target a different MySQL series:
#   docker build --build-arg MYSQL_TAG=8.4 -t mysql-uuidv7 .
#
# Run:
#   docker run --name mysql-uuidv7 -e MYSQL_ROOT_PASSWORD=change-me \
#     -p 3306:3306 -d mysql-uuidv7
#
# Verify:
#   docker exec -it mysql-uuidv7 mysql -uroot -pchange-me -e "SELECT uuidv7();"

ARG MYSQL_TAG=8.0

# ---------------------------------------------------------------------
# Stage 1: compile the UDF.
#
# We build against plain Debian's libmysqlclient-dev rather than
# anything pulled from the mysql: image itself: the UDF_INIT/UDF_ARGS
# struct layout and the my_bool/bool + is_null/error calling
# convention this plugin targets are the same across Oracle MySQL 8.0+
# and MariaDB (verified by compiling and running this exact source
# against both), so a plain Debian toolchain is all that's needed and
# keeps this stage small and independent of the server image's own
# (larger, apt-repo-gated) package set.
# ---------------------------------------------------------------------
FROM debian:bookworm-slim AS builder

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc \
      libc6-dev \
      default-libmysqlclient-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY uuidv7_mysql.c .

RUN gcc -O2 -Wall -fPIC -shared uuidv7_mysql.c -o uuidv7_mysql.so \
      $(mysql_config --cflags) -lpthread

# ---------------------------------------------------------------------
# Stage 2: the real image -- unmodified official MySQL server, plus
# the plugin file and an init script that registers the SQL functions.
# ---------------------------------------------------------------------
FROM mysql:${MYSQL_TAG}

COPY --from=builder /build/uuidv7_mysql.so /usr/lib64/mysql/plugin/uuidv7_mysql.so
RUN chmod 644 /usr/lib64/mysql/plugin/uuidv7_mysql.so

# docker-entrypoint.sh (baked into the base image) runs every *.sql
# file under /docker-entrypoint-initdb.d/ in name order, but only the
# very first time the container starts with an empty data directory --
# see the "Initializing a fresh instance" section of the image's docs.
# If you attach an existing /var/lib/mysql volume, run this file's
# statements manually instead (or add them to a migration).
COPY init-uuidv7.sql /docker-entrypoint-initdb.d/10-uuidv7-functions.sql
