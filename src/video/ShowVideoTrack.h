// Native TrackPanel view and diagnostics for video tracks.
#pragma once
#include "PrepareVideoPreview.h"
#include <cstdint>
#include <vector>
class AudacityProject;
namespace VideoTimeline {
struct VideoTrackStatus {
   bool visible{}, thumbnails{};
   int leftOffset{}, width{}, height{}, usableWidth{};
   double viewStart{}, viewEnd{};
   struct Row {
      int index{}, x{}, y{}, width{}, height{}, dataLeft{}, dataWidth{}, dataTop{};
      bool selected{};
   };
   uintptr_t panelHandle{};
   std::vector<Row> rows;
};
void ShowProjectVideoTrack(AudacityProject &project,
   std::shared_ptr<const VideoPreview::PreparedMedia> media = {});
VideoTrackStatus GetProjectVideoTrackStatus(AudacityProject &project);
}
