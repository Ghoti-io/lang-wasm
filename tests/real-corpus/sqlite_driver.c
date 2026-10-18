/* SPDX-License-Identifier: LGPL-3.0-only */
#include <sqlite3.h>
#include <stdint.h>
#include <string.h>
#include "corpus_result.h"

/* SQLite requires a lower VFS during initialization even when the workload
 * uses only :memory:. Register a deliberately unusable, syscall-free VFS;
 * SQLite's built-in memdb VFS replaces it as the default during initialization.
 * Any accidental attempt to open a host file fails closed. */
static int corpus_vfs_open(sqlite3_vfs *vfs, const char *name,
    sqlite3_file *file, int flags, int *out_flags) {
  (void)vfs; (void)name; (void)file; (void)flags; (void)out_flags;
  return SQLITE_CANTOPEN;
}
static int corpus_vfs_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
  (void)vfs; (void)name; (void)sync_dir; return SQLITE_IOERR_DELETE;
}
static int corpus_vfs_access(sqlite3_vfs *vfs, const char *name, int flags,
    int *result) {
  (void)vfs; (void)name; (void)flags; *result = 0; return SQLITE_OK;
}
static int corpus_vfs_full_path(sqlite3_vfs *vfs, const char *name,
    int capacity, char *out) {
  (void)vfs;
  size_t length = strlen(name);
  if (capacity <= 0 || length >= (size_t)capacity) return SQLITE_CANTOPEN;
  memcpy(out, name, length + 1);
  return SQLITE_OK;
}
static void *corpus_vfs_dl_open(sqlite3_vfs *vfs, const char *name) {
  (void)vfs; (void)name; return 0;
}
static void corpus_vfs_dl_error(sqlite3_vfs *vfs, int capacity, char *out) {
  (void)vfs;
  if (capacity > 0) out[0] = '\0';
}
static void (*corpus_vfs_dl_sym(sqlite3_vfs *vfs, void *handle,
    const char *symbol))(void) {
  (void)vfs; (void)handle; (void)symbol; return 0;
}
static void corpus_vfs_dl_close(sqlite3_vfs *vfs, void *handle) {
  (void)vfs; (void)handle;
}
static int corpus_vfs_randomness(sqlite3_vfs *vfs, int count, char *out) {
  (void)vfs;
  if (count > 0) memset(out, 0, (size_t)count);
  return count;
}
static int corpus_vfs_sleep(sqlite3_vfs *vfs, int microseconds) {
  (void)vfs; (void)microseconds; return 0;
}
static int corpus_vfs_time(sqlite3_vfs *vfs, double *out) {
  (void)vfs; *out = 2451545.0; return SQLITE_OK;
}

static sqlite3_vfs corpus_vfs = {
  .iVersion = 1,
  .szOsFile = sizeof(sqlite3_file),
  .mxPathname = 512,
  .zName = "corpus-no-host-io",
  .xOpen = corpus_vfs_open,
  .xDelete = corpus_vfs_delete,
  .xAccess = corpus_vfs_access,
  .xFullPathname = corpus_vfs_full_path,
  .xDlOpen = corpus_vfs_dl_open,
  .xDlError = corpus_vfs_dl_error,
  .xDlSym = corpus_vfs_dl_sym,
  .xDlClose = corpus_vfs_dl_close,
  .xRandomness = corpus_vfs_randomness,
  .xSleep = corpus_vfs_sleep,
  .xCurrentTime = corpus_vfs_time
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&corpus_vfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }

uint64_t corpus_run(void) {
  corpus_result_reset();
  sqlite3 *db = 0;
  sqlite3_stmt *statement = 0;
  int rc = sqlite3_open(":memory:", &db);
  if (rc != SQLITE_OK) goto fail;
  rc = sqlite3_exec(db,
      "CREATE TABLE readings(id INTEGER PRIMARY KEY, label TEXT, value INTEGER);"
      "INSERT INTO readings VALUES"
      "(1,'alpha',13),(2,'beta',-7),(3,'gamma',29),(4,'delta',0);",
      0, 0, 0);
  if (rc != SQLITE_OK) goto fail;
  rc = sqlite3_prepare_v2(db,
      "SELECT id,label,value FROM readings WHERE id >= ?1 ORDER BY id DESC",
      -1, &statement, 0);
  if (rc != SQLITE_OK) goto fail;
  if (sqlite3_bind_int(statement, 1, 2) != SQLITE_OK) goto fail;
  while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
    const unsigned char *label = sqlite3_column_text(statement, 1);
    const int length = sqlite3_column_bytes(statement, 1);
    if (length < 0 || !label ||
        !corpus_result_append_u64((uint64_t)sqlite3_column_int64(statement, 0)) ||
        !corpus_result_append_u32((uint32_t)length) ||
        !corpus_result_append(label, (uint32_t)length) ||
        !corpus_result_append_u64((uint64_t)sqlite3_column_int64(statement, 2)))
      goto fail;
  }
  if (rc != SQLITE_DONE) goto fail;
  rc = sqlite3_finalize(statement);
  statement = 0;
  if (rc != SQLITE_OK) goto fail;
  if (sqlite3_close(db) != SQLITE_OK) {
    sqlite3_close_v2(db);
    corpus_result_reset();
    return 0;
  }
  corpus_result_valid = 1;
  return 1;
fail:
  if (statement) {
    sqlite3_finalize(statement);
    statement = 0;
  }
  if (db) sqlite3_close_v2(db);
  corpus_result_reset();
  return 0;
}
