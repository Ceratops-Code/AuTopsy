// Narrow native-window adapter for the feature-owned video timeline.
#pragma once
#include "PrepareVideoPreview.h"
class AudacityProject;
namespace VideoTimeline {
struct VideoTrackStatus {
   bool visible{}, thumbnails{};
   int leftOffset{}, width{}, usableWidth{};
   double viewStart{}, viewEnd{};
};
void ShowProjectVideoTrack(AudacityProject &project,
   std::shared_ptr<const VideoPreview::PreparedMedia> media = {});
VideoTrackStatus GetProjectVideoTrackStatus(AudacityProject &project);
}
