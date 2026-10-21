// Feature-owned preview media. Original files remain the source for export.
#pragma once
#include <wx/string.h>
#include <memory>
class wxWindow;
namespace VideoPreview {
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
   double duration, bool lightweight, wxWindow *parent, bool buildIfMissing = true);
void CleanPreviewCache();
}
