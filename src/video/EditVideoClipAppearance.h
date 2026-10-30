// Native presentation boundary; picture content and timing remain untouched.
#pragma once
#include "EditVideoContent.h"
class AudacityProject;
class AuTopsyVideoTrack;
class wxWindow;
namespace VideoEditing {
bool ChangeVideoClipAppearance(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t id, const VideoClipAppearance &appearance);
void ShowVideoClipAppearanceDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t id, wxWindow *parent);
}
