// Bounded disposable previews and thumbnails, produced by the bundled FFmpeg.
// A completed marker publishes the pair atomically to consumers. Leases protect
// live entries; interrupted builds are discarded on startup or the next import.
#include "PrepareVideoPreview.h"
#include "ChoosePreviewQuality.h"
#include "ExportVideoMix.h"
#include "FileNames.h"
#include <wx/msw/wrapwin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace VideoPreview {
namespace {
namespace fs = std::filesystem;
constexpr auto Signature = "AuTopsy preview cache 1";
struct CloseHandleDeleter {
   void operator()(void *value) const {
      if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
   }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
struct Lease { Handle handle; };
std::map<fs::path, std::weak_ptr<const PreparedMedia>> liveMedia;
fs::path CacheRoot() { return fs::path(FileNames::DataDir().ToStdWstring()) / "_video-previews"; }
std::string Read(const fs::path &path) {
   std::ifstream input(path, std::ios::binary);
   return {std::istreambuf_iterator<char>(input), {}};
}
void Write(const fs::path &path, const std::string &value) {
   std::ofstream output(path, std::ios::binary | std::ios::trunc);
   output << value; output.flush();
   if (!output) throw std::runtime_error("Cannot write a video preview cache record.");
}
bool PlainDirectory(const fs::path &path) {
   const auto flags = GetFileAttributesW(path.c_str());
   return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY) &&
      !(flags & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool PlainFile(const fs::path &path) {
   const auto flags = GetFileAttributesW(path.c_str());
   return flags != INVALID_FILE_ATTRIBUTES &&
      !(flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
// Delete exact producer-owned names only. An unexpected file retains the entry
// rather than allowing a broad recursive delete of user or live work.
void RemoveEntry(const fs::path &path) {
   for (const auto *name : {"_building.mp4", "_building.jpg", "_preview.mp4",
            "_building.mov", "_preview.mov", "_building.png", "_preview.png", "_title.png", "_concat.txt",
           "_frames.jpg", "_complete", "_source", "_owner", "_lease"}) {
      std::error_code error;
      fs::remove(path / name, error);
   }
   std::error_code error; fs::remove(path, error);
}
std::string SourceKey(const std::string &source) {
   // A filename key, not a security digest. The complete source identity below
   // is compared before reuse, so a collision cannot select another picture.
   uint64_t hash = 14695981039346656037ULL;
   for (unsigned char c : source) { hash ^= c; hash *= 1099511628211ULL; }
   std::ostringstream text; text << std::hex << std::setw(16) << std::setfill('0') << hash;
   return text.str();
}
}

std::shared_ptr<const PreparedMedia> FindPreparedVideoThumbnails(const wxString &source,
   double duration, wxWindow *parent)
{
   for (const int height : {540, 360, 720, 1080})
      if (auto media = PrepareVideoMedia(source, duration, true, parent, false, height))
         return media;
   return PrepareVideoMedia(source, duration, false, parent, false);
}

void CleanPreviewCache()
{
   const auto root = CacheRoot();
   if (!fs::exists(root) || !PlainDirectory(root)) return;
   struct Entry { fs::path path; fs::file_time_type time; bool live; };
   std::vector<Entry> entries;
   for (const auto &child : fs::directory_iterator(root)) {
      const auto path = child.path();
      const auto name = path.filename().string();
      if (name.rfind("_preview-", 0) != 0 || name.size() < 27 || !PlainDirectory(path)) continue;
      const auto owner = path / "_owner";
      if (fs::exists(owner) && (!PlainFile(owner) || Read(owner) != Signature)) continue;
      if (fs::exists(path / "_lease") && !PlainFile(path / "_lease")) continue;
      Handle lease{CreateFileW((path / "_lease").c_str(), GENERIC_READ | GENERIC_WRITE,
         0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
      const bool live = lease.get() == INVALID_HANDLE_VALUE;
      if (!live && (!PlainFile(path / "_complete") || !PlainFile(path / "_source"))) {
         lease.reset(); RemoveEntry(path); continue;
      }
      std::error_code error;
      auto time = fs::last_write_time(path / "_complete", error);
      if (error) time = fs::file_time_type::clock::now();
      entries.push_back({path, time, live});
   }
   std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.time > b.time; });
   std::map<std::string, unsigned> versions;
   size_t retained{};
   for (const auto &entry : entries) {
      const auto group = entry.path.filename().string().substr(0, 25);
      const auto version = ++versions[group];
      if (entry.live || (version <= 3 && retained < 36)) { ++retained; continue; }
      Handle lease{CreateFileW((entry.path / "_lease").c_str(), GENERIC_READ | GENERIC_WRITE,
         0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
      if (lease.get() != INVALID_HANDLE_VALUE) { lease.reset(); RemoveEntry(entry.path); }
   }
   for (auto it = liveMedia.begin(); it != liveMedia.end();)
      if (it->second.expired()) it = liveMedia.erase(it); else ++it;
}

std::shared_ptr<const PreparedMedia> PrepareVideoMedia(const wxString &source,
   double duration, bool lightweight, wxWindow *parent, bool buildIfMissing, int maximumHeight)
{
   if (!std::isfinite(duration) || duration <= 0)
      throw std::runtime_error("Video preview requires a finite source duration.");
   const auto scaleFilter = PreviewQualityScaleFilter(maximumHeight);
   const auto sourcePath = fs::absolute(fs::path(source.ToStdWstring()));
   Handle sourceFile{CreateFileW(sourcePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
   BY_HANDLE_FILE_INFORMATION info{};
   if (sourceFile.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(sourceFile.get(), &info))
      throw std::runtime_error("Cannot read the source video for preview.");
   const auto sourceName = wxString(sourcePath.wstring());
   const auto utf8 = sourceName.ToUTF8();
   const std::string canonical{utf8.data(), utf8.length()};
   std::ostringstream revision;
   revision << std::hex << info.dwVolumeSerialNumber << '-' << info.nFileIndexHigh << '-'
      << info.nFileIndexLow << '-' << info.ftLastWriteTime.dwHighDateTime << '-'
      << info.ftLastWriteTime.dwLowDateTime << '-' << info.nFileSizeHigh << '-'
      << info.nFileSizeLow << '-' << int(lightweight);
   if (lightweight && maximumHeight != 540) revision << "-p" << std::dec << maximumHeight;
   const auto identity = canonical + '\n' + revision.str() + "\nrecipe=1";
   const auto root = CacheRoot();
   const auto path = root / ("_preview-" + SourceKey(canonical) + '-' + revision.str());
   if (const auto found = liveMedia[path].lock()) return found;
   if (!buildIfMissing && (!PlainDirectory(path) || !PlainFile(path / "_complete") ||
       Read(path / "_source") != identity || !PlainFile(path / "_frames.jpg") ||
       (lightweight && !PlainFile(path / "_preview.mp4")))) return {};
   fs::create_directories(root);
   if (!PlainDirectory(root)) throw std::runtime_error("The preview cache must be a plain directory.");
   CleanPreviewCache();
   fs::create_directory(path);
   if (!PlainDirectory(path)) throw std::runtime_error("Cannot open the video preview cache entry.");
   const auto leasePath = path / "_lease";
   if (fs::exists(leasePath) && !PlainFile(leasePath))
      throw std::runtime_error("The preview cache lease is not a regular file.");
   auto lease = std::make_shared<Lease>();
   lease->handle.reset(CreateFileW(leasePath.c_str(), GENERIC_READ | GENERIC_WRITE,
      0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
   if (lease->handle.get() == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Another video preview is using this cache entry.");
   const auto owner = path / "_owner";
   if (fs::exists(owner) && (!PlainFile(owner) || Read(owner) != Signature))
      throw std::runtime_error("The preview cache entry has another owner.");
   Write(owner, Signature);
   bool complete = PlainFile(path / "_complete") && Read(path / "_source") == identity &&
      PlainFile(path / "_frames.jpg") && (!lightweight || PlainFile(path / "_preview.mp4"));
   try {
      if (!complete) {
         // No consumer sees a half-built pair. Repeated calls inspect files;
         // they do not recover or trust a saved execution checkpoint.
         for (const auto *file : {"_complete", "_building.mp4", "_building.jpg", "_preview.mp4", "_frames.jpg"})
            fs::remove(path / file);
         const auto buildingVideo = wxString((path / "_building.mp4").wstring());
         const auto preview = wxString((path / "_preview.mp4").wstring());
         const auto sheet = wxString((path / "_building.jpg").wstring());
         if (lightweight) {
            if (!VideoExport::RunBundledVideoTool({"-v", "error", "-nostdin", "-n", "-hwaccel", "auto",
                 "-i", sourceName, "-map", "0:v:0", "-an", "-sn", "-dn", "-map_metadata", "-1",
                 "-vf", wxString::FromUTF8(scaleFilter),
                 "-c:v", "mpeg4", "-q:v", "6", "-g", "15", "-bf", "0", "-threads", "2", buildingVideo},
                 parent, _("Prepare Video Preview"), wxString::Format(_("Making a smooth %dp preview. Export uses the original video..."), maximumHeight)))
               throw PreviewPreparationCancelled{};
            fs::rename(path / "_building.mp4", path / "_preview.mp4");
         }
         std::ostringstream rate; rate.imbue(std::locale::classic()); rate << std::setprecision(17) << 128.0 / duration;
         const auto filter = "fps=" + rate.str() +
            ",scale=160:90:force_original_aspect_ratio=decrease,pad=160:90:(ow-iw)/2:(oh-ih)/2,setsar=1,tile=16x8";
         if (!VideoExport::RunBundledVideoTool({"-v", "error", "-nostdin", "-n", "-i",
              lightweight ? preview : sourceName, "-an", "-vf", wxString::FromUTF8(filter),
              "-frames:v", "1", "-q:v", "3", "-update", "1", sheet}, parent,
              _("Prepare Video Track"), _("Making timeline thumbnails...")))
            throw PreviewPreparationCancelled{};
         fs::rename(path / "_building.jpg", path / "_frames.jpg");
         Write(path / "_source", identity);
         Write(path / "_complete", Signature);
         complete = true;
      }
      auto result = std::make_shared<PreparedMedia>();
      result->sourcePath = source;
      result->playbackPath = lightweight ? wxString((path / "_preview.mp4").wstring()) : source;
      result->contactSheet = wxString((path / "_frames.jpg").wstring());
      result->duration = duration; result->lightweight = lightweight; result->lease = lease;
      liveMedia[path] = result;
      CleanPreviewCache();
      return result;
   }
   catch (...) {
      lease->handle.reset();
      if (!complete) RemoveEntry(path);
      throw;
   }
}

std::shared_ptr<const PreparedMedia> PrepareRenderedVideoMedia(const std::string &group,
   const std::string &recipe, const std::vector<std::string> &sources, double duration,
   const std::function<void(const wxString&)> &render, RenderedVideoKind kind)
{
   if (!std::isfinite(duration) || duration <= 0 || !render)
      throw std::runtime_error("A rendered picture needs a usable duration and renderer.");
   std::vector<Handle> inputs;
   std::ostringstream identity; identity.imbue(std::locale::classic());
   identity << "picture-transition=1\n" << recipe;
   const char *extension{};
   switch (kind) {
   case RenderedVideoKind::Ordinary: extension = ".mp4"; break;
   case RenderedVideoKind::Transparent: extension = ".mov"; break;
   case RenderedVideoKind::StaticImage: extension = ".png"; break;
   default: throw std::runtime_error("Unsupported derived picture container.");
   }
   if (kind != RenderedVideoKind::Ordinary) identity << "\ncontainer=" << extension;
   for (const auto &source : sources) {
      const auto path = fs::absolute(fs::path(wxString::FromUTF8(source).ToStdWstring()));
      Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
      BY_HANDLE_FILE_INFORMATION info{};
      if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info))
         throw std::runtime_error("Cannot read a picture transition source: " + source);
      identity << '\n' << source << ':' << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh
         << ':' << info.nFileIndexLow << ':' << info.ftLastWriteTime.dwHighDateTime << ':'
         << info.ftLastWriteTime.dwLowDateTime << ':' << info.nFileSizeHigh << ':' << info.nFileSizeLow;
      inputs.push_back(std::move(file));
   }
   const auto value = identity.str();
   const auto root = CacheRoot();
   fs::create_directories(root);
   if (!PlainDirectory(root)) throw std::runtime_error("The preview cache must be a plain directory.");
   const auto path = root / ("_preview-" + SourceKey(group) + "-fx-" + SourceKey(value));
   if (auto live = liveMedia[path].lock()) {
      if (Read(path / "_source") != value) throw std::runtime_error("A picture cache identity collided.");
      return live;
   }
   CleanPreviewCache(); fs::create_directory(path);
   if (!PlainDirectory(path)) throw std::runtime_error("Cannot create the picture transition cache.");
   if (fs::exists(path / "_lease") && !PlainFile(path / "_lease"))
      throw std::runtime_error("The picture cache lease is not a regular file.");
   auto lease = std::make_shared<Lease>();
   lease->handle.reset(CreateFileW((path / "_lease").c_str(), GENERIC_READ | GENERIC_WRITE,
      0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
   if (lease->handle.get() == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Another process is preparing this picture transition.");
   if (fs::exists(path / "_owner") && (!PlainFile(path / "_owner") || Read(path / "_owner") != Signature))
      throw std::runtime_error("The picture cache entry has another owner.");
   Write(path / "_owner", Signature);
   const auto pending = path / (std::string("_building") + extension);
   const auto published = path / (std::string("_preview") + extension);
   bool complete = PlainFile(path / "_complete") && PlainFile(published) &&
      PlainFile(path / "_source") && Read(path / "_source") == value;
   try {
      if (!complete) {
         fs::remove(path / "_complete"); fs::remove(pending);
         fs::remove(published);
         render(wxString(pending.wstring()));
         if (!PlainFile(pending) || !fs::file_size(pending))
            throw std::runtime_error("The transition renderer produced no picture.");
         fs::rename(pending, published);
         Write(path / "_source", value); Write(path / "_complete", Signature); complete = true;
      }
      auto result = std::make_shared<PreparedMedia>();
      result->sourcePath = result->playbackPath = wxString(published.wstring());
      result->duration = duration; result->lease = lease;
      liveMedia[path] = result; CleanPreviewCache(); return result;
   }
   catch (...) { lease->handle.reset(); if (!complete) RemoveEntry(path); throw; }
}
}
