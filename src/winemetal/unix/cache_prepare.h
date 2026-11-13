#pragma once

// WMTCachePrepare's work (winemetal.h), in plain C over SQLite and POSIX, so that tests/host/cache_prepare.c runs it
// natively against a file it writes. Include winemetal.h first, for struct WMTCachePrepared.
//
// The table's work happens under the writers' lock (CacheWriter's `<file>-lock`, cache.c), with a connection of its
// own that opens the file as a writer does. Every writer takes that lock for each write, so no write lands between
// the drop and the count, and the count is the table's at that moment.

#include "sqlite3.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int64_t
cache_query_number(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int64_t value = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

// makes table cache_<version>, drops every other cache_* table in one transaction, and counts the table's rows
static void
cache_prepare_table(sqlite3 *db, uint64_t version, struct WMTCachePrepared *prepared) {
  char table[32];
  snprintf(table, sizeof(table), "cache_%016llx", (unsigned long long)version);
  char *sql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS %s (key BLOB PRIMARY KEY, value BLOB NOT NULL);", table);
  bool made = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
  sqlite3_free(sql);
  // after the table is made, which may take free pages, so that the drop's count is the pages it freed
  int64_t page = cache_query_number(db, "PRAGMA page_size;");
  int64_t free_before = cache_query_number(db, "PRAGMA freelist_count;");
  if (made && sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) == SQLITE_OK) {
    uint64_t dropped = 0;
    bool failed = false;
    sqlite3_stmt *other;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name GLOB 'cache_*' AND "
                               "name != ?1 LIMIT 1;", -1, &other, NULL) != SQLITE_OK)
      failed = true;
    else
      sqlite3_bind_text(other, 1, table, -1, SQLITE_STATIC);
    while (!failed && sqlite3_step(other) == SQLITE_ROW) {
      sql = sqlite3_mprintf("DROP TABLE \"%w\";", (const char *)sqlite3_column_text(other, 0));
      sqlite3_reset(other);
      failed = sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK;
      sqlite3_free(sql);
      dropped += !failed;
    }
    sqlite3_finalize(other);
    if (!failed && sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK) {
      prepared->dropped_tables = dropped;
      int64_t free_after = cache_query_number(db, "PRAGMA freelist_count;");
      if (page > 0 && free_before >= 0 && free_after >= free_before)
        prepared->dropped_bytes = (uint64_t)(free_after - free_before) * (uint64_t)page;
    } else {
      fprintf(stderr, "mullion: cache other tables not dropped: %s\n", sqlite3_errmsg(db));
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
  }
  sql = sqlite3_mprintf("SELECT COUNT(*) FROM %s;", table);
  prepared->entries = cache_query_number(db, sql);
  sqlite3_free(sql);
}

// removes the regular files of `directory` named <shard_family>*.metallib but not <shard_keep>*
static void
cache_remove_shards(const char *directory, const char *shard_family, const char *shard_keep,
                    struct WMTCachePrepared *prepared) {
  DIR *dir = opendir(directory);
  if (!dir)
    return;
  size_t family = strlen(shard_family), keep = strlen(shard_keep), suffix = strlen(".metallib");
  for (struct dirent *entry; (entry = readdir(dir));) {
    const char *name = entry->d_name;
    size_t length = strlen(name);
    if (strncmp(name, shard_family, family) || !strncmp(name, shard_keep, keep) || length < suffix ||
        strcmp(name + length - suffix, ".metallib"))
      continue;
    char *file = sqlite3_mprintf("%s/%s", directory, name);
    struct stat st;
    if (file && !lstat(file, &st) && S_ISREG(st.st_mode) && !unlink(file)) {
      prepared->removed_shards++;
      prepared->removed_bytes += (uint64_t)st.st_size;
    }
    sqlite3_free(file);
  }
  closedir(dir);
}

// WMTCachePrepare on a resolved path; `prepared` starts as the thunk leaves it (entries and bytes -1, the rest 0)
static void
cache_prepare(const char *db_path, uint64_t version, const char *shard_family, const char *shard_keep,
              struct WMTCachePrepared *prepared) {
  char *lock_path = sqlite3_mprintf("%s-lock", db_path);
  int lock = lock_path ? open(lock_path, O_RDWR | O_CREAT, 0666) : -1;
  sqlite3_free(lock_path);
  while (lock >= 0 && flock(lock, LOCK_EX))
    if (errno != EINTR) {
      close(lock);
      lock = -1;
    }
  if (lock >= 0) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL) ==
        SQLITE_OK) {
      sqlite3_busy_timeout(db, 2000);
      sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
      sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
      cache_prepare_table(db, version, prepared);
    }
    sqlite3_close(db);
    close(lock);
  }
  struct stat st;
  if (!stat(db_path, &st))
    prepared->bytes = st.st_size;

  char *directory = strdup(db_path);
  char *slash = directory ? strrchr(directory, '/') : NULL;
  if (slash) {
    *slash = 0;
    cache_remove_shards(*directory ? directory : "/", shard_family, shard_keep, prepared);
  }
  free(directory);
}
