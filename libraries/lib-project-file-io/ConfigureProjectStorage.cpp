/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ConfigureProjectStorage.h"

#include <sqlite3.h>

namespace autopsy::project_storage {
namespace {
int CheckPragma(sqlite3* db, const char* schema, const char* pragma,
   const char* expected)
{
   auto sql = sqlite3_mprintf("PRAGMA \"%w\".%s;", schema, pragma);
   if (!sql)
      return SQLITE_NOMEM;
   sqlite3_stmt* statement = nullptr;
   auto rc = sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
   sqlite3_free(sql);
   if (rc == SQLITE_OK) {
      rc = sqlite3_step(statement);
      if (rc == SQLITE_ROW) {
         const auto value = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0));
         rc = value && sqlite3_stricmp(value, expected) == 0
            ? SQLITE_OK : SQLITE_ERROR;
      }
      else if (rc == SQLITE_DONE)
         rc = SQLITE_ERROR;
   }
   const auto finalRC = sqlite3_finalize(statement);
   return rc == SQLITE_OK ? finalRC : rc;
}
}

int ConfigureProjectStorage(sqlite3* db, const char* schema, StorageMode mode)
{
   if (!db || !schema || !*schema || !sqlite3_get_autocommit(db))
      return SQLITE_MISUSE;

   const bool live = mode == StorageMode::LiveProject;
   if (!live && mode != StorageMode::StandaloneCopy)
      return SQLITE_MISUSE;
   auto rc = sqlite3_busy_timeout(db, 5000);
   if (rc != SQLITE_OK)
      return rc;

   // NORMAL in WAL mode can lose several acknowledged transactions after an
   // OS crash. FULL syncs every commit, including autosaves and sample blocks.
   // EXTRA also protects the rollback-journal transaction that first switches
   // a database into WAL: its deleted journal must not reappear after a cut.
   // Copies remain batched in one transaction; disabling the journal is not
   // necessary to avoid a separate disk sync for every copied audio block.
   auto sql = sqlite3_mprintf(
      "PRAGMA \"%w\".locking_mode=NORMAL;"
      "PRAGMA \"%w\".synchronous=%s;"
      "PRAGMA \"%w\".journal_mode=%s;",
      schema, schema, "EXTRA",
      schema, live ? "WAL" : "DELETE");
   if (!sql)
      return SQLITE_NOMEM;
   rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
   sqlite3_free(sql);
   if (rc == SQLITE_OK && live) {
      sql = sqlite3_mprintf("PRAGMA \"%w\".synchronous=FULL;", schema);
      if (!sql)
         return SQLITE_NOMEM;
      rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
      sqlite3_free(sql);
   }
   if (rc == SQLITE_OK)
      rc = CheckPragma(db, schema, "journal_mode", live ? "wal" : "delete");
   if (rc == SQLITE_OK)
      rc = CheckPragma(db, schema, "synchronous", live ? "2" : "3");
   if (rc == SQLITE_OK && live)
      rc = sqlite3_exec(db, "PRAGMA wal_autocheckpoint=0;",
         nullptr, nullptr, nullptr);
   return rc;
}
}
