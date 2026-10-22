// Host-free, value-based edit preparation. Native adapters publish one completed
// plan inside one project/history transaction; snapshots are never a second live
// project store. Track IDs are opaque adapter-provided identities, not indices.
#pragma once
#include "EditVideoTimeline.h"

namespace VideoEditing {
struct VideoTrackSnapshot {
   std::string id;
   uint64_t revision{};
   VideoTimeline::Timeline timeline;
   std::string parentTrackId;
};
struct VideoProjectSnapshot {
   // Native display order: first track is the uppermost picture layer.
   std::vector<VideoTrackSnapshot> tracks;
};

enum class VideoEditKind { MoveClip, TrimClip, SplitClip, RemoveClip, MoveBoundTracks, HealClips, JoinClips, SeparateClips, ChangeClipSpeed };
struct VideoEditRequest {
   VideoEditKind kind;
   std::string trackId;
   uint64_t clipId{};
   // Absolute project times. MoveBoundTracks sets this track's first clip start;
   // it shifts the entire parent/descendant group by the same exact delta.
   double start{}, end{};
   double playbackRate{1};
};

void RequireUsableVideoProject(const VideoProjectSnapshot &project);
bool SameVideoProject(const VideoProjectSnapshot &left, const VideoProjectSnapshot &right);

class VideoEditPlan {
public:
   const VideoProjectSnapshot &Before() const { return mBefore; }
   const VideoProjectSnapshot &After() const { return mAfter; }
   const std::vector<std::string> &ChangedTrackIds() const { return mChangedTrackIds; }
private:
   VideoProjectSnapshot mBefore, mAfter;
   std::vector<std::string> mChangedTrackIds;
   friend VideoEditPlan PlanVideoEdits(const VideoProjectSnapshot &,
      const std::vector<VideoEditRequest> &);
};

// A rejected request cannot change the input. A plan checks the whole snapshot,
// not merely revision counters, before publication. An already-applied exact
// plan is a no-op; unrelated intervening edits make it stale and reject it.
VideoEditPlan PlanVideoEdits(const VideoProjectSnapshot &project,
   const std::vector<VideoEditRequest> &requests);
enum class VideoEditApplication { Applied, AlreadyApplied };
VideoEditApplication ApplyVideoEditPlan(VideoProjectSnapshot &project, const VideoEditPlan &plan);
}
