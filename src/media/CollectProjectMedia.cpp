#include "CollectProjectMedia.h"
#include "CollectMediaFiles.h"
#include "../video/CreateVideoTrack.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectFileIO.h"
#include "ProjectWindow.h"
#include "CommandContext.h"
#include "CommonCommandFlags.h"
#include "MenuRegistry.h"
#include "SettingsVisitor.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/progdlg.h>
#include <wx/textdlg.h>
#include <wx/msgdlg.h>
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <map>
#include <set>

namespace ProjectMedia {
namespace fs = std::filesystem;
namespace {
using SourceMap = std::map<std::string, std::string>;
// This is a serialization capability scoped to one GUI operation, not a new
// project model or saved execution record. SaveCopy's native serializer remains
// the sole writer of audio data, labels and all standard project properties.
std::map<const AudacityProject*, const SourceMap*> collectionSources;
std::string Utf8(const wxString &value) { const auto text = value.ToUTF8(); return {text.data(), text.length()}; }
class Digest final {
   BCRYPT_ALG_HANDLE algorithm{};
   BCRYPT_HASH_HANDLE hash{};
public:
   Digest() {
      if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
         throw std::runtime_error("Windows could not open its SHA-256 provider.");
      if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
         BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
         throw std::runtime_error("Windows could not create the media fingerprint.");
      }
   }
   ~Digest() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
   void Add(const char *data, size_t size) {
      while (size) {
         const auto count = static_cast<ULONG>(std::min<size_t>(size, 1024 * 1024));
         if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)), count, 0) < 0)
            throw std::runtime_error("Windows could not fingerprint the media bytes.");
         data += count; size -= count;
      }
   }
   std::string Finish() {
      std::array<unsigned char, 32> bytes{};
      if (BCryptFinishHash(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0)
         throw std::runtime_error("Windows could not finish the media fingerprint.");
      constexpr auto digits = "0123456789abcdef";
      std::string value;
      for (const auto byte : bytes) { value += digits[byte >> 4]; value += digits[byte & 15]; }
      return value;
   }
};
class DestinationLock final {
   HANDLE handle{INVALID_HANDLE_VALUE};
public:
   explicit DestinationLock(const fs::path &destination) {
      auto path = destination.parent_path() / fs::u8path("_" + destination.filename().u8string() + ".collection.lock");
      // Delete-on-close releases a killed worker's lock without progress files.
      // CREATE_NEW refuses a foreign file instead of deleting it on close.
      handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
      if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("This project folder is already being collected or is not writable.");
   }
   ~DestinationLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
class CollectionSources final {
   const AudacityProject *project;
public:
   CollectionSources(const AudacityProject &value, const SourceMap &sources) : project(&value) {
      if (!collectionSources.emplace(project, &sources).second)
         throw std::runtime_error("This project is already being collected.");
   }
   ~CollectionSources() { collectionSources.erase(project); }
};
}
std::string OpenVideoSourcePath(const AudacityProject *project, const std::string &path, const std::string &localPath)
{
   if (!project || path.empty()) return path;
   const auto projectPath = ProjectFileIO::Get(*project).GetFileName();
   if (!projectPath.empty() && !localPath.empty() && fs::u8path(localPath).is_relative()) {
      const auto local = (fs::path(projectPath.ToStdWstring()).parent_path() / fs::u8path(localPath)).lexically_normal();
      if (fs::is_regular_file(local)) {
         // Keep an unchanged absolute spelling stable across save/reopen. The
         // local hint wins only when the collection actually moved or differs.
         std::error_code error;
         if (fs::u8path(path).is_absolute() && fs::equivalent(local, fs::u8path(path), error) && !error) return path;
         return local.generic_u8string();
      }
   }
   if (fs::u8path(path).is_absolute()) return path;
   if (projectPath.empty()) throw std::runtime_error("Relative video media requires a saved project location.");
   return (fs::path(projectPath.ToStdWstring()).parent_path() / fs::u8path(path)).lexically_normal().u8string();
}
std::string StoreVideoSourcePath(const AudacityProject *project, const std::string &path)
{
   if (!project || path.empty()) return path;
   if (const auto found = collectionSources.find(project); found != collectionSources.end()) {
      const auto mapped = found->second->find(path);
      if (mapped == found->second->end()) throw std::runtime_error("The project gained a media source during collection.");
      return mapped->second;
   }
   return path;
}
std::string StoreLocalVideoSourcePath(const AudacityProject *project, const std::string &path)
{
   if (!project || path.empty()) return {};
   if (const auto found = collectionSources.find(project); found != collectionSources.end()) {
      const auto mapped = found->second->find(path);
      return mapped == found->second->end() ? std::string{} : mapped->second;
   }
   const auto projectPath = ProjectFileIO::Get(*project).GetFileName();
   if (projectPath.empty()) return {};
   const auto base = fs::path(projectPath.ToStdWstring()).parent_path();
   const auto relative = fs::u8path(path).lexically_normal().lexically_relative(base.lexically_normal());
   if (!relative.empty() && *relative.begin() != "..") return relative.generic_u8string();
   return {};
}

