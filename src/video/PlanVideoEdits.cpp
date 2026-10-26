#include "PlanVideoEdits.h"
#include "JoinVideoClips.h"
#include <limits>
#include <unordered_map>

namespace VideoEditing {
namespace {
bool SameTimeline(const VideoTimeline::Timeline &a, const VideoTimeline::Timeline &b)
{
   if (a.sourcePath != b.sourcePath || a.sourceDuration != b.sourceDuration ||
       a.frameRate != b.frameRate || a.nextId != b.nextId ||
       a.sourceWidth != b.sourceWidth || a.sourceHeight != b.sourceHeight ||
       a.protectedSourcePaths != b.protectedSourcePaths || a.clips.size() != b.clips.size() ||
       a.placement.id != b.placement.id || a.placement.parentId != b.placement.parentId ||
       !(a.placement.rectangle == b.placement.rectangle))
      return false;
   for (size_t i = 0; i < a.clips.size(); ++i) {
      const auto &x = a.clips[i], &y = b.clips[i];
      if (!VideoTimeline::SameVideoContent(x, y) || x.id != y.id || x.start != y.start || x.sourceStart != y.sourceStart ||
          x.duration != y.duration || x.linkGroup != y.linkGroup) return false;
   }
   return true;
}

size_t TrackIndex(const VideoProjectSnapshot &project, const std::string &id)
{
   const auto found = std::find_if(project.tracks.begin(), project.tracks.end(),
      [&](const auto &track) { return track.id == id; });
   if (found == project.tracks.end()) throw std::runtime_error("Unknown video track: " + id);
   return size_t(found - project.tracks.begin());
}

std::string GroupRoot(const VideoProjectSnapshot &project, size_t index)
{
   while (!project.tracks[index].parentTrackId.empty())
      index = TrackIndex(project, project.tracks[index].parentTrackId);
   return project.tracks[index].id;
}

void MoveBoundTracks(VideoProjectSnapshot &project, size_t anchor, double start)
{
   if (!std::isfinite(start) || start < 0 || project.tracks[anchor].timeline.clips.empty())
      throw std::runtime_error("Moving bound tracks needs a finite start and a nonempty anchor track.");
   const auto root = GroupRoot(project, anchor);
   const auto delta = start - project.tracks[anchor].timeline.clips.front().start;
   // Do not snap each source independently: that would change synchronization
   // between bound sources with different frame rates.
   for (size_t i = 0; i < project.tracks.size(); ++i)
      if (GroupRoot(project, i) == root)
         for (auto &clip : project.tracks[i].timeline.clips) clip.start += delta;
}
}

void RequireUsableVideoProject(const VideoProjectSnapshot &project)
{
   if (project.tracks.size() > 1000) throw std::runtime_error("Too many video tracks in an edit snapshot.");
   std::unordered_map<std::string, size_t> tracks;
   for (size_t i = 0; i < project.tracks.size(); ++i) {
      const auto &track = project.tracks[i];
      if (track.id.empty() || !tracks.emplace(track.id, i).second)
         throw std::runtime_error("Video tracks require unique, nonempty identities.");
      VideoTimeline::RequireWellFormedTimeline(track.timeline);
   }
   for (const auto &track : project.tracks) {
      std::unordered_set<std::string> ancestors{track.id};
      auto parent = track.parentTrackId;
      while (!parent.empty()) {
         const auto found = tracks.find(parent);
         if (found == tracks.end() || !ancestors.insert(parent).second)
            throw std::runtime_error("Embedded video requires an existing parent without cycles.");
         parent = project.tracks[found->second].parentTrackId;
      }
   }
}

bool SameVideoProject(const VideoProjectSnapshot &a, const VideoProjectSnapshot &b)
{
   if (a.tracks.size() != b.tracks.size()) return false;
   for (size_t i = 0; i < a.tracks.size(); ++i) {
      const auto &x = a.tracks[i], &y = b.tracks[i];
      if (x.id != y.id || x.revision != y.revision || x.parentTrackId != y.parentTrackId ||
          !SameTimeline(x.timeline, y.timeline)) return false;
   }
   return true;
}

VideoEditPlan PlanVideoEdits(const VideoProjectSnapshot &project,
   const std::vector<VideoEditRequest> &requests)
{
   RequireUsableVideoProject(project);
   VideoEditPlan plan;
   plan.mBefore = project;
   plan.mAfter = project;
   for (const auto &request : requests) {
      const auto index = TrackIndex(plan.mAfter, request.trackId);
      auto &timeline = plan.mAfter.tracks[index].timeline;
      switch (request.kind) {
      case VideoEditKind::MoveClip:
         VideoTimeline::MoveClipTo(timeline, request.clipId, request.start); break;
      case VideoEditKind::TrimClip:
         VideoTimeline::TrimVideoClip(timeline, request.clipId, request.start, request.end); break;
      case VideoEditKind::SplitClip:
         VideoTimeline::SplitClipAt(timeline, request.start); break;
      case VideoEditKind::RemoveClip:
         VideoTimeline::RemoveClip(timeline, request.clipId); break;
      case VideoEditKind::MoveBoundTracks:
         MoveBoundTracks(plan.mAfter, index, request.start); break;
      case VideoEditKind::HealClips:
         VideoTimeline::HealVideoClips(timeline, request.start, request.end); break;
      case VideoEditKind::JoinClips:
         VideoTimeline::JoinVideoClips(timeline, request.start, request.end); break;
      case VideoEditKind::SeparateClips:
         VideoTimeline::SeparateVideoClips(timeline, request.start, request.end); break;
      case VideoEditKind::ChangeClipSpeed:
         VideoTimeline::ChangeVideoClipSpeed(timeline, request.clipId, request.playbackRate); break;
      default:
         throw std::runtime_error("Unknown video edit operation.");
      }
      RequireUsableVideoProject(plan.mAfter);
   }
   for (size_t i = 0; i < project.tracks.size(); ++i) {
      auto &track = plan.mAfter.tracks[i];
      if (SameTimeline(project.tracks[i].timeline, track.timeline)) continue;
      if (track.revision == std::numeric_limits<uint64_t>::max())
         throw std::runtime_error("Video track revision is exhausted.");
      ++track.revision;
      plan.mChangedTrackIds.push_back(track.id);
   }
   return plan;
}

VideoEditApplication ApplyVideoEditPlan(VideoProjectSnapshot &project, const VideoEditPlan &plan)
{
   if (SameVideoProject(project, plan.After())) return VideoEditApplication::AlreadyApplied;
   if (!SameVideoProject(project, plan.Before()))
      throw std::runtime_error("The video edit is stale; prepare it from the current project.");
   auto completed = plan.After(); // All allocation can fail before the single effect.
   project.tracks.swap(completed.tracks); // noexcept, whole-snapshot publication.
   return VideoEditApplication::Applied;
}
}
