#pragma once

// The session header's count of a shader cache (cache.c, cache_stats): the rows of table cache_<version> in `file`, 0
// without that table, -1 when they can't be counted without writing.
//
// It writes nothing, and opens nothing for writing. Any connection to a WAL database (the writer's mode), read-only
// too, opens its -shm for writing, rebuilds it, and makes the -wal and -shm when they aren't there
// (https://sqlite.org/wal.html, "Read-Only Databases"). So the file opens immutable: SQLite reads the file alone, with
// no -wal, -shm or lock. That is right while no writer changes the file, so it counts under CacheWriter's own lock
// (`<file>-lock`, which a writer holds exclusively while it has the cache open): opened read-only and never made,
// taken shared without waiting. A writer that has it is -1, and one can't start meanwhile. Without a lock file, one
// that appears during the count is -1 as well. Rows a writer left in a -wal without checkpointing them (it closed
// while another process still had the cache open) aren't counted.
//
// Plain C and SQLite: tests/host/cache_count.c builds it natively.

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include "sqlite3.h"

static inline int64_t
cache_count(const char *file, uint64_t version) {
  char lock_path[4096], uri[3 * 4096 + 32], query[64];
  if ((size_t)snprintf(lock_path, sizeof(lock_path), "%s-lock", file) >= sizeof(lock_path))
    return -1;
  int lock = open(lock_path, O_RDONLY | O_CLOEXEC);
  if (lock < 0 && errno != ENOENT)
    return -1;
  if (lock >= 0 && flock(lock, LOCK_SH | LOCK_NB)) {
    close(lock);
    return -1;
  }
  // the file as a URI: % ? and # escaped, which would otherwise end its path. it fits: the file is shorter than 4092
  size_t n = (size_t)snprintf(uri, sizeof(uri), "file:");
  for (const char *c = file; *c; c++)
    n += (size_t)snprintf(uri + n, sizeof(uri) - n, strchr("%?#", *c) ? "%%%02X" : "%c", (unsigned char)*c);
  n += (size_t)snprintf(uri + n, sizeof(uri) - n, "?immutable=1");
  snprintf(query, sizeof(query), "SELECT COUNT(*) FROM cache_%llu;", (unsigned long long)version);
  int64_t count = -1;
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  if (n < sizeof(uri) &&
      sqlite3_open_v2(uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK) {
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int64(stmt, 0);
    } else if (!strncmp(sqlite3_errmsg(db), "no such table", 13)) {
      // a cache written for another AIRCONV_VERSION only
      count = 0;
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (lock >= 0)
    close(lock);
  else if (!access(lock_path, F_OK) || errno != ENOENT)
    return -1;
  return count;
}
