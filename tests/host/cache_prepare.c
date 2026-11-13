// contract: when a shader cache opens (WMTCachePrepare, src/winemetal/unix/cache_prepare.h), every cache_* table but
// its own identity's is dropped, once, and what they took is counted; its own table's rows are counted under the
// writers' lock, so a write that holds the lock is counted; and the pipeline archives of other identities of the same
// Metal version are removed, nothing else of the directory
#define WINEMETAL_API
#include "../../src/winemetal/winemetal.h"
#include "../../src/winemetal/unix/cache_prepare.h"
#include <stdio.h>
#include <sys/wait.h>

static unsigned failures = 0;

static void
check(bool ok, const char *what) {
  if (!ok)
    failures++, printf("failed: %s\n", what);
}

#define TAG 0x0123456789abcdefull
#define TABLE "cache_0123456789abcdef"

static void
sql(sqlite3 *db, const char *text) {
  char *error = NULL;
  if (sqlite3_exec(db, text, NULL, NULL, &error) != SQLITE_OK)
    printf("failed: %s: %s\n", text, error), failures++;
  sqlite3_free(error);
}

// `rows` rows of 1 KiB in `table`
static void
fill(sqlite3 *db, const char *table, int rows) {
  char *text = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\" (key BLOB PRIMARY KEY, value BLOB NOT NULL);"
                               "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < %d) "
                               "INSERT INTO \"%w\" SELECT randomblob(20), zeroblob(1024) FROM n;",
                               table, rows, table);
  sql(db, text);
  sqlite3_free(text);
}

static int64_t
tables(sqlite3 *db) {
  return cache_query_number(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table';");
}

static void
touch(const char *directory, const char *name, int bytes) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", directory, name);
  FILE *f = fopen(path, "w");
  for (int i = 0; f && i < bytes; i++)
    fputc('x', f);
  if (f)
    fclose(f);
}

static bool
exists(const char *directory, const char *name) {
  char path[512];
  struct stat st;
  snprintf(path, sizeof(path), "%s/%s", directory, name);
  return !lstat(path, &st);
}

static struct WMTCachePrepared
prepare(const char *db_path) {
  struct WMTCachePrepared prepared = {.entries = -1, .bytes = -1};
  cache_prepare(db_path, TAG, "pipelines_400_", "pipelines_400_0123456789abcdef_", &prepared);
  return prepared;
}

