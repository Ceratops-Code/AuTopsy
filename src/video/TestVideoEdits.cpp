// Dedicated planner cases; no Audacity process, native classes or media files.
#include "PlanVideoEdits.h"
#include <catch2/catch.hpp>
#include <limits>

using namespace VideoEditing;
namespace {
VideoProjectSnapshot Project()
{
   return {{{"parent", 4, VideoTimeline::CreateVideoTimeline("parent.mp4", 12, 30)},
      {"child", 9, VideoTimeline::CreateVideoTimeline("child.mp4", 8, 25), "parent"},
      {"other", 2, VideoTimeline::CreateVideoTimeline("other.mp4", 20, 24)}}};
}
}

TEST_CASE("Edit plans publish all replacements once and reject stale snapshots", "[video-edits]")
{
   auto project = Project();
   const auto before = project;
   const auto plan = PlanVideoEdits(project, {
      {VideoEditKind::MoveClip, "parent", 1, 2},
      {VideoEditKind::TrimClip, "child", 1, 1, 7}});
   REQUIRE(SameVideoProject(project, before));
   REQUIRE(plan.ChangedTrackIds() == std::vector<std::string>{"parent", "child"});
   REQUIRE(ApplyVideoEditPlan(project, plan) == VideoEditApplication::Applied);
   REQUIRE(project.tracks[0].timeline.clips[0].start == 2);
   REQUIRE(project.tracks[1].timeline.clips[0].sourceStart == 1);
   REQUIRE(project.tracks[0].revision == 5);
   REQUIRE(project.tracks[1].revision == 10);
   REQUIRE(project.tracks[2].revision == 2);
   REQUIRE(ApplyVideoEditPlan(project, plan) == VideoEditApplication::AlreadyApplied);
   auto stale = before;
   stale.tracks[2].timeline.clips[0].start = 3;
   const auto intervening = stale;
   REQUIRE_THROWS(ApplyVideoEditPlan(stale, plan));
   REQUIRE(SameVideoProject(stale, intervening));
}

TEST_CASE("Rejected batch edits expose none of their earlier candidate changes", "[video-edits]")
{
   auto project = Project();
   const auto before = project;
   REQUIRE_THROWS(PlanVideoEdits(project, {
      {VideoEditKind::MoveClip, "parent", 1, 4},
      {VideoEditKind::TrimClip, "child", 1, -1, 3}}));
   REQUIRE(SameVideoProject(project, before));
   project.tracks[0].revision = std::numeric_limits<uint64_t>::max();
   REQUIRE_THROWS(PlanVideoEdits(project, {{VideoEditKind::MoveClip, "parent", 1, 2}}));
   REQUIRE(project.tracks[0].timeline.clips[0].start == 0);
   REQUIRE_THROWS(PlanVideoEdits(project, {{VideoEditKind::MoveClip, "missing", 1, 2}}));
}

TEST_CASE("Fresh desired edits converge after interruption on either side of publication", "[video-edits]")
{
   const std::vector<VideoEditRequest> requests{{VideoEditKind::MoveClip, "parent", 1, 3}};
   for (const bool afterEffect : {false, true}) {
      auto project = Project();
      const auto planned = PlanVideoEdits(project, requests);
      if (afterEffect) ApplyVideoEditPlan(project, planned);
      // Discard all caller execution progress, then use current authoritative
      // state and the normal entry points, as a fresh gesture/command would.
      const auto fresh = PlanVideoEdits(project, requests);
      ApplyVideoEditPlan(project, fresh);
      REQUIRE(project.tracks[0].timeline.clips[0].start == 3);
      REQUIRE(project.tracks[0].revision == 5);
      for (int repeat = 0; repeat < 4; ++repeat) {
         const auto again = PlanVideoEdits(project, requests);
         REQUIRE(again.ChangedTrackIds().empty());
         REQUIRE(ApplyVideoEditPlan(project, again) == VideoEditApplication::AlreadyApplied);
      }
   }
}

TEST_CASE("Bound track movement follows identities and keeps every source synchronized", "[video-edits]")
{
   auto project = Project();
   project.tracks.push_back({"grandchild", 1,
      VideoTimeline::CreateVideoTimeline("grandchild.mp4", 6, 30000.0 / 1001), "child"});
   // Reordering the pane does not alter the relationship or its movement.
   std::swap(project.tracks[0], project.tracks[2]);
   const auto plan = PlanVideoEdits(project, {{VideoEditKind::MoveBoundTracks, "child", 0, 1.123}});
   ApplyVideoEditPlan(project, plan);
   REQUIRE(plan.ChangedTrackIds().size() == 3);
   REQUIRE(project.tracks[0].timeline.clips[0].start == 0); // unrelated
   for (size_t i = 1; i < project.tracks.size(); ++i) {
      REQUIRE(project.tracks[i].timeline.clips[0].start == Approx(1.123));
      REQUIRE(project.tracks[i].timeline.clips[0].sourceStart == 0);
   }
   project.tracks[2].timeline.clips[0].start = 3;
   const auto before = project;
   REQUIRE_THROWS(PlanVideoEdits(project, {{VideoEditKind::MoveBoundTracks, "parent", 0, 0}}));
   REQUIRE(SameVideoProject(project, before));
}

TEST_CASE("Snapshots reject ambiguous tracks and embedded parent cycles", "[video-edits]")
{
   auto project = Project();
   project.tracks[0].parentTrackId = "child";
   REQUIRE_THROWS(RequireUsableVideoProject(project));
   project = Project(); project.tracks[1].parentTrackId = "missing";
   REQUIRE_THROWS(RequireUsableVideoProject(project));
   project = Project(); project.tracks[1].id = "parent";
   REQUIRE_THROWS(RequireUsableVideoProject(project));
}

TEST_CASE("Planned trims untrim original content and split repeats retain flags", "[video-edits]")
{
   auto project = Project();
   auto &clip = project.tracks[0].timeline.clips[0];
   clip.markers.push_back({std::string(32, 'a'), 5, "keep"});
   for (int i = 0; i < 40; ++i) {
      ApplyVideoEditPlan(project, PlanVideoEdits(project, {{VideoEditKind::TrimClip, "parent", 1, 2, 10}}));
      ApplyVideoEditPlan(project, PlanVideoEdits(project, {{VideoEditKind::TrimClip, "parent", 1, 0, 12}}));
   }
   REQUIRE(project.tracks[0].timeline.clips[0].sourceStart == 0);
   REQUIRE(project.tracks[0].timeline.clips[0].duration == 12);
   const std::vector<VideoEditRequest> split{{VideoEditKind::SplitClip, "parent", 0, 6}};
   ApplyVideoEditPlan(project, PlanVideoEdits(project, split));
   const auto repeated = PlanVideoEdits(project, split);
   REQUIRE(repeated.ChangedTrackIds().empty());
   REQUIRE(project.tracks[0].timeline.clips.size() == 2);
   for (const auto &part : project.tracks[0].timeline.clips) {
      REQUIRE(part.markers[0].text == "keep");
      REQUIRE(part.markers[0].position == 5);
   }
}
