#pragma once

// The session header's count of a shader cache (cache.c, cache_stats): the rows of table cache_<version> in `file`, 0
// without that table, -1 when they can't be counted without writing. A read-only connection to a WAL database (the
// writer's mode) makes its -wal and -shm files when they aren't there (https://sqlite.org/wal.html, "Read-Only
// Databases"), so the file is opened immutable, which makes and locks nothing, and only while it has no -wal file: a
// writer that has it open keeps one, and the rows it holds aren't in the file yet. A -wal that appears while it counts
// (a writer that opened meanwhile, which may checkpoint into the file) makes the count unknown too.
//
// Plain C and SQLite: tests/host/cache_count.c builds it natively.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "sqlite3.h"

static inline int64_t
cache_count(const char *file, uint64_t version) {
  char path[4096], uri[3 * sizeof(path) + 32], query[64];
  if ((size_t)snprintf(path, sizeof(path), "%s-wal", file) >= sizeof(path))
    return -1;
  if (!access(path, F_OK) || errno != ENOENT)
    return -1;
  // the file as a URI: % ? and # escaped, which would otherwise end its path. it fits: the file is shorter than `path`
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
  if (!access(path, F_OK) || errno != ENOENT)
    return -1;
  return count;
}
