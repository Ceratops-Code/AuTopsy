/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AUTOPSY_CONFIGURE_PROJECT_STORAGE_H
#define AUTOPSY_CONFIGURE_PROJECT_STORAGE_H

struct sqlite3;

namespace autopsy::project_storage {

enum class StorageMode {
   LiveProject,
   StandaloneCopy
};

// Use SQLite's own durable commit protocols, without changing the AUP3 schema.
// Call outside a transaction. On success, the selected journal and sync modes
// have also been read back; unsupported storage must not silently weaken them.
// Live projects retain WAL for background checkpoints. Copies use a rollback
// journal so a successful commit leaves one self-contained project file.
int ConfigureProjectStorage(sqlite3* db, const char* schema, StorageMode mode);

}
#endif
