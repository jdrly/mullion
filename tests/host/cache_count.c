// contract: the session header's cache count (src/winemetal/unix/cache_count.h) counts a WAL cache's rows without
// making a file: no -wal or -shm appears beside it, which SQLite makes for a read-only connection to a WAL database
// that has none (https://sqlite.org/wal.html, "Read-Only Databases"). Whether a closed cache keeps them is the
// platform's SQLite's (macOS's does); counting works either way. A writer's rows still in its -wal count, a cache
// without the table has 0, a -wal without its -shm is unknown (-1), and a path with % ? or # in it still opens
#include "../../src/winemetal/unix/cache_count.h"
#include <stdlib.h>
#include <sys/stat.h>

static unsigned failures;

static void
check(int ok, const char *what) {
  if (!ok)
    failures++, printf("failed: %s\n", what);
}

static int
exists(const char *file, const char *suffix) {
  char path[4096];
  struct stat st;
  snprintf(path, sizeof(path), "%s%s", file, suffix);
  return !stat(path, &st);
}

// a cache as CacheWriter makes it: WAL, table cache_<version>, `rows` rows; left open when `writer` is given
static void
make(const char *file, unsigned version, unsigned rows, sqlite3 **writer) {
  static unsigned key;
  sqlite3 *db;
  char sql[128];
  sqlite3_open_v2(file, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
  snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS cache_%u (key BLOB PRIMARY KEY, value BLOB);", version);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  for (unsigned i = 0; i < rows; i++) {
    snprintf(sql, sizeof(sql), "INSERT INTO cache_%u VALUES (x'%08x', x'00');", version, key++);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
  }
  if (writer)
    *writer = db;
  else
    sqlite3_close(db);
}

// what is beside the cache: 1 its -wal, 2 its -shm
static int
sidecars(const char *file) {
  return exists(file, "-wal") | exists(file, "-shm") << 1;
}

// the sidecars of a cache no connection has open, whose -wal is in the file
static void
drop(const char *file) {
  char path[4096];
  snprintf(path, sizeof(path), "%s-wal", file);
  unlink(path);
  snprintf(path, sizeof(path), "%s-shm", file);
  unlink(path);
}

int
main(void) {
  char directory[] = "/tmp/host_cache_count.XXXXXX", file[512];
  if (!mkdtemp(directory))
    return printf("no temporary directory\n"), 2;

  snprintf(file, sizeof(file), "%s/shaders_400.db", directory);
  make(file, 103, 5, NULL);
  int kept = sidecars(file);
  printf("a closed cache keeps %s\n", kept == 3 ? "its -wal and -shm" : kept ? "one of -wal and -shm" : "neither");
  check(cache_count(file, 103) == (kept == 1 || kept == 2 ? -1 : 5), "five rows as the cache was closed");
  check(sidecars(file) == kept, "counting made nothing beside the closed cache");
  drop(file);
  check(cache_count(file, 103) == 5, "five rows without -wal and -shm");
  check(sidecars(file) == 0, "counting made no -wal or -shm");
  check(cache_count(file, 104) == 0 && sidecars(file) == 0, "no rows for another AIRCONV_VERSION");

  sqlite3 *writer;
  make(file, 103, 2, &writer);
  check(sidecars(file) == 3, "the open writer keeps its -wal and -shm");
  check(cache_count(file, 103) == 7, "the open writer's rows count");
  sqlite3_close(writer);
  drop(file);

  char wal[600];
  snprintf(wal, sizeof(wal), "%s-wal", file);
  fclose(fopen(wal, "w"));
  check(cache_count(file, 103) == -1 && sidecars(file) == 1, "a -wal without its -shm: unknown, and nothing made");
  unlink(wal);

  char odd[512];
  snprintf(odd, sizeof(odd), "%s/a%%25 b?c#d.db", directory);
  make(odd, 103, 3, NULL);
  drop(odd);
  check(cache_count(odd, 103) == 3, "a path with % ? and # in it");

  char missing[512];
  snprintf(missing, sizeof(missing), "%s/missing.db", directory);
  check(cache_count(missing, 103) == -1 && !exists(missing, ""), "a missing file isn't made");

  char command[600];
  snprintf(command, sizeof(command), "rm -rf '%s'", directory);
  if (system(command))
    printf("left %s\n", directory);
  printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
