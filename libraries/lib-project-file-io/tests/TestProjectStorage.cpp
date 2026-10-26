/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <catch2/catch.hpp>
#include "../ConfigureProjectStorage.h"
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace {
using autopsy::project_storage::ConfigureProjectStorage;
using autopsy::project_storage::StorageMode;

// SQLite's documented VFS interface lets the test separate writes handed to
// the OS from bytes made durable by xSync. A cut rejects subsequent I/O and
// discards every unsynced write. Unlike killing an app, this also loses OS cache.
// It is deliberately single-threaded: production checkpoint scheduling and
// real storage firmware are outside this test's guarantees.
struct DiskFile {
   std::vector<unsigned char> bytes, durable;
   std::vector<std::unique_ptr<unsigned char[]>> shared;
   bool exists{false}, durableExists{false};
};
struct PowerCutDisk;
struct OpenFile {
   sqlite3_file base;
   PowerCutDisk* disk{};
   DiskFile* file{};
   int flags{};
};

struct PowerCutDisk {
   sqlite3_vfs vfs{};
   std::map<std::string, DiskFile> files;
   bool powered{true}, cutAfter{false};
   int interruptAt{}, effects{}, temporary{};
   std::vector<std::string> events;
   const char* name{"autopsy-project-storage-test"};

   PowerCutDisk();
   ~PowerCutDisk() { sqlite3_vfs_unregister(&vfs); }
   int Effect(const char* operation, const std::function<void()>& action) {
      if (!powered)
         return SQLITE_IOERR;
      events.emplace_back(operation);
      ++effects;
      if (interruptAt == effects && !cutAfter) {
         powered = false;
         return SQLITE_IOERR;
      }
      action();
      if (interruptAt == effects && cutAfter) {
         powered = false;
         return SQLITE_IOERR;
      }
      return SQLITE_OK;
   }
   void Arm(int index, bool after) {
      interruptAt = index; cutAfter = after; effects = 0; events.clear();
   }
   void RestoreDurableBytes() {
      for (auto& item : files) {
         auto& file = item.second;
         file.bytes = file.durable;
         file.exists = file.durableExists;
         file.shared.clear();
      }
      powered = true; Arm(0, false);
   }
};

OpenFile& File(sqlite3_file* file) { return *reinterpret_cast<OpenFile*>(file); }
int Close(sqlite3_file* raw) {
   auto& f = File(raw);
   if (f.flags & SQLITE_OPEN_DELETEONCLOSE)
      f.file->exists = false;
   f.base.pMethods = nullptr;
   return SQLITE_OK;
}
int Read(sqlite3_file* raw, void* buffer, int size, sqlite3_int64 offset) {
   auto& f = File(raw);
   if (!f.disk->powered)
      return SQLITE_IOERR_READ;
   const auto start = static_cast<size_t>(offset);
   const auto available = start < f.file->bytes.size()
      ? std::min<size_t>(size, f.file->bytes.size() - start) : 0;
   if (available)
      std::memcpy(buffer, f.file->bytes.data() + start, available);
   if (available < static_cast<size_t>(size)) {
      std::memset(static_cast<unsigned char*>(buffer) + available, 0, size - available);
      return SQLITE_IOERR_SHORT_READ;
   }
   return SQLITE_OK;
}
int Write(sqlite3_file* raw, const void* buffer, int size, sqlite3_int64 offset) {
   auto& f = File(raw);
   return f.disk->Effect("write", [&] {
      const auto start = static_cast<size_t>(offset);
      if (start + size > f.file->bytes.size())
         f.file->bytes.resize(start + size);
      std::memcpy(f.file->bytes.data() + start, buffer, size);
   });
}
int Truncate(sqlite3_file* raw, sqlite3_int64 size) {
   auto& f = File(raw);
   return f.disk->Effect("truncate", [&] { f.file->bytes.resize(static_cast<size_t>(size)); });
}
int Sync(sqlite3_file* raw, int) {
   auto& f = File(raw);
   return f.disk->Effect("sync", [&] {
      f.file->durable = f.file->bytes;
      f.file->durableExists = f.file->exists;
   });
}
int Size(sqlite3_file* raw, sqlite3_int64* size) {
   auto& f = File(raw);
   if (!f.disk->powered)
      return SQLITE_IOERR_FSTAT;
   *size = f.file->bytes.size(); return SQLITE_OK;
}
int Lock(sqlite3_file*, int) { return SQLITE_OK; }
int Reserved(sqlite3_file*, int* held) { *held = 0; return SQLITE_OK; }
int Control(sqlite3_file*, int, void*) { return SQLITE_NOTFOUND; }
int Sector(sqlite3_file*) { return 512; }
int Characteristics(sqlite3_file*) { return 0; }
int SharedMap(sqlite3_file* raw, int page, int size, int extend, void volatile** out) {
   auto& f = File(raw);
   if (!f.disk->powered)
      return SQLITE_IOERR_SHMMAP;
   auto& regions = f.file->shared;
   if (static_cast<size_t>(page) >= regions.size()) {
      if (!extend) { *out = nullptr; return SQLITE_OK; }
      regions.resize(page + 1);
   }
   if (!regions[page])
      regions[page] = std::make_unique<unsigned char[]>(size);
   *out = regions[page].get(); return SQLITE_OK;
}
int SharedLock(sqlite3_file*, int, int, int) { return SQLITE_OK; }
void SharedBarrier(sqlite3_file*) {}
int SharedUnmap(sqlite3_file*, int) { return SQLITE_OK; }
const sqlite3_io_methods methods{
   2, Close, Read, Write, Truncate, Sync, Size, Lock, Lock, Reserved, Control,
   Sector, Characteristics, SharedMap, SharedLock, SharedBarrier, SharedUnmap,
   nullptr, nullptr
};
PowerCutDisk& Disk(sqlite3_vfs* vfs) {
   return *static_cast<PowerCutDisk*>(vfs->pAppData);
}
int Open(sqlite3_vfs* vfs, const char* path, sqlite3_file* raw, int flags, int* actual) {
   auto& disk = Disk(vfs);
   raw->pMethods = nullptr;
   if (!disk.powered)
      return SQLITE_CANTOPEN;
   auto key = path ? std::string(path) : "temporary-" + std::to_string(++disk.temporary);
   auto& state = disk.files[key];
   if (!state.exists && !(flags & SQLITE_OPEN_CREATE))
      return SQLITE_CANTOPEN;
   state.exists = true;
   auto& f = File(raw);
   f.disk = &disk; f.file = &state; f.flags = flags; f.base.pMethods = &methods;
   if (actual)
      *actual = flags;
   return SQLITE_OK;
}
int Delete(sqlite3_vfs* vfs, const char* name, int syncDirectory) {
   auto& disk = Disk(vfs);
   return disk.Effect("delete", [&] {
      auto& file = disk.files[name]; file.exists = false;
      if (syncDirectory)
         file.durableExists = false;
   });
}
int Access(sqlite3_vfs* vfs, const char* name, int, int* exists) {
   auto it = Disk(vfs).files.find(name);
   *exists = it != Disk(vfs).files.end() && it->second.exists;
   return SQLITE_OK;
}
int FullPath(sqlite3_vfs*, const char* name, int length, char* out) {
   if (std::strlen(name) >= static_cast<size_t>(length))
      return SQLITE_CANTOPEN;
   std::strcpy(out, name); return SQLITE_OK;
}
int Random(sqlite3_vfs*, int size, char* bytes) {
   std::memset(bytes, 0x37, size); return size;
}
int Sleep(sqlite3_vfs*, int) { return 0; }
int Time(sqlite3_vfs*, double* time) { *time = 2460000; return SQLITE_OK; }
PowerCutDisk::PowerCutDisk() {
   REQUIRE(sqlite3_initialize() == SQLITE_OK);
   vfs.iVersion = 1; vfs.szOsFile = sizeof(OpenFile); vfs.mxPathname = 1024;
   vfs.zName = name; vfs.pAppData = this; vfs.xOpen = Open; vfs.xDelete = Delete;
   vfs.xAccess = Access; vfs.xFullPathname = FullPath; vfs.xRandomness = Random;
   vfs.xSleep = Sleep; vfs.xCurrentTime = Time;
   REQUIRE(sqlite3_vfs_register(&vfs, 0) == SQLITE_OK);
}

struct Database {
   sqlite3* handle{};
   Database(PowerCutDisk& disk, const char* path = "project.aup3") {
      REQUIRE(sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
         disk.name) == SQLITE_OK);
   }
   ~Database() { if (handle) sqlite3_close_v2(handle); }
   void Close() { REQUIRE(sqlite3_close_v2(handle) == SQLITE_OK); handle = nullptr; }
   int Execute(const char* sql) { return sqlite3_exec(handle, sql, nullptr, nullptr, nullptr); }
   std::string Value(const char* sql) {
      sqlite3_stmt* statement = nullptr;
      const auto rc = sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr);
      INFO("SQL: " << sql << "; SQLite: " << sqlite3_errmsg(handle));
      REQUIRE(rc == SQLITE_OK);
      REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
      const auto text = sqlite3_column_text(statement, 0);
      std::string value = text ? reinterpret_cast<const char*>(text) : "";
      REQUIRE(sqlite3_finalize(statement) == SQLITE_OK);
      return value;
   }
};
const char* seed =
   "CREATE TABLE project(id INTEGER PRIMARY KEY,dict BLOB,doc BLOB);"
   "CREATE TABLE sampleblocks(blockid INTEGER PRIMARY KEY,samples BLOB);"
   "INSERT INTO sampleblocks VALUES(1,X'01020304');"
   "INSERT INTO project VALUES(1,X'01','1');";
