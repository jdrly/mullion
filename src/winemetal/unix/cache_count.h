#pragma once

// The session header's count of a shader cache (cache.c, cache_stats): the rows of table cache_<version> in `file`, 0
// without that table, -1 when they can't be counted without writing a file. A read-only connection to a WAL database
// (the writer's mode) makes its -wal and -shm files when they aren't there (https://sqlite.org/wal.html, "Read-Only
// Databases"). So: with neither, the file opens immutable, which makes and locks nothing; with both (an open writer
// keeps them, and macOS's SQLite keeps them after it closes), it opens read-only, which then makes nothing and also
// counts the rows still in the -wal; with one of them, or one that appears while it counts (a writer that opened
// meanwhile, which may checkpoint into the file), the count is unknown.
//
// Plain C and SQLite: tests/host/cache_count.c builds it natively.

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "sqlite3.h"

// 1 if `file` with `suffix` is there, 0 if it isn't, -1 if that can't be told
static inline int
cache_count_exists(const char *file, const char *suffix) {
  char path[4096];
  if ((size_t)snprintf(path, sizeof(path), "%s%s", file, suffix) >= sizeof(path))
    return -1;
  return !access(path, F_OK) ? 1 : errno == ENOENT ? 0 : -1;
}

static inline int64_t
cache_count(const char *file, uint64_t version) {
  int wal = cache_count_exists(file, "-wal"), shm = cache_count_exists(file, "-shm");
  if (wal < 0 || shm < 0 || wal != shm || strlen(file) >= 4096)
    return -1;
  bool immutable = !wal;
  char uri[3 * 4096 + 32], query[64];
  // the file as a URI: % ? and # escaped, which would otherwise end its path. it fits: the file is shorter than 4096
  size_t n = (size_t)snprintf(uri, sizeof(uri), "file:");
  for (const char *c = file; *c; c++)
    n += (size_t)snprintf(uri + n, sizeof(uri) - n, strchr("%?#", *c) ? "%%%02X" : "%c", (unsigned char)*c);
  n += (size_t)snprintf(uri + n, sizeof(uri) - n, immutable ? "?immutable=1" : "?mode=ro");
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
  if (immutable && (cache_count_exists(file, "-wal") || cache_count_exists(file, "-shm")))
    return -1;
  return count;
}