int
main(void) {
  char directory[] = "/tmp/mullion-cache-prepare-XXXXXX";
  if (!mkdtemp(directory))
    return 2;
  char db_path[512];
  snprintf(db_path, sizeof(db_path), "%s/shaders_400.db", directory);

  // a new file: the table made, nothing dropped
  struct WMTCachePrepared prepared = prepare(db_path);
  check(prepared.entries == 0 && prepared.dropped_tables == 0 && prepared.dropped_bytes == 0, "a new cache: empty");
  check(prepared.bytes > 0, "a new cache: its size");

  // the build before's tables (AIRCONV_VERSION 104's, and another id's) and this id's rows
  sqlite3 *db;
  sqlite3_open(db_path, &db);
  fill(db, "cache_104", 2048);
  fill(db, "cache_fedcba9876543210", 1024);
  fill(db, "cache_\"quoted", 1);
  fill(db, "not_a_cache", 1);
  fill(db, TABLE, 300);
  sqlite3_close(db);
  // archives: AIRCONV_VERSION's, another id's, this id's, another Metal version's, and names that aren't archives
  touch(directory, "pipelines_400_0.metallib", 4096);
  touch(directory, "pipelines_400_fedcba9876543210_0.metallib", 1000);
  touch(directory, "pipelines_400_0123456789abcdef_0.metallib", 10);
  touch(directory, "pipelines_310_0.metallib", 10);
  touch(directory, "pipelines_400_0.txt", 10);
  char sub[600];
  snprintf(sub, sizeof(sub), "%s/pipelines_400_1.metallib", directory);
  mkdir(sub, 0700);

  prepared = prepare(db_path);
  check(prepared.entries == 300, "this id's rows counted");
  check(prepared.dropped_tables == 3, "the other cache_ tables dropped");
  check(prepared.dropped_bytes >= (3072ull << 10), "what they took counted");
  sqlite3_open(db_path, &db);
  check(tables(db) == 2, "this id's table and the table that isn't a cache's are left");
  check(cache_query_number(db, "SELECT COUNT(*) FROM " TABLE ";") == 300, "this id's rows are left");
  sqlite3_close(db);
  check(prepared.removed_shards == 2 && prepared.removed_bytes == 5096, "the other ids' archives removed");
  check(!exists(directory, "pipelines_400_0.metallib") &&
            !exists(directory, "pipelines_400_fedcba9876543210_0.metallib"),
        "AIRCONV_VERSION's and the other id's archives are gone");
  check(exists(directory, "pipelines_400_0123456789abcdef_0.metallib") && exists(directory, "pipelines_310_0.metallib") &&
            exists(directory, "pipelines_400_0.txt") && exists(directory, "pipelines_400_1.metallib"),
        "this id's, another Metal version's and what isn't an archive are left");

  // once: the next open drops nothing
  prepared = prepare(db_path);
  check(prepared.dropped_tables == 0 && prepared.dropped_bytes == 0 && prepared.removed_shards == 0,
        "the next open drops nothing");

  // a writer that holds the lock: its row is counted, the count waits for it
  int ready[2];
  if (pipe(ready))
    return 2;
  pid_t child = fork();
  if (!child) {
    char lock_path[600];
    snprintf(lock_path, sizeof(lock_path), "%s-lock", db_path);
    int lock = open(lock_path, O_RDWR | O_CREAT, 0666);
    flock(lock, LOCK_EX);
    if (write(ready[1], "x", 1) != 1)
      _exit(1);
    usleep(300 * 1000);
    sqlite3 *writer;
    sqlite3_open(db_path, &writer);
    fill(writer, TABLE, 1);
    sqlite3_close(writer);
    flock(lock, LOCK_UN);
    _exit(failures != 0);
  }
  char byte;
  if (read(ready[0], &byte, 1) != 1)
    return 2;
  prepared = prepare(db_path);
  int status;
  waitpid(child, &status, 0);
  check(WIFEXITED(status) && !WEXITSTATUS(status), "the writer wrote");
  check(prepared.entries == 301, "a write under the lock is counted");

  // what a drop freed doesn't depend on whether this id's table was made from free pages: two files with the same
  // old table and free pages, one with this id's table made before it opens
  {
    uint64_t freed[2];
    for (int made = 0; made < 2; made++) {
      char path[600];
      snprintf(path, sizeof(path), "%s/shaders_free%d.db", directory, made);
      sqlite3_open(path, &db);
      fill(db, "cache_104", 64);
      fill(db, "scratch", 64);
      sql(db, "DROP TABLE scratch;");
      if (made)
        fill(db, TABLE, 0);
      sqlite3_close(db);
      prepared = prepare(path);
      freed[made] = prepared.dropped_bytes;
    }
    check(freed[0] > 0 && freed[0] == freed[1], "the pages a drop freed, whether or not making the table took some");
  }

  // a file that isn't a database: nothing counted, nothing removed but archives
  char bad[600];
  snprintf(bad, sizeof(bad), "%s/shaders_bad.db", directory);
  touch(directory, "shaders_bad.db", 100);
  prepared = (struct WMTCachePrepared){.entries = -1, .bytes = -1};
  cache_prepare(bad, TAG, "pipelines_400_", "pipelines_400_0123456789abcdef_", &prepared);
  check(prepared.entries == -1 && prepared.dropped_tables == 0, "a damaged file: n/a");

  char command[600];
  snprintf(command, sizeof(command), "rm -rf '%s'", directory);
  if (system(command))
    printf("left %s\n", directory);
  printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
