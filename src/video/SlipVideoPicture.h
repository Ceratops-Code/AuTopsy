// Picture-only source-window editing. Native history and preparation live here.
#pragma once
#include <cstdint>
class AudacityProject;
class AuTopsyVideoTrack;
class wxWindow;
namespace VideoEditing {
bool ChangeVideoPictureSource(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, double sourceStart);
void ShowVideoPictureSlip(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent);
}
