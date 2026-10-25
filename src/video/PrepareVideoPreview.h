// Feature-owned preview media. Original files remain the source for export.
#pragma once
#include <wx/string.h>
#include <memory>
#include <stdexcept>
#include <functional>
#include <string>
#include <vector>
class wxWindow;
namespace VideoPreview {
class PreviewPreparationCancelled final : public std::runtime_error {
public:
   PreviewPreparationCancelled() : std::runtime_error("Video preview preparation was cancelled.") {}
};
struct PreparedMedia {
   wxString sourcePath, playbackPath, contactSheet;
   double duration{};
   bool lightweight{};
   // Keeps the cache entry leased while either the decoder or filmstrip uses it.
   std::shared_ptr<void> lease;
};
constexpr int ThumbnailColumns = 16, ThumbnailCount = 128;
constexpr int ThumbnailWidth = 160, ThumbnailHeight = 90;
std::shared_ptr<const PreparedMedia> PrepareVideoMedia(const wxString &source,
   double duration, bool lightweight, wxWindow *parent, bool buildIfMissing = true, int maximumHeight = 540);
// Filmstrips may read any completed quality profile. This never builds media or
// selects the playback quality, and retains the cache owner's normal lease.
std::shared_ptr<const PreparedMedia> FindPreparedVideoThumbnails(const wxString &source,
   double duration, wxWindow *parent);
void CleanPreviewCache();
// Derived picture-only spans use the same bounded, leased cache as proxies.
// The callback owns only the supplied pending filename; publication and cleanup
// remain here. Source files are held read-only for the whole operation.
std::shared_ptr<const PreparedMedia> PrepareRenderedVideoMedia(const std::string &group,
   const std::string &recipe, const std::vector<std::string> &sources, double duration,
   const std::function<void(const wxString&)> &render);
}