const char* edit =
   "BEGIN; INSERT OR REPLACE INTO sampleblocks VALUES(2,X'05060708');"
   "UPDATE project SET doc='1,2' WHERE id=1; COMMIT;";
void Prepare(Database& db, StorageMode mode) {
   REQUIRE(ConfigureProjectStorage(db.handle, "main", mode) == SQLITE_OK);
   REQUIRE(db.Execute(seed) == SQLITE_OK);
}
void CheckProject(Database& db, bool requireEdit) {
   REQUIRE(db.Value("PRAGMA integrity_check;") == "ok");
   REQUIRE(db.Value("SELECT hex(samples) FROM sampleblocks WHERE blockid=1") == "01020304");
   auto doc = db.Value("SELECT doc FROM project WHERE id=1");
   REQUIRE((doc == "1" || doc == "1,2"));
   if (requireEdit)
      REQUIRE(doc == "1,2");
   if (doc == "1,2")
      REQUIRE(db.Value("SELECT hex(samples) FROM sampleblocks WHERE blockid=2") == "05060708");
}
void CutAndReopen(PowerCutDisk& disk, Database& db,
   const std::function<void(Database&)>& check) {
   std::string state;
   for (const auto& item : disk.files)
      state += item.first + " live=" + std::to_string(item.second.bytes.size())
         + " durable=" + std::to_string(item.second.durable.size())
         + " exists=" + std::to_string(item.second.durableExists) + "; ";
   INFO("Before power loss: " << state);
   disk.powered = false;
   db.Close();
   disk.RestoreDurableBytes();
   Database reopened(disk);
   check(reopened);
}
}

