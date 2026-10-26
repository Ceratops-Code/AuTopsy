/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ConfigureProjectStorage.h"

#include <sqlite3.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

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

void AwaitProjectSaveTestBoundary(sqlite3* db, const char* phase)
{
   const auto requested = std::getenv("AUTOPSY_TEST_SAVE_ROOT");
   if (!requested || !*requested)
      return;
   namespace fs = std::filesystem;
   const auto root = fs::u8path(requested);
   const auto project = db ? sqlite3_db_filename(db, "main") : nullptr;
   // A test environment never authorizes pausing a different open project.
   if (!project || fs::u8path(project).parent_path() != root)
      return;
   if (!phase || (std::string(phase) != "before" && std::string(phase) != "after") ||
       !root.is_absolute() || root.filename() != "_audio_temp" ||
       fs::is_symlink(root) || fs::weakly_canonical(root) != root)
      throw std::runtime_error("Project save test boundary requires its private canonical audio directory.");
   const auto marker = root.parent_path() / "_owner";
   std::ifstream owner(marker);
   const std::string signature{std::istreambuf_iterator<char>(owner), {}};
   if (fs::is_symlink(marker) || signature != "AuTopsy behavior tests 1")
      throw std::runtime_error("Project save test boundary has a different case owner.");
   const auto arm = root / "_save_boundary_arm";
   if (!fs::exists(arm))
      return;
   if (fs::is_symlink(arm) || !fs::is_regular_file(arm))
      throw std::runtime_error("Project save test boundary refuses a redirected arm file.");
   std::ifstream input(arm);
   const std::string selected{std::istreambuf_iterator<char>(input), {}};
   input.close();
   if (selected != "before" && selected != "after")
      throw std::runtime_error("Project save test boundary requires before or after.");
   if (selected != phase)
      return;
   const auto ready = root / "_save_boundary_ready";
   const auto pending = root / "_save_boundary_ready.pending";
   const auto release = root / "_save_boundary_release";
   if (fs::exists(ready) || fs::exists(pending) || fs::exists(release))
      throw std::runtime_error("Project save test boundary contains an unresolved signal.");
   struct RemoveSignal {
      fs::path path, pending;
      ~RemoveSignal() { std::error_code error; fs::remove(path, error); fs::remove(pending, error); }
   } cleanup{ready, pending};
   {
      std::ofstream output(pending, std::ios::binary);
      output << phase;
      output.flush();
      if (!output)
         throw std::runtime_error("Cannot publish the private save boundary signal.");
   }
   fs::rename(pending, ready);
   // Signals are transient synchronization inside one live test, never restart
   // checkpoints. The test runner kills only its owned process at this point.
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
   while (!fs::exists(release)) {
      if (std::chrono::steady_clock::now() >= deadline)
         throw std::runtime_error("Private project save boundary timed out.");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
   }
   if (fs::is_symlink(release) || !fs::is_regular_file(release))
      throw std::runtime_error("Project save test boundary refuses a redirected release signal.");
   fs::remove(release);
   fs::remove(arm);
}
}
