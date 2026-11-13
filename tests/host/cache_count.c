// contract: the session header's cache count (src/winemetal/unix/cache_count.h) counts a WAL cache's rows without
// writing anything: no -wal or -shm file appears beside it, which SQLite makes for a read-only connection to a WAL
// database that has none (https://sqlite.org/wal.html, "Read-Only Databases"). 0 without the table, -1 while a writer
// has the cache open (its -wal holds rows the file doesn't), and a path with % ? or # in it still opens
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

int
main(void) {
  char directory[] = "/tmp/host_cache_count.XXXXXX", file[512];
  if (!mkdtemp(directory))
    return printf("no temporary directory\n"), 2;

  snprintf(file, sizeof(file), "%s/shaders_400.db", directory);
  make(file, 103, 5, NULL);
  check(!exists(file, "-wal") && !exists(file, "-shm"), "the closed cache has no -wal or -shm");
  check(cache_count(file, 103) == 5, "five rows");
  check(!exists(file, "-wal") && !exists(file, "-shm"), "counting made no -wal or -shm");
  check(cache_count(file, 104) == 0, "no rows for another AIRCONV_VERSION");

  sqlite3 *writer;
  make(file, 103, 2, &writer);
  check(exists(file, "-wal"), "the open writer keeps a -wal");
  check(cache_count(file, 103) == -1, "unknown while a writer has it open");
  sqlite3_close(writer);
  check(cache_count(file, 103) == 7, "seven rows once it closed");

  char odd[512];
  snprintf(odd, sizeof(odd), "%s/a%%25 b?c#d.db", directory);
  make(odd, 103, 3, NULL);
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
