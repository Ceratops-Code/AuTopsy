// Native UI/history boundary for owned picture properties.
#pragma once
#include "EditVideoContent.h"
class wxString;
class AudacityProject;
class AuTopsyVideoTrack;
class wxWindow;
namespace VideoTimeline { struct Timeline; }
namespace VideoEditing {
// Complete text-track publication shared by manual overlays and caption batches.
// Preparation may fail or be cancelled; neither case inserts a partial row.
std::shared_ptr<AuTopsyVideoTrack> AddProjectVideoTextTrack(AudacityProject &project,
   AuTopsyVideoTrack *parent, VideoTimeline::Timeline candidate, const wxString &name);
struct VideoTextOverlaySettings {
   std::string text;
   VideoTextAppearance appearance;
   double start{}, seconds{5};
   VideoFrameRectangle rectangle{.1, .75, .8, .2};
};
std::shared_ptr<AuTopsyVideoTrack> AddProjectVideoTextOverlay(AudacityProject &project,
   AuTopsyVideoTrack &parent, const VideoTextOverlaySettings &settings);
bool ChangeProjectVideoTextOverlay(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, const VideoTextOverlaySettings &settings);
bool ShowVideoTextOverlayDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent, double at, bool edit);
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
