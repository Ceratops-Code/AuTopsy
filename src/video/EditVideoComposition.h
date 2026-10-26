// Native UI/history boundary for owned picture properties.
#pragma once
#include "EditVideoContent.h"
class wxString;
class AudacityProject;
class AuTopsyVideoTrack;
class wxWindow;
namespace VideoEditing {
enum class VideoClipPictureAction { Reverse, Freeze, Repeat };
bool ApplyProjectVideoPictureAction(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, VideoClipPictureAction action, double at, double seconds, int copies = 1);
void ShowVideoClipPictureAction(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, VideoClipPictureAction action, wxWindow *parent, double at);
void AddProjectVideoPicture(AudacityProject &project, VideoTimeline::Clip clip, const wxString &name);
bool ChangeProjectVideoClipMute(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, bool muted);
bool ChangeProjectVideoClipCrop(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, const VideoFrameCrop &crop);
bool ChangeProjectVideoTrackPlacement(AudacityProject &project, AuTopsyVideoTrack &track,
   const VideoLayerPlacement &placement);
void ShowVideoTrackEmbeddingDialog(AudacityProject &project, AuTopsyVideoTrack &track, wxWindow *parent);
void ShowVideoClipCropDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent);
}
