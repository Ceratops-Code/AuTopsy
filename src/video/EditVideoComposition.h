// Native UI/history boundary for owned picture properties.
#pragma once
#include "EditVideoContent.h"
class AudacityProject;
class AuTopsyVideoTrack;
class wxWindow;
namespace VideoEditing {
bool ChangeProjectVideoClipMute(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, bool muted);
bool ChangeProjectVideoClipCrop(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, const VideoFrameCrop &crop);
void ShowVideoClipCropDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent);
}
