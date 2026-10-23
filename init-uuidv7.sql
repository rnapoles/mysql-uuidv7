-- Registers the uuidv7 UDF functions. Runs automatically on first
-- container startup (see /docker-entrypoint-initdb.d/ in the
-- Dockerfile). Safe to re-run manually: each CREATE is preceded by a
-- DROP IF EXISTS.

DROP FUNCTION IF EXISTS uuidv7;
DROP FUNCTION IF EXISTS uuidv7_blob;
DROP FUNCTION IF EXISTS uuidv4;
DROP FUNCTION IF EXISTS uuidv4_blob;
DROP FUNCTION IF EXISTS uuid_str;
DROP FUNCTION IF EXISTS uuid_blob;
DROP FUNCTION IF EXISTS uuid_version;
DROP FUNCTION IF EXISTS uuidv7_timestamp;
DROP FUNCTION IF EXISTS uuidv7_timestamp_iso;

CREATE FUNCTION uuidv7               RETURNS STRING  SONAME 'uuidv7_mysql.so';
CREATE FUNCTION uuidv7_blob          RETURNS STRING  SONAME 'uuidv7_mysql.so';
CREATE FUNCTION uuidv4               RETURNS STRING  SONAME 'uuidv7_mysql.so';
CREATE FUNCTION uuidv4_blob          RETURNS STRING  SONAME 'uuidv7_mysql.so';
CREATE FUNCTION uuid_str             RETURNS STRING  SONAME 'uuidv7_mysql.so';
CREATE FUNCTION uuid_blob            RETURNS STRING  SONAME 'uuidv7_mysql.so';
CREATE FUNCTION uuid_version         RETURNS INTEGER SONAME 'uuidv7_mysql.so';
CREATE FUNCTION uuidv7_timestamp     RETURNS INTEGER SONAME 'uuidv7_mysql.so';
CREATE FUNCTION uuidv7_timestamp_iso RETURNS STRING  SONAME 'uuidv7_mysql.so';
