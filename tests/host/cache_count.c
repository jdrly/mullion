// contract: the session header's cache count (src/winemetal/unix/cache_count.h) counts a WAL cache's rows without
// writing: the cache's file, its -wal, -shm and -lock are byte for byte what they were, and none appears. A read-only
// SQLite connection would make the -wal and -shm, or rewrite the -shm (https://sqlite.org/wal.html, "Read-Only
// Databases"). Whether a closed cache keeps its -wal and -shm is the platform's SQLite's (macOS's does); counting
// works either way. A writer that holds CacheWriter's lock makes the count unknown (-1); a cache without the table
// has 0; a path with % ? or # in it still opens
#include "../../src/winemetal/unix/cache_count.h"
#include <stdlib.h>
#include <sys/stat.h>

static unsigned failures;

static void
check(int ok, const char *what) {
  if (!ok)
    failures++, printf("failed: %s\n", what);
}

// the cache's files as they are: for each, whether it is there and an FNV-1a hash of its bytes
static uint64_t
snapshot(const char *file) {
  static const char *const suffixes[] = {"", "-wal", "-shm", "-lock"};
  uint64_t hash = 14695981039346656037ull;
  for (unsigned i = 0; i < 4; i++) {
    char path[4096];
    snprintf(path, sizeof(path), "%s%s", file, suffixes[i]);
    FILE *f = fopen(path, "rb");
    hash = (hash ^ (f != NULL)) * 1099511628211ull;
    for (int c; f && (c = fgetc(f)) != EOF;)
      hash = (hash ^ (unsigned char)c) * 1099511628211ull;
    if (f)
      fclose(f);
  }
  return hash;
}

static int
exists(const char *file, const char *suffix) {
  char path[4096];
  struct stat st;
  snprintf(path, sizeof(path), "%s%s", file, suffix);
  return !stat(path, &st);
}

// a cache as CacheWriter has it: its lock (`<file>-lock`, flock LOCK_EX), WAL, table cache_<version>, `rows` rows
// more; left open, with its lock, when `writer` is given
static void
make(const char *file, unsigned version, unsigned rows, sqlite3 **writer, int *lock) {
  static unsigned key;
  char path[4096], sql[128];
  snprintf(path, sizeof(path), "%s-lock", file);
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  flock(fd, LOCK_EX);
  sqlite3 *db;
  sqlite3_open_v2(file, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
  snprintf(sql, sizeof(sql), "CREATE TABLE IF NOT EXISTS cache_%u (key BLOB PRIMARY KEY, value BLOB);", version);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
  for (unsigned i = 0; i < rows; i++) {
    snprintf(sql, sizeof(sql), "INSERT INTO cache_%u VALUES (x'%08x', x'00');", version, key++);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
  }
  if (writer) {
    *writer = db, *lock = fd;
  } else {
    sqlite3_close(db);
    close(fd);
  }
}

// the cache's -wal and -shm once no connection has it open (its -wal is in the file)
static void
drop(const char *file) {
  char path[4096];
  snprintf(path, sizeof(path), "%s-wal", file);
  unlink(path);
  snprintf(path, sizeof(path), "%s-shm", file);
  unlink(path);
}

// the count, and whether the cache's files stayed as they were
static int64_t
count(const char *file, uint64_t version, const char *what) {
  uint64_t before = snapshot(file);
  int64_t n = cache_count(file, version);
  char text[256];
  snprintf(text, sizeof(text), "%s: counting wrote nothing", what);
  check(snapshot(file) == before, text);
  return n;
}

int
main(void) {
  char directory[] = "/tmp/host_cache_count.XXXXXX", file[512];
  if (!mkdtemp(directory))
    return printf("no temporary directory\n"), 2;

  snprintf(file, sizeof(file), "%s/shaders_400.db", directory);
  make(file, 103, 5, NULL, NULL);
  printf("a closed cache keeps %s\n", exists(file, "-wal") && exists(file, "-shm")   ? "its -wal and -shm"
                                      : exists(file, "-wal") || exists(file, "-shm") ? "one of -wal and -shm"
                                                                                     : "neither");
  check(count(file, 103, "the cache as closed") == 5, "five rows as the cache was closed");
  drop(file);
  check(count(file, 103, "no -wal or -shm") == 5, "five rows without -wal and -shm");
  check(count(file, 104, "another version") == 0, "no rows for another AIRCONV_VERSION");

  sqlite3 *writer;
  int lock;
  make(file, 103, 2, &writer, &lock);
  check(count(file, 103, "an open writer") == -1, "unknown while a writer holds the lock");
  sqlite3_close(writer);
  close(lock);
  check(count(file, 103, "the writer closed") == 7, "seven rows once the writer closed");

  char odd[512];
  snprintf(odd, sizeof(odd), "%s/a%%25 b?c#d.db", directory);
  make(odd, 103, 3, NULL, NULL);
  check(count(odd, 103, "an odd path") == 3, "a path with % ? and # in it");

  char bare[512], bare_lock[600];
  snprintf(bare, sizeof(bare), "%s/bare.db", directory);
  make(bare, 103, 4, NULL, NULL);
  drop(bare);
  snprintf(bare_lock, sizeof(bare_lock), "%s-lock", bare);
  unlink(bare_lock);
  check(count(bare, 103, "no lock file") == 4, "four rows in a cache without a lock file");

  char missing[512];
  snprintf(missing, sizeof(missing), "%s/missing.db", directory);
  check(count(missing, 103, "a missing file") == -1 && !exists(missing, ""), "a missing file isn't made");

  char command[600];
  snprintf(command, sizeof(command), "rm -rf '%s'", directory);
  if (system(command))
    printf("left %s\n", directory);
  printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
