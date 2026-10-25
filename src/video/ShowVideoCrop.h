// Picture-only crop interaction. Draft state and the decoded frame live only
// for this dialog; the native adapter commits Apply before the dialog closes.
#pragma once
#include "EditVideoContent.h"
#include <functional>
class wxWindow;
namespace VideoEditing {
void ShowVideoPictureCrop(wxWindow *parent, const VideoTimeline::Clip &clip,
   const VideoTimeline::MediaSource &fallback, double projectTime,
   const std::function<void(const VideoFrameCrop&)> &apply);
}
