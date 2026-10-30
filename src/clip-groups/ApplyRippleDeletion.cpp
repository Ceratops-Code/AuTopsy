#include "ApplyRippleDeletion.h"
#include "PlanRippleDeletion.h"
#include "GroupAudioClips.h"
#include "LinkMediaClips.h"
#include "SelectMediaClips.h"
#include "GuardMediaDialogs.h"
#include "../video/CreateVideoTrack.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectHistory.h"
#include "SyncLock.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "WaveTrack.h"
#include "WaveClip.h"

namespace ClipGroups {
bool DeleteMediaRangeClosingGap(AudacityProject &project, bool wholeProject)
{
   ClipActions::MediaDialogCommandGuard commands;
   if (ProjectAudioIO::Get(project).IsAudioActive()) throw std::runtime_error("Stop playback before ripple deletion.");
   auto &tracks = TrackList::Get(project);
   auto &selection = ViewInfo::Get(project).selectedRegion;
   const SelectedRegion previous = selection;
   try {
      VideoTimeline::SelectVideoClipForDeletion(project);
      if (selection.isPoint()) { selection = previous; return false; }
      const auto from = selection.t0(), to = selection.t1();
      const auto families = BoundVideoTrackMoveFamilies(tracks);
      std::vector<RippleTrackInput<TrackId>> inputs;
      for (auto *track : tracks) if (track->SupportsBasicEditing()) {
         RippleTrackInput<TrackId> input{track->GetId(), SyncLock::IsSelectedOrSyncLockSelected(*track)};
         if (const auto family = families.find(track); family != families.end()) input.family = family->second;
         if (const auto *audio = dynamic_cast<const WaveTrack*>(track)) {
            for (const auto &clip : audio->Intervals())
               input.clips.push_back({clip->GetPlayStartTime(), clip->GetPlayEndTime(),
                  IsAudioClipTimingLocked(*clip), {GetClipGroupId(*clip)}});
         }
         else if (const auto *video = dynamic_cast<const AuTopsyVideoTrack*>(track)) {
            for (const auto &clip : video->Timeline().clips)
               input.clips.push_back({clip.start, clip.End(), clip.timingLocked,
                  VideoTimeline::VideoContentLinkGroups(clip)});
         }
         inputs.push_back(std::move(input));
      }
      const auto targets = PlanRippleDeletion(inputs, from, to, wholeProject);
      if (targets.empty()) { selection = previous; return false; }
      // No event-yielding work is permitted between snapshot, preflight and
      // publication. Native Clear retains its audio/label/video edit contract;
      // history restores the entire project if any edit or autosave fails.
      try {
         for (const auto &id : targets) {
            auto *track = tracks.FindById(id);
            if (!track) throw std::runtime_error("A ripple target was removed.");
            track->Clear(from, to);
         }
         selection.collapseToT0();
         ProjectHistory::Get(project).PushState(
            wholeProject ? XO("Ripple deleted across the whole project") : XO("Ripple deleted across selected and linked tracks"),
            XO("Ripple Delete"));
      }
      catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
      ClearTemporaryClipSelection(project);
      Viewport::Get(project).HandleResize(); Viewport::Get(project).Redraw(); return true;
   }
   catch (...) { selection = previous; throw; }
}
}
