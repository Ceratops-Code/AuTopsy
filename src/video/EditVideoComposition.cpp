#include "EditVideoComposition.h"
#include "CreateVideoTrack.h"
#include "ShowVideoCrop.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include <wx/button.h>
#include "ProjectHistory.h"
#include "Viewport.h"
#include "ViewInfo.h"
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <algorithm>
#include <stdexcept>

namespace VideoEditing {
bool ChangeProjectVideoClipMute(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, bool muted)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before changing clip picture mute.");
   auto candidate = track.Timeline();
   if (!VideoTimeline::ChangeVideoClipMute(candidate, clipId, muted)) return false;
   const auto previous = track.Timeline();
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(
      muted ? XO("Muted video clip picture") : XO("Unmuted video clip picture"), XO("Video Clip Mute")); }
   catch (...) { track.SetTimeline(previous); throw; }
   Viewport::Get(project).HandleResize();
   return true;
}

bool ChangeProjectVideoClipCrop(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, const VideoFrameCrop &crop)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing picture crop.");
   auto candidate = track.Timeline();
   if (!VideoTimeline::ChangeVideoClipCrop(candidate, clipId, crop)) return false;
   const auto previous = track.Timeline();
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(XO("Cropped video picture"), XO("Video Crop")); }
   catch (...) { track.SetTimeline(previous); throw; }
   Viewport::Get(project).HandleResize();
   return true;
}

void ShowVideoClipCropDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent)
{
   ClipActions::MediaDialogCommandGuard commands;
   const auto found = std::find_if(track.Timeline().clips.begin(), track.Timeline().clips.end(),
      [clipId](const auto &clip) { return clip.id == clipId; });
   if (found == track.Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
   const auto original = *found;
   ShowVideoPictureCrop(parent, original, VideoTimeline::SourceFor(track.Timeline(), original),
      ViewInfo::Get(project).selectedRegion.t0(), [&](const VideoFrameCrop &selected) {
         ChangeProjectVideoClipCrop(project, track, clipId, selected);
      });
}
}