TEST_CASE("Committed live edits survive loss of every unsynced write", "[project-storage]") {
   PowerCutDisk disk; Database db(disk);
   Prepare(db, StorageMode::LiveProject);
   REQUIRE(db.Execute(edit) == SQLITE_OK);
   REQUIRE(db.Execute("UPDATE project SET dict=X'020304';") == SQLITE_OK);
   CutAndReopen(disk, db, [](Database& reopened) {
      CheckProject(reopened, true);
      REQUIRE(reopened.Value("SELECT hex(dict) FROM project") == "020304");
   });
}

TEST_CASE("Power-cut model exposes the previous NORMAL durability loss", "[project-storage]") {
   PowerCutDisk disk; Database db(disk);
   Prepare(db, StorageMode::LiveProject);
   REQUIRE(db.Execute("PRAGMA synchronous=NORMAL;") == SQLITE_OK);
   REQUIRE(db.Execute(edit) == SQLITE_OK);
   CutAndReopen(disk, db, [](Database& reopened) {
      CheckProject(reopened, false);
      REQUIRE(reopened.Value("SELECT doc FROM project") == "1");
   });
}

TEST_CASE("An interrupted later edit cannot lose previously acknowledged clip blocks", "[project-storage]") {
   // Several acknowledged transactions and page-sized, distinct blocks model a
   // real arrangement rather than a single saved row. Only its final two clips
   // change in the interrupted edit; every older clip must remain byte-exact.
   const auto change = std::string("BEGIN; DELETE FROM sampleblocks WHERE blockid>4;")
      + "INSERT INTO sampleblocks VALUES(7,CAST('" + std::string(8192, 'z') + "' AS BLOB));"
      + "UPDATE project SET doc='1,2,3,4,7'; COMMIT;";
   for (const auto mode : {StorageMode::LiveProject, StorageMode::StandaloneCopy}) {
      const auto populate = [mode](Database& db) {
         Prepare(db, mode);
         std::string document = "1";
         for (int id = 2; id <= 6; ++id) {
            document += "," + std::to_string(id);
            const auto sql = "BEGIN; INSERT INTO sampleblocks VALUES(" + std::to_string(id)
               + ",CAST('" + std::string(8192, static_cast<char>('a' + id)) + "' AS BLOB));"
               + "UPDATE project SET doc='" + document + "'; COMMIT;";
            REQUIRE(db.Execute(sql.c_str()) == SQLITE_OK);
         }
         std::vector<std::string> stable;
         for (int id = 1; id <= 4; ++id)
            stable.push_back(db.Value(("SELECT hex(samples) FROM sampleblocks WHERE blockid="
               + std::to_string(id)).c_str()));
         return stable;
      };
      int effects{};
      {
         PowerCutDisk disk; Database db(disk); populate(db); disk.Arm(0, false);
         REQUIRE(db.Execute(change.c_str()) == SQLITE_OK);
         effects = disk.effects;
      }
      REQUIRE(effects > 0);
      for (int at = 1; at <= effects; ++at) {
         for (const bool after : {false, true}) {
            CAPTURE(static_cast<int>(mode), at, after);
            PowerCutDisk disk; Database db(disk);
            const auto expected = populate(db);
            disk.Arm(at, after);
            const auto status = db.Execute(change.c_str());
            CutAndReopen(disk, db, [&](Database& reopened) {
               REQUIRE(reopened.Value("PRAGMA integrity_check") == "ok");
               for (int id = 1; id <= 4; ++id)
                  REQUIRE(reopened.Value(("SELECT hex(samples) FROM sampleblocks WHERE blockid="
                     + std::to_string(id)).c_str()) == expected[id - 1]);
               const auto document = reopened.Value("SELECT doc FROM project");
               REQUIRE((document == "1,2,3,4,5,6" || document == "1,2,3,4,7"));
               if (status == SQLITE_OK) REQUIRE(document == "1,2,3,4,7");
               REQUIRE(reopened.Value("SELECT count(*) FROM sampleblocks")
                  == (document == "1,2,3,4,7" ? "5" : "6"));
               if (document == "1,2,3,4,7")
                  REQUIRE(reopened.Value("SELECT length(samples) FROM sampleblocks WHERE blockid=7") == "8192");
               REQUIRE(ConfigureProjectStorage(reopened.handle, "main", mode) == SQLITE_OK);
               for (int repeat = 0; repeat != 2; ++repeat) {
                  REQUIRE(reopened.Execute(change.c_str()) == SQLITE_OK);
                  REQUIRE(reopened.Value("SELECT doc FROM project") == "1,2,3,4,7");
                  REQUIRE(reopened.Value("SELECT count(*) FROM sampleblocks") == "5");
               }
            });
         }
      }
   }
}

