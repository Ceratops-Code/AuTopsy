#include "SelectMediaClips.h"
#include "GroupAudioClips.h"
#include "Project.h"
#include "Track.h"
#include "WaveClip.h"
#include "WaveTrack.h"
#include "ViewInfo.h"
#include "tracks/ui/TimeShiftHandle.h"
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
#include "../video/CreateVideoTrack.h"
#endif
#include <limits>
#include <memory>
#include <stdexcept>

namespace ClipGroups {
namespace {
struct ClipReference {
   TemporaryClipSelection::Key key{};
   std::weak_ptr<Track> track;
   std::weak_ptr<WaveClip> audio;
   std::uint64_t videoId{};
};
struct ProjectClipSelection final : ClientData::Base {
   TemporaryClipSelection selection;
   std::vector<ClipReference> references;
   std::uint64_t nextKey{1};

   void Prune(AudacityProject &project) {
      auto &tracks = TrackList::Get(project);
      std::vector<TemporaryClipSelection::Key> live;
      references.erase(std::remove_if(references.begin(), references.end(), [&](ClipReference &ref) {
         if (const auto audio = ref.audio.lock()) {
            for (auto *track : tracks.Any<WaveTrack>())
               for (const auto &clip : track->Intervals()) if (clip.get() == audio.get()) {
                  ref.track = track->SharedPointer(); live.push_back(ref.key); return false;
               }
         }
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
         else if (ref.videoId) {
            const auto owner = ref.track.lock();
            for (auto *track : tracks.Any<AuTopsyVideoTrack>())
               if (track == owner.get())
                  for (const auto &clip : track->Timeline().clips) if (clip.id == ref.videoId) {
                     live.push_back(ref.key); return false;
                  }
         }
#endif
         return true;
      }), references.end());
      selection.Retain(live);
   }
   std::uint64_t Add(Track &track, std::shared_ptr<WaveClip> audio, std::uint64_t videoId) {
      for (const auto &ref : references)
         if (audio ? ref.audio.lock() == audio : ref.videoId == videoId && ref.track.lock().get() == &track)
            return ref.key;
      if (nextKey == std::numeric_limits<std::uint64_t>::max())
         throw std::overflow_error("Temporary clip selection exhausted its identities.");
      references.push_back({nextKey++, track.SharedPointer(), audio, videoId});
      return references.back().key;
   }
   void DropUnused() {
      references.erase(std::remove_if(references.begin(), references.end(), [&](const ClipReference &ref) {
         return !selection.Contains(ref.key);
      }), references.end());
   }
};
const AudacityProject::AttachedObjects::RegisteredFactory sSelectionKey{
   [](AudacityProject &) { return std::make_shared<ProjectClipSelection>(); }
};
ProjectClipSelection &For(AudacityProject &project)
{ return project.AttachedObjects::Get<ProjectClipSelection>(sSelectionKey); }
}

bool ClickTemporaryMediaClip(AudacityProject &project, Track &track, const ChannelGroupInterval &hit, bool additive)
{
   auto &state = For(project);
   state.Prune(project);
   std::uint64_t key{};
   if (auto *wave = dynamic_cast<WaveTrack *>(&track)) {
      for (const auto &clip : wave->Intervals())
         if (clip.get() == &hit) {
            key = state.Add(track, clip, 0); break;
         }
   }
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
   else if (auto *video = dynamic_cast<AuTopsyVideoTrack *>(&track)) {
      const auto *interval = dynamic_cast<const LinkedClipInterval *>(&hit);
      if (interval)
         for (const auto &clip : video->Timeline().clips)
            if (clip.id == interval->VideoClipId()) { key = state.Add(track, {}, clip.id); break; }
   }
#endif
   if (!key) return false;
   state.selection.Click(key, additive);
   state.DropUnused();
   return true;
}

bool ClickTemporaryMediaClipAtPointer(AudacityProject &project, Track &track,
   const wxRect &rect, int x, int y, bool additive)
{
   auto shifter = MakeTrackShifter::Call(track, project);
   const auto &view = ViewInfo::Get(project);
   TrackShifter::HitTestParams params{rect, x, y};
   shifter->HitTest(view.PositionToTime(x, rect.x), view, &params);
   return params.hitInterval && ClickTemporaryMediaClip(project, track, *params.hitInterval, additive);
}

void ClearTemporaryClipSelection(AudacityProject &project)
{ auto &state = For(project); state.selection.Clear(); state.references.clear(); }

bool HasTemporaryClipSelection(AudacityProject &project)
{ auto &state = For(project); state.Prune(project); return state.selection.IsExplicit(); }

bool IsAudioClipInTemporarySelection(AudacityProject &project, const WaveClip &clip)
{
   const auto &state = For(project);
   return state.selection.IsExplicit() && std::any_of(state.references.begin(), state.references.end(),
      [&](const ClipReference &ref) { return state.selection.Contains(ref.key) && ref.audio.lock().get() == &clip; });
}

bool IsVideoClipInTemporarySelection(AudacityProject &project, const Track &track, std::uint64_t id)
{
   const auto &state = For(project);
   return state.selection.IsExplicit() && std::any_of(state.references.begin(), state.references.end(),
      [&](const ClipReference &ref) { return state.selection.Contains(ref.key) && ref.videoId == id && ref.track.lock().get() == &track; });
}

TemporaryClipSelection::Key TemporaryVideoClipSelectionKey(AudacityProject &project,
   const Track &track, std::uint64_t id)
{
   const auto &state = For(project);
   for (const auto &ref : state.references)
      if (state.selection.Contains(ref.key) && ref.videoId == id && ref.track.lock().get() == &track)
         return ref.key;
   return 0;
}

void MoveTemporaryVideoClipReference(AudacityProject &project, TemporaryClipSelection::Key key,
   Track &destination, std::uint64_t id)
{
   // Remap by the opaque selection key, never a source track/ID pair that an
   // earlier attach may already have changed during a multi-track migration.
   auto &state = For(project);
   if (!key || !state.selection.Contains(key)) return;
   for (auto &ref : state.references) if (ref.key == key) {
      ref.track = destination.SharedPointer(); ref.videoId = id; return;
   }
}

bool ApplyTemporaryClipMoveSelection(AudacityProject &project, ClipMoveState &state)
{
   auto &saved = For(project);
   saved.Prune(project);
   const auto keys = saved.selection.MovingKeys();
   if (keys.empty()) return false;
   // Discard the host's contiguous time-range hit selection before expanding
   // our exact identities. Its existing engine still owns all actual movement.
   for (auto &pair : state.shifters) pair.second = MakeTrackShifter::Call(*pair.first, project);
   for (const auto key : keys) {
      const auto found = std::find_if(saved.references.begin(), saved.references.end(),
         [key](const ClipReference &ref) { return ref.key == key; });
      if (found == saved.references.end()) throw std::runtime_error("A selected clip is no longer available.");
      const auto track = found->track.lock();
      const auto shifter = state.shifters.find(track.get());
      if (shifter == state.shifters.end()) throw std::runtime_error("A selected clip track is no longer available.");
      const auto before = shifter->second->MovingIntervals().size();
      shifter->second->UnfixIntervals([&](const ChannelGroupInterval &interval) {
         if (const auto audio = found->audio.lock()) return &interval == audio.get();
         const auto *video = dynamic_cast<const LinkedClipInterval *>(&interval);
         return video && video->VideoClipId() == found->videoId;
      });
      if (shifter->second->MovingIntervals().size() != before + 1)
         throw std::runtime_error("A selected clip is missing or ambiguous; no clips were moved.");
   }
   return true;
}
}