namespace {
class CollectProjectMediaCommand final : public AudacityCommand {
   wxString mDirectory;
   bool mInteractive{};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Save a portable project copy with its referenced video files."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mDirectory, wxT("Filename"), wxString{}); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      mInteractive = true;
      wxDirDialog dialog{&ProjectWindow::Get(project),
         _("Choose where to save the portable project folder."), {}, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST};
      if (dialog.ShowModal() != wxID_OK) return false;
      wxTextEntryDialog name{&ProjectWindow::Get(project),
         _("Name of the new folder. It will contain Project.aup3 and its Media folder."),
         _("Collect Project Media"), _("AuTopsy project")};
      if (name.ShowModal() != wxID_OK) return false;
      const auto leaf = fs::path(name.GetValue().ToStdWstring());
      if (leaf.empty() || leaf.filename() != leaf || leaf == "." || leaf == "..") return false;
      mDirectory = wxString((fs::path(dialog.GetPath().ToStdWstring()) / leaf).wstring()); return true;
   }
   bool Apply(const CommandContext &context) override {
      try {
         auto &project = context.project;
         if (ProjectAudioIO::Get(project).IsAudioActive()) throw std::runtime_error("Stop playback or recording before collecting project media.");
         if (mDirectory.empty()) throw std::runtime_error("Choose a new collection folder.");
         const auto destination = fs::absolute(fs::path(mDirectory.ToStdWstring())).lexically_normal();
         if (!fs::is_directory(destination.parent_path())) throw std::runtime_error("Choose a folder inside an existing parent.");
         DestinationLock lock(destination);
         ClipActions::MediaDialogCommandGuard commands;
         wxProgressDialog progress{_("Collect Project Media"), _("Checking referenced files..."), 1000,
            &ProjectWindow::Get(project), wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME};
         auto pulse = [&] {
            if (!progress.Pulse()) throw std::runtime_error("Media collection was cancelled. The original project is unchanged.");
         };
         const auto fingerprint = [&](const fs::path &path) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error("Cannot read a project or media file: " + path.u8string());
            std::vector<char> bytes(1024 * 1024); Digest digest;
            while (stream) {
               stream.read(bytes.data(), bytes.size());
               digest.Add(bytes.data(), static_cast<size_t>(stream.gcount())); pulse();
            }
            if (!stream.eof()) throw std::runtime_error("A file could not be read completely.");
            return digest.Finish();
         };
         std::set<std::string> sources;
         for (const auto *track : VideoTimeline::ProjectVideoTracks(project)) {
            if (!track->Timeline().sourcePath.empty()) sources.insert(track->Timeline().sourcePath);
            for (const auto &clip : track->Timeline().clips)
               for (const auto &source : VideoTimeline::VideoContentSources(clip, VideoTimeline::SourceFor(track->Timeline(), clip)))
                  sources.insert(source.path);
         }
         CollectionRequest request; request.destination = destination;
         SourceMap paths;
         std::map<std::string, std::string> names;
         for (const auto &source : sources) {
            const auto path = fs::u8path(source);
            const auto hash = fingerprint(path);
            auto inserted = names.emplace(hash, hash + path.extension().u8string());
            paths.emplace(source, "Media/" + inserted.first->second);
            request.files.push_back({path, inserted.first->second, hash});
         }
         CollectionSources serialization(project, paths);
         const auto doc = Utf8(ProjectFileIO::Get(project).GenerateDoc());
         Digest identity; identity.Add(doc.data(), doc.size()); request.documentIdentity = identity.Finish();
         const auto copy = CollectMediaFiles(request, fingerprint, [&](const fs::path &target) {
            pulse();
            if (!ProjectFileIO::Get(project).SaveCopy(wxString(target.wstring())))
               throw std::runtime_error("The project copy could not be saved. No collection was published.");
         }, [&](const char *, const fs::path &, bool) { pulse(); });
         context.Status(wxString(copy.wstring()));
         if (mInteractive) wxMessageBox(_("Portable copy saved here:\n") + wxString(copy.wstring()) +
            _("\n\nMove or back up the whole folder, including Media. Your original project is still open."),
            _("Collect Project Media"), wxOK | wxICON_INFORMATION, &ProjectWindow::Get(project));
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol CollectProjectMediaCommand::Symbol{XO("CollectProjectMedia")};
BuiltinCommandsModule::Registration<CollectProjectMediaCommand> sCollectMedia;
using namespace MenuRegistry;
AttachedItem sCollectMenu{Section("CollectProjectMedia", Command("CollectProjectMedia", XXO("Collect Project Media..."),
   CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())), wxT("File/Save")};
}
}