TEST_CASE("Edits remain atomic across every interrupted write and sync", "[project-storage]") {
   for (auto mode : {StorageMode::LiveProject, StorageMode::StandaloneCopy}) {
      int count{};
      {
         PowerCutDisk disk; Database db(disk); Prepare(db, mode); disk.Arm(0, false);
         REQUIRE(db.Execute(edit) == SQLITE_OK); count = disk.effects;
      }
      REQUIRE(count > 0);
      for (int at = 1; at <= count; ++at) {
         for (bool after : {false, true}) {
            CAPTURE(static_cast<int>(mode), at, after);
            PowerCutDisk disk; Database db(disk); Prepare(db, mode); disk.Arm(at, after);
            const auto rc = db.Execute(edit);
            REQUIRE_FALSE(disk.powered);
            CutAndReopen(disk, db, [mode, rc](Database& reopened) {
               CheckProject(reopened, rc == SQLITE_OK);
               REQUIRE(ConfigureProjectStorage(reopened.handle, "main", mode) == SQLITE_OK);
               REQUIRE(reopened.Execute(edit) == SQLITE_OK);
               REQUIRE(reopened.Execute(edit) == SQLITE_OK);
               CheckProject(reopened, true);
            });
         }
      }
   }
}

TEST_CASE("A copy commit reports an interrupted storage effect", "[project-storage]") {
   PowerCutDisk disk; Database db(disk); Prepare(db, StorageMode::StandaloneCopy);
   REQUIRE(db.Execute("BEGIN; INSERT INTO sampleblocks VALUES(2,X'05060708');"
      "UPDATE project SET doc='1,2';") == SQLITE_OK);
   disk.Arm(1, false);
   REQUIRE(db.Execute("COMMIT;") != SQLITE_OK);
   REQUIRE_FALSE(disk.powered);
   CutAndReopen(disk, db, [](Database& reopened) { CheckProject(reopened, false); });
}

