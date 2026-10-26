// Picture-only crop interaction. Draft state and the decoded frame live only
// for this dialog; the native adapter commits Apply before the dialog closes.
#pragma once
#include "EditVideoContent.h"
#include <functional>
#include <optional>
class wxWindow;
namespace VideoEditing {
struct VideoPlacementPicture {
   VideoTimeline::Clip clip;
   VideoTimeline::MediaSource source;
};
void ShowVideoPicturePlacement(wxWindow *parent,
   const std::optional<VideoPlacementPicture> &child,
   const std::optional<VideoPlacementPicture> &background, double projectTime,
   const VideoFrameRectangle &initial, const std::function<void(const VideoFrameRectangle&)> &apply);
void ShowVideoPictureCrop(wxWindow *parent, const VideoTimeline::Clip &clip,
   const VideoTimeline::MediaSource &fallback, double projectTime,
   const std::function<void(const VideoFrameCrop&)> &apply);
}
