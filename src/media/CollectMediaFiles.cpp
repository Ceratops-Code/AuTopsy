#include "CollectMediaFiles.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <stdexcept>

namespace ProjectMedia {
namespace fs = std::filesystem;
namespace {
constexpr auto Signature = "AuTopsy collected project 1";
constexpr auto ProjectName = "Project.aup3";
constexpr auto ManifestName = "_collection.txt";
struct Manifest {
   std::string target, document, projectDigest;
   std::map<std::string, std::string> files;
};
std::string Text(const fs::path &path) { return path.generic_u8string(); }
Manifest ReadManifest(const fs::path &path)
{
   std::ifstream stream(path, std::ios::binary);
   std::string signature; std::getline(stream, signature);
   Manifest value;
   if (signature != Signature || !(stream >> std::quoted(value.target) >> std::quoted(value.document)
       >> std::quoted(value.projectDigest))) throw std::runtime_error("The collection folder has another owner or an incomplete manifest.");
   std::string name, hash;
   while (stream >> std::quoted(name)) {
      if (!(stream >> std::quoted(hash)) || !value.files.emplace(name, hash).second)
         throw std::runtime_error("The collection manifest is incomplete or ambiguous.");
   }
   if (!stream.eof()) throw std::runtime_error("The collection manifest is unreadable.");
   return value;
}
void WriteManifest(const fs::path &stage, const Manifest &value)
{
   const auto pending = stage / "_collection.pending";
   std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
   stream << Signature << '\n' << std::quoted(value.target) << '\n' << std::quoted(value.document)
      << '\n' << std::quoted(value.projectDigest) << '\n';
   for (const auto &file : value.files) stream << std::quoted(file.first) << ' ' << std::quoted(file.second) << '\n';
   stream.close();
   if (!stream) throw std::runtime_error("Cannot finish the collection manifest.");
   // The completed owner record is never removed first. Native replacement of
   // the staging record is handled with a second name so interruption keeps it.
   const auto current = stage / ManifestName;
   const auto old = stage / "_collection.previous";
   if (fs::exists(current)) fs::rename(current, old);
   fs::rename(pending, current);
   fs::remove(old);
}
void RemoveStaging(const fs::path &stage, const fs::path &parent, const std::string &target)
{
   if (!fs::exists(stage)) return;
   if (fs::is_symlink(stage) || fs::weakly_canonical(stage).parent_path() != parent)
      throw std::runtime_error("Collection cleanup must stay inside its selected parent folder.");
   const auto current = stage / ManifestName;
   const auto old = stage / "_collection.previous";
   if (fs::exists(current) || fs::exists(old)) {
      if (ReadManifest(fs::exists(current) ? current : old).target != target)
         throw std::runtime_error("The unfinished collection belongs to another destination.");
   }
   else for (const auto &entry : fs::directory_iterator(stage)) {
      if (entry.path().filename() != "_collection.pending" || !entry.is_regular_file())
         throw std::runtime_error("The unfinished folder is not an owned collection workspace.");
   }
   fs::remove_all(stage);
}
}

fs::path CollectMediaFiles(const CollectionRequest &request, const CollectionFingerprint &fingerprint,
   const std::function<void(const fs::path&)> &saveProject, const CollectionObserver &observe)
{
   if (request.destination.empty() || request.documentIdentity.empty())
      throw std::runtime_error("Choose a new project folder and a complete project snapshot.");
   const auto destination = fs::absolute(request.destination).lexically_normal();
   if (destination.filename().empty() || destination.filename() == "." || destination.filename() == ".." ||
       !fs::is_directory(destination.parent_path())) throw std::runtime_error("Choose a new folder inside an existing parent.");
   const auto parent = fs::canonical(destination.parent_path());
   const auto target = Text(parent / destination.filename());
   const auto stage = parent / fs::u8path("_" + Text(destination.filename()) + ".collecting");
   for (const auto &file : request.files) {
      const auto relative = fs::weakly_canonical(file.source).lexically_relative(stage);
      if (!relative.empty() && *relative.begin() != "..")
         throw std::runtime_error("A collection workspace cannot be an original media source.");
   }
   RemoveStaging(stage, parent, target);
   Manifest desired{target, request.documentIdentity};
   for (const auto &file : request.files) {
      const auto name = fs::u8path(file.name);
      if (name.empty() || name.filename() != name || file.name == "." || file.name == ".." || file.digest.empty())
         throw std::runtime_error("Collected media names must be plain filenames with complete identities.");
      if (!fs::is_regular_file(file.source) || fingerprint(file.source) != file.digest)
         throw std::runtime_error("A referenced video is missing or changed. Relink it before collecting.");
      const auto existing = desired.files.emplace(file.name, file.digest);
      if (!existing.second && existing.first->second != file.digest)
         throw std::runtime_error("Different video sources cannot share one collected filename.");
   }
   if (fs::exists(destination)) {
      if (fs::is_symlink(destination) || !fs::is_directory(destination))
         throw std::runtime_error("The destination already exists. Choose a new project folder.");
      const auto actual = ReadManifest(destination / ManifestName);
      if (actual.document != desired.document || actual.files != desired.files || actual.projectDigest.empty())
         throw std::runtime_error("This folder contains a different project version. Choose a new folder to keep both.");
      if (fingerprint(destination / ProjectName) != actual.projectDigest)
         throw std::runtime_error("The collected project changed. Choose a new folder rather than overwriting it.");
      for (const auto &file : actual.files)
         if (fingerprint(destination / "Media" / fs::u8path(file.first)) != file.second)
            throw std::runtime_error("Collected media changed or is missing. Choose a new folder.");
      return destination / ProjectName;
   }
   const auto effect = [&](const char *name, const fs::path &path, const auto &action) {
      if (observe) observe(name, path, false);
      action();
      if (observe) observe(name, path, true);
   };
   try {
      effect("create-workspace", stage, [&] { fs::create_directory(stage); });
      effect("write-owner", stage / ManifestName, [&] { WriteManifest(stage, {target, request.documentIdentity}); });
      effect("create-media-folder", stage / "Media", [&] { fs::create_directory(stage / "Media"); });
      for (const auto &file : request.files) {
         const auto output = stage / "Media" / fs::u8path(file.name);
         if (fs::exists(output)) continue;
         effect("copy-media", output, [&] {
            fs::copy_file(file.source, output, fs::copy_options::none);
            if (fingerprint(output) != file.digest) throw std::runtime_error("A video changed during copying; the collection was not published.");
         });
      }
      effect("save-project", stage / ProjectName, [&] { saveProject(stage / ProjectName); });
      desired.projectDigest = fingerprint(stage / ProjectName);
      if (desired.projectDigest.empty()) throw std::runtime_error("The collected project was not saved completely.");
      effect("write-manifest", stage / ManifestName, [&] { WriteManifest(stage, desired); });
      effect("publish-folder", destination, [&] { fs::rename(stage, destination); });
      return destination / ProjectName;
   }
   catch (...) {
      // A successful final rename is a completed archive even if a caller stops
      // immediately afterward. A fresh call reads the archive, never a step log.
      RemoveStaging(stage, parent, target);
      throw;
   }
}
}