TEST_CASE("Checkpoint interruption preserves all acknowledged edits", "[project-storage]") {
   int count{};
   {
      PowerCutDisk disk; Database db(disk); Prepare(db, StorageMode::LiveProject);
      REQUIRE(db.Execute(edit) == SQLITE_OK); disk.Arm(0, false);
      REQUIRE(sqlite3_wal_checkpoint_v2(db.handle, "main", SQLITE_CHECKPOINT_TRUNCATE,
         nullptr, nullptr) == SQLITE_OK);
      count = disk.effects;
   }
   REQUIRE(count > 0);
   for (int at = 1; at <= count; ++at) {
      for (bool after : {false, true}) {
         CAPTURE(at, after);
         PowerCutDisk disk; Database db(disk); Prepare(db, StorageMode::LiveProject);
         REQUIRE(db.Execute(edit) == SQLITE_OK); disk.Arm(at, after);
         const auto rc = sqlite3_wal_checkpoint_v2(db.handle, "main", SQLITE_CHECKPOINT_TRUNCATE,
            nullptr, nullptr);
         REQUIRE_FALSE(disk.powered);
         REQUIRE(rc != SQLITE_OK);
         CutAndReopen(disk, db, [](Database& reopened) { CheckProject(reopened, true); });
      }
   }
}

TEST_CASE("Attached copy settings do not weaken the live source", "[project-storage]") {
   PowerCutDisk disk; Database db(disk); Prepare(db, StorageMode::LiveProject);
   REQUIRE(db.Execute("ATTACH 'copy.aup3' AS outbound;") == SQLITE_OK);
   for (int repeat = 0; repeat != 2; ++repeat)
      REQUIRE(ConfigureProjectStorage(db.handle, "outbound", StorageMode::StandaloneCopy) == SQLITE_OK);
   REQUIRE(db.Value("PRAGMA main.synchronous") == "2");
   REQUIRE(db.Value("PRAGMA main.journal_mode") == "wal");
   REQUIRE(db.Value("PRAGMA outbound.synchronous") == "3");
   REQUIRE(db.Value("PRAGMA outbound.journal_mode") == "delete");
   REQUIRE(db.Execute("CREATE TABLE outbound.project AS SELECT * FROM main.project;") == SQLITE_OK);
   REQUIRE(db.Execute("DETACH outbound;") == SQLITE_OK);
   REQUIRE_FALSE(disk.files["copy.aup3-journal"].exists);
   REQUIRE_FALSE(disk.files["copy.aup3-wal"].exists);
   REQUIRE(db.Value("SELECT doc FROM main.project") == "1");
}

TEST_CASE("Storage setup rejects unsupported modes and active transactions", "[project-storage]") {
   PowerCutDisk disk; Database db(disk); Prepare(db, StorageMode::LiveProject);
   REQUIRE(ConfigureProjectStorage(nullptr, "main", StorageMode::LiveProject) == SQLITE_MISUSE);
   REQUIRE(ConfigureProjectStorage(db.handle, nullptr, StorageMode::LiveProject) == SQLITE_MISUSE);
   REQUIRE(ConfigureProjectStorage(db.handle, "", StorageMode::LiveProject) == SQLITE_MISUSE);
   REQUIRE(ConfigureProjectStorage(db.handle, "missing", StorageMode::LiveProject) != SQLITE_OK);
   REQUIRE(db.Execute("BEGIN;") == SQLITE_OK);
   REQUIRE(ConfigureProjectStorage(db.handle, "main", StorageMode::StandaloneCopy) == SQLITE_MISUSE);
   REQUIRE(db.Execute("ROLLBACK;") == SQLITE_OK);
   REQUIRE(ConfigureProjectStorage(db.handle, "main", StorageMode::LiveProject) == SQLITE_OK);
   CheckProject(db, false);
   sqlite3* memory{};
   REQUIRE(sqlite3_open(":memory:", &memory) == SQLITE_OK);
   const auto rc = ConfigureProjectStorage(memory, "main", StorageMode::LiveProject);
   REQUIRE(sqlite3_close(memory) == SQLITE_OK);
   REQUIRE(rc != SQLITE_OK);
}

namespace {
struct PrivateSaveBoundary {
   static unsigned long ProcessId() {
#ifdef _WIN32
      return GetCurrentProcessId();
#else
      return static_cast<unsigned long>(getpid());
#endif
   }
   static bool ProcessRunning(unsigned long pid) {
#ifdef _WIN32
      const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
      if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
      const bool running = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
      CloseHandle(process);
      return running;
#else
      return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
   }
   static void RemoveAbandonedRoots() {
      // The CTest working directory owns only this prefix. Preserve every live
      // PID, ambiguous owner or redirected path, including a reused PID.
      const auto parent = std::filesystem::current_path();
      const std::string prefix = "_save-boundary-";
      for (const auto& item : std::filesystem::directory_iterator(parent)) {
         const auto name = item.path().filename().string();
         if (name.rfind(prefix, 0) != 0 || item.is_symlink() || !item.is_directory()) continue;
         const auto end = name.find('-', prefix.size());
         if (end == std::string::npos) continue;
         const auto id = name.substr(prefix.size(), end - prefix.size());
         if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) continue;
         unsigned long pid{};
         try { pid = std::stoul(id); }
         catch (const std::exception&) { continue; }
         if (!pid || ProcessRunning(pid)) continue;
         std::ifstream marker(item.path() / "_owner");
         const std::string signature{std::istreambuf_iterator<char>(marker), {}};
         marker.close();
         if (signature == "AuTopsy behavior tests 1" || std::filesystem::is_empty(item.path())) {
            if (std::filesystem::weakly_canonical(item.path()).parent_path() == parent)
               std::filesystem::remove_all(item.path());
         }
      }
   }
   std::filesystem::path caseRoot = std::filesystem::current_path() /
      ("_save-boundary-" + std::to_string(ProcessId()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   std::filesystem::path audio = caseRoot / "_audio_temp";
   sqlite3* db{};
   std::optional<std::string> previous;
   bool ownsRoot{};

   static void SetRoot(const std::string& value) {
#ifdef _WIN32
      _putenv_s("AUTOPSY_TEST_SAVE_ROOT", value.c_str());
#else
      if (value.empty()) unsetenv("AUTOPSY_TEST_SAVE_ROOT");
      else setenv("AUTOPSY_TEST_SAVE_ROOT", value.c_str(), 1);
#endif
   }
   PrivateSaveBoundary() {
      if (const auto value = std::getenv("AUTOPSY_TEST_SAVE_ROOT")) previous = value;
      try {
         RemoveAbandonedRoots();
         if (!(ownsRoot = std::filesystem::create_directory(caseRoot)))
            throw std::runtime_error("Private save test directory already exists.");
         std::ofstream(caseRoot / "_owner") << "AuTopsy behavior tests 1";
         std::filesystem::create_directory(audio);
         if (sqlite3_open((audio / "test.aup3").u8string().c_str(), &db) != SQLITE_OK)
            throw std::runtime_error("Private save test database did not open.");
         SetRoot(audio.u8string());
      }
      catch (...) { Cleanup(); throw; }
   }
   void Cleanup() {
      if (db) { sqlite3_close(db); db = nullptr; }
      SetRoot(previous.value_or(""));
      std::error_code error;
      if (ownsRoot && std::filesystem::weakly_canonical(caseRoot, error) == caseRoot)
         std::filesystem::remove_all(caseRoot, error);
   }
   ~PrivateSaveBoundary() { Cleanup(); }
};
}

TEST_CASE("Private save barriers repeat both boundaries and remove completed signals", "[project-storage]") {
   using autopsy::project_storage::AwaitProjectSaveTestBoundary;
   PrivateSaveBoundary test;
   for (const auto phase : {"before", "after", "before", "after"}) {
      std::ofstream(test.audio / "_save_boundary_arm") << phase;
      const auto other = std::string(phase) == "before" ? "after" : "before";
      AwaitProjectSaveTestBoundary(test.db, other);
      REQUIRE_FALSE(std::filesystem::exists(test.audio / "_save_boundary_ready"));
      auto worker = std::async(std::launch::async, [&] { AwaitProjectSaveTestBoundary(test.db, phase); });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!std::filesystem::exists(test.audio / "_save_boundary_ready") &&
             std::chrono::steady_clock::now() < deadline)
         std::this_thread::sleep_for(std::chrono::milliseconds(5));
      const bool observed = std::filesystem::exists(test.audio / "_save_boundary_ready");
      // Always release before assertions, so a failed observation cannot strand
      // its worker or the SQLite handle that the fixture must close.
      // Publish only a closed file: Windows refuses the worker's removal while
      // the writer still owns its handle. A pending file is never a signal.
      const auto pending = test.audio / "_save_boundary_release.pending";
      std::ofstream signal(pending, std::ios::binary);
      signal << "continue";
      signal.flush();
      const bool waitingForPublication =
         worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
      signal.close();
      if (!signal)
         throw std::runtime_error("Cannot write the private save release signal.");
      std::filesystem::rename(pending, test.audio / "_save_boundary_release");
      worker.get();
      REQUIRE(observed);
      REQUIRE(waitingForPublication);
      REQUIRE_FALSE(std::filesystem::exists(pending));
      REQUIRE_FALSE(std::filesystem::exists(test.audio / "_save_boundary_ready"));
      REQUIRE_FALSE(std::filesystem::exists(test.audio / "_save_boundary_arm"));
      REQUIRE_FALSE(std::filesystem::exists(test.audio / "_save_boundary_release"));
   }
}

TEST_CASE("Save barriers refuse foreign ownership and leave ordinary projects untouched", "[project-storage]") {
   using autopsy::project_storage::AwaitProjectSaveTestBoundary;
   PrivateSaveBoundary test;
   for (int repeat{}; repeat < 2; ++repeat) {
      PrivateSaveBoundary::SetRoot("");
      REQUIRE_NOTHROW(AwaitProjectSaveTestBoundary(test.db, "before"));
      PrivateSaveBoundary::SetRoot((test.caseRoot / "unrelated").u8string());
      REQUIRE_NOTHROW(AwaitProjectSaveTestBoundary(test.db, "before"));
      PrivateSaveBoundary::SetRoot(test.audio.u8string());
      REQUIRE_NOTHROW(AwaitProjectSaveTestBoundary(test.db, "before"));
      std::ofstream(test.caseRoot / "_owner", std::ios::trunc) << "different owner";
      REQUIRE_THROWS(AwaitProjectSaveTestBoundary(test.db, "before"));
      std::ofstream(test.caseRoot / "_owner", std::ios::trunc) << "AuTopsy behavior tests 1";
      std::filesystem::create_directory(test.audio / "_save_boundary_arm");
      REQUIRE_THROWS(AwaitProjectSaveTestBoundary(test.db, "before"));
      std::filesystem::remove(test.audio / "_save_boundary_arm");
      REQUIRE_FALSE(std::filesystem::exists(test.audio / "_save_boundary_ready"));
   }
}
