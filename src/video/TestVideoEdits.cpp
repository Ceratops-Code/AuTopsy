// Dedicated planner cases; no Audacity process, native classes or media files.
#include "PlanVideoEdits.h"
#include "JoinVideoClips.h"
#include "RelinkVideoMedia.h"
#include "../clip-groups/PlanClipPlacement.h"
#include <catch2/catch.hpp>
#include <limits>

using namespace VideoEditing;

TEST_CASE("Range export rebases every layer while retaining content clocks and source protection", "[video-edits][range-export]")
{
   using namespace VideoTimeline;
   auto main = CreateVideoTimeline("main.mp4", 12, 30);
   main.sourceWidth = 640; main.sourceHeight = 360; main.placement.id = "main";
   main.clips[0].opacity = {0, 4, 12, 2}; main.clips[0].crop.top = .2;
   SplitClipAt(main, 5);
   auto overlay = CreateVideoTimeline("overlay.mp4", 12, 25);
   overlay.placement = {"overlay", "main", {.6, .1, .3, .3}};
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto plan = PlanVideoRangeExport({&overlay, &main}, main, 3, 8);
      REQUIRE(plan.projectStart == 3); REQUIRE(plan.duration == 5);
      REQUIRE(plan.layers.size() == 2);
      REQUIRE(plan.layers[0].placement.parentId == "main");
      REQUIRE(plan.layers[0].placement.rectangle == overlay.placement.rectangle);
      REQUIRE(plan.layers[1].clips.size() == 2);
      REQUIRE(plan.layers[1].clips[0].start == 0);
      REQUIRE(plan.layers[1].clips[0].sourceStart == 3);
      REQUIRE(plan.layers[1].clips[1].start == 2);
      REQUIRE(plan.layers[1].clips[1].sourceStart == 5);
      REQUIRE(plan.layers[1].clips[0].crop == main.clips[0].crop);
      REQUIRE(plan.layers[1].clips[0].opacity == main.clips[0].opacity);
      REQUIRE(plan.layers[1].clips[0].ContentTimeAtProjectTime(.5) ==
         main.clips[0].ContentTimeAtProjectTime(3.5));
      REQUIRE(main.clips[0].duration == 5); REQUIRE(main.clips[1].start == 5);
      const auto empty = PlanVideoRangeExport({}, main, 15, 17);
      REQUIRE(empty.picture.clips.empty()); REQUIRE(empty.duration == 2);
      REQUIRE(empty.picture.sourcePath == "main.mp4"); REQUIRE(empty.picture.sourceWidth == 640);
      REQUIRE(std::find(empty.picture.protectedSourcePaths.begin(), empty.picture.protectedSourcePaths.end(),
         "main.mp4") != empty.picture.protectedSourcePaths.end());
      REQUIRE_THROWS(PlanVideoRangeExport({&main}, main, 2, 2));
      REQUIRE_THROWS(PlanVideoRangeExport({&main}, main, -1, 2));
      REQUIRE_THROWS(PlanVideoRangeExport({nullptr}, main, 0, 2));
   }
}
TEST_CASE("Video names and colours survive structural edits without changing picture sampling", "[video-edits][clip-appearance]")
{
   using namespace VideoTimeline;
   const VideoClipAppearance style{u8"\u05e9\u05dc\u05d5\u05dd / \u0422\u0435\u0441\u0442", 0x22eecc};
   for (int repeat{}; repeat < 2; ++repeat) {
      auto timeline = CreateVideoTimeline("source.mp4", 12, 30);
      const auto original = SampleVideoContent(timeline.clips[0], SourceFor(timeline, timeline.clips[0]), 3);
      timeline.clips[0].appearance = style;
      REQUIRE_NOTHROW(RequireWellFormedTimeline(timeline));
      const auto named = SampleVideoContent(timeline.clips[0], SourceFor(timeline, timeline.clips[0]), 3);
      REQUIRE(named.source.path == original.source.path); REQUIRE(named.sourceTime == original.sourceTime);
      auto generated = timeline;
      REQUIRE(ReverseVideoClip(generated, 1)); REQUIRE(generated.clips[0].appearance == style);
      REQUIRE_FALSE(ReverseVideoClip(generated, 1)); REQUIRE(generated.clips[0].appearance == style);
      generated = timeline;
      REQUIRE(FreezeVideoClip(generated, 1, 3, 2)); REQUIRE(generated.clips[0].appearance == style);
      SplitClipAt(timeline, 6); REQUIRE(timeline.clips[1].appearance == style);
      auto copy = CopyVideoRange(timeline, 2, 9);
      REQUIRE(copy.clips[0].appearance == style); REQUIRE(copy.clips[1].appearance == style);
      const VideoClipAppearance second{"Second", 0xffffff}; timeline.clips[1].appearance = second;
      REQUIRE(JoinVideoClips(timeline, 0, 12)); REQUIRE(timeline.clips[0].appearance == style);
      REQUIRE(SeparateVideoClips(timeline, 0, 12));
      REQUIRE(timeline.clips[0].appearance == style); REQUIRE(timeline.clips[1].appearance == second);
      REQUIRE(HealVideoClips(timeline, 0, 12)); REQUIRE(timeline.clips[0].appearance == style);
      REQUIRE_NOTHROW(RequireUsableVideoClipAppearance({"", -1}));
      for (const auto &value : std::vector<VideoClipAppearance>{{"bad\nname", 0}, {std::string(1025, 'x'), 0},
            {std::string("\xc0\x80", 2), 0}, {std::string("\xed\xa0\x80", 3), 0}, {"ok", -2}, {"ok", 0x1000000}})
         REQUIRE_THROWS(RequireUsableVideoClipAppearance(value));
   }
}

TEST_CASE("Clip placement distinguishes touching edges, gaps and occupied destinations", "[video-edits][clip-placement]")
{
   using namespace ClipActions;
   const std::vector<ClipPlacementInterval> neighbours{{0, 2}, {7, 9}, {3, 4}};
   for (int repeat{}; repeat < 2; ++repeat) {
      REQUIRE(FindGapBeforeClip(5, neighbours) == 4);
      REQUIRE_NOTHROW(RequireFreeClipPlacement({4, 7, 1}, neighbours));
      REQUIRE_THROWS(RequireFreeClipPlacement({3.5, 5, 1}, neighbours));
      REQUIRE_THROWS(FindGapBeforeClip(3.5, neighbours));
      REQUIRE_THROWS(RequireClipTiming({2, 2, 1}));
      REQUIRE_THROWS(RequireClipTiming({0, 2, std::numeric_limits<double>::quiet_NaN()}));
      REQUIRE_NOTHROW(RequireConsistentClipRetiming(4, 1, {8, 10, 2}));
      REQUIRE_THROWS(RequireConsistentClipRetiming(4, 1, {8, 11, 2}));
   }
}

TEST_CASE("Combined speed and placement preserve linked offsets at final free destinations", "[video-edits][clip-placement]")
{
   auto timeline = VideoTimeline::CreateVideoTimeline("source.mp4", 12, 30);
   auto original = timeline.clips[0]; original.start = 4; original.duration = 4;
   original.timingLocked = true;
   const VideoTimeline::VideoClipRetiming change{4, .5, 6};
   const auto moved = VideoTimeline::RetimeVideoClip(original, VideoTimeline::SourceFor(timeline, original), change);
   REQUIRE(moved.start == 10);
   REQUIRE(moved.duration == 2);
   REQUIRE(moved.playbackRate == 2);
   REQUIRE(moved.timingLocked);
   REQUIRE(change.ProjectTimeAfterRetiming(6) == 11);
   auto unprotected = moved; unprotected.timingLocked = false;
   REQUIRE_FALSE(VideoTimeline::SameVideoContent(moved, unprotected));
   REQUIRE_THROWS(VideoTimeline::RetimeVideoClip(original, VideoTimeline::SourceFor(timeline, original),
      {4, .5, std::numeric_limits<double>::infinity()}));
}
namespace {
VideoProjectSnapshot Project()
{
   return {{{"parent", 4, VideoTimeline::CreateVideoTimeline("parent.mp4", 12, 30)},
      {"child", 9, VideoTimeline::CreateVideoTimeline("child.mp4", 8, 25), "parent"},
      {"other", 2, VideoTimeline::CreateVideoTimeline("other.mp4", 20, 24)}}};
}
}

TEST_CASE("A link assigned to a joined parent retains a whole soundtrack window", "[video-edits][video-links]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("source.mp4", 8, 30);
   SplitClipAt(timeline, 4);
   JoinVideoClips(timeline, 0, 8);
   timeline.clips[0].linkGroup = std::string(32, 'a');
   ChangeVideoClipSpeed(timeline, 1, 2);
   TrimVideoClip(timeline, 1, 1, 3);
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto windows = VideoContentLinkWindows(timeline.clips[0]);
      REQUIRE(windows.size() == 1);
      REQUIRE(windows[0].group == std::string(32, 'a'));
      REQUIRE(windows[0].start == 1);
      REQUIRE(windows[0].end == 3);
      REQUIRE(windows[0].sourceOrigin == 0);
      REQUIRE(windows[0].sourceEnd == 4);
      REQUIRE(windows[0].playbackRate == 2);
      REQUIRE(windows[0].wholeContent);
   }
}

TEST_CASE("Track and retained-tree bounds are independent", "[video-edits][video-storage]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("source.mp4", 1, 30);
   timeline.clips.clear();
   for (size_t i{}; i < MaximumVideoTrackClips; ++i)
      timeline.clips.push_back({i + 1, double(i) / 30, 0, 1.0 / 30});
   timeline.nextId = timeline.clips.size() + 1;
   REQUIRE_NOTHROW(RequireWellFormedTimeline(timeline));
   auto extra = timeline.clips.back(); extra.id = timeline.nextId++; extra.start = timeline.End();
   timeline.clips.push_back(extra);
   REQUIRE_THROWS(RequireWellFormedTimeline(timeline));
   auto content = std::make_shared<VideoClipContent>();
   content->duration = timeline.End(); content->frameRate = 30;
   content->parts = timeline.clips;
   for (auto &part : content->parts) part.media = {"source.mp4", 1, 30};
   Clip joined{1, 0, 0, content->duration}; joined.content = content;
   REQUIRE_THROWS(RequireUsableVideoContent(joined, {}));
   content->parts.resize(MaximumVideoContentClips - 1);
   REQUIRE_NOTHROW(RequireUsableVideoContent(joined, {}));
   timeline.clips = {joined, joined}; timeline.nextId = 3;
   timeline.clips[1].id = 2; timeline.clips[1].start = joined.End();
   REQUIRE_NOTHROW(RequireWellFormedTimeline(timeline));
   const auto composed = ComposeVideoTracks({&timeline});
   REQUIRE(composed.clips.size() == 2 * (MaximumVideoContentClips - 1));
   REQUIRE_NOTHROW(RequireWellFormedTimeline(composed, MaximumVideoRenderSegments));
   REQUIRE_THROWS(RequireWellFormedTimeline(composed));
}

TEST_CASE("Video speed retains source handles flags and split editability", "[video-edits][video-speed]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("source.mp4", 12, 30);
   MoveClipTo(timeline, 1, 3);
   TrimVideoClip(timeline, 1, 5, 11);
   timeline.clips[0].markers = {{std::string(32, 'a'), 6, "retained flag"}};
   for (int repeat{}; repeat < 2; ++repeat) {
      REQUIRE(ChangeVideoClipSpeed(timeline, 1, 2));
      REQUIRE_FALSE(ChangeVideoClipSpeed(timeline, 1, 2));
      REQUIRE(timeline.clips[0].duration == Approx(3));
      REQUIRE(PictureAt(timeline, 5.5).sourceTime == Approx(3));
      REQUIRE(timeline.clips[0].ProjectTimeAtContentTime(6) == Approx(7));
      REQUIRE(SplitClipAt(timeline, 6));
      REQUIRE(timeline.clips[1].sourceStart == Approx(4));
      REQUIRE(timeline.clips[1].playbackRate == 2);
      REQUIRE(HealVideoClips(timeline, 5, 8));
      REQUIRE(timeline.clips[0].markers[0].position == 6);
      REQUIRE(ChangeVideoClipSpeed(timeline, 1, 1));
      REQUIRE(timeline.clips[0].start == 5);
      REQUIRE(timeline.clips[0].sourceStart == 2);
      REQUIRE(timeline.clips[0].duration == 6);
   }
   ChangeVideoClipSpeed(timeline, 1, 2);
   REQUIRE(DragVideoClipEdge(timeline, 1, true, 4));
   REQUIRE(timeline.clips[0].sourceStart == 0);
   REQUIRE(DragVideoClipEdge(timeline, 1, false, 10));
   REQUIRE(timeline.clips[0].duration == 6);
   REQUIRE(timeline.clips[0].markers[0].position == 6);
}

TEST_CASE("Retimed clipboard and gap edits preserve content coordinates", "[video-edits][video-speed]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("source.mp4", 8, 30);
   ChangeVideoClipSpeed(timeline, 1, 2);
   const auto copied = CopyVideoRange(timeline, 1, 3);
   REQUIRE(copied.clips[0].sourceStart == 2);
   REQUIRE(copied.clips[0].playbackRate == 2);
   REQUIRE(PictureAt(copied, 0.5).sourceTime == Approx(3));
   InsertVideoGap(timeline, 1, 1);
   REQUIRE_FALSE(PictureAt(timeline, 1.5).visible);
   REQUIRE(PictureAt(timeline, 2.25).sourceTime == Approx(2.5));
   EraseVideoRange(timeline, 2, 2.5, true);
   REQUIRE(PictureAt(timeline, 2.25).sourceTime == Approx(3.5));
   Timeline pasted;
   PasteVideoRange(pasted, 1, copied, 2);
   REQUIRE(PictureAt(pasted, 1.5).sourceTime == Approx(3));
}

TEST_CASE("Retiming plans repeat across their single publication boundary", "[video-edits][video-speed]")
{
   using namespace VideoTimeline;
   const VideoProjectSnapshot before{{{"rate", 4, CreateVideoTimeline("source.mp4", 8, 30)}}};
   const std::vector<VideoEditRequest> request{{VideoEditKind::ChangeClipSpeed, "rate", 1, 0, 0, 2}};
   for (bool after : {false, true}) {
      auto current = before;
      const auto plan = PlanVideoEdits(current, request);
      try {
         if (!after) throw std::runtime_error("interrupted before publication");
         ApplyVideoEditPlan(current, plan);
         throw std::runtime_error("interrupted after publication");
      }
      catch (const std::runtime_error &) {}
      REQUIRE(ApplyVideoEditPlan(current, plan) ==
         (after ? VideoEditApplication::AlreadyApplied : VideoEditApplication::Applied));
      REQUIRE(ApplyVideoEditPlan(current, plan) == VideoEditApplication::AlreadyApplied);
      REQUIRE(current.tracks[0].timeline.clips[0].duration == 4);
      REQUIRE(current.tracks[0].revision == 5);
      REQUIRE(PlanVideoEdits(current, request).ChangedTrackIds().empty());
   }
}

TEST_CASE("Invalid rates and collisions cannot publish a partial retiming", "[video-edits][video-speed]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("source.mp4", 8, 30);
   SplitClipAt(timeline, 4);
   const VideoProjectSnapshot before{{{"rate", 1, timeline}}};
   for (double rate : {0.0, -1.0, 0.001, 101.0, std::numeric_limits<double>::infinity(),
         std::numeric_limits<double>::quiet_NaN(), 0.5}) {
      REQUIRE_THROWS(ChangeVideoClipSpeed(timeline, 1, rate));
      REQUIRE(SameVideoProject(before, {{{"rate", 1, timeline}}}));
   }
}

TEST_CASE("Mixed-speed joins flatten and separate without changing source samples", "[video-edits][video-speed]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("red.mp4", 8, 30);
   SplitClipAt(timeline, 4);
   timeline.clips[0].linkGroup = std::string(32, 'a');
   timeline.clips[0].markers = {{std::string(32, 'c'), 2, "red flag"}};
   timeline.clips[1].media = {"blue.mp4", 8, 25};
   timeline.clips[1].linkGroup = std::string(32, 'b');
   timeline.clips[1].markers = {{std::string(32, 'd'), 6, "blue flag"}};
   ChangeVideoClipSpeed(timeline, 1, 2);
   ChangeVideoClipSpeed(timeline, 2, 0.5);
   JoinVideoClips(timeline, 0, 12);
   ChangeVideoClipSpeed(timeline, 1, 2);
   REQUIRE(timeline.End() == 6);
   const auto windows = VideoContentLinkWindows(timeline.clips[0]);
   REQUIRE(windows.size() == 2);
   REQUIRE(windows[0].playbackRate == 4);
   REQUIRE(windows[1].playbackRate == 1);
   REQUIRE(windows[1].start == 2);
   REQUIRE(windows[1].end == 6);
   struct Expected { double at; bool visible; std::string source; double time, rate; };
   std::vector<Expected> expected;
   for (double at : {0.25, 0.5, 1.5, 2.5, 3.0, 5.5}) {
      const auto picture = EvaluateVideoLayers({&timeline}, at);
      expected.push_back({at, picture.clip != nullptr, picture.source.path, picture.sourceTime, picture.playbackRate});
   }
   const auto flattened = ComposeVideoTracks({&timeline});
   REQUIRE(SeparateVideoClips(timeline, 0, 6));
   REQUIRE_FALSE(SeparateVideoClips(timeline, 0, 6));
   REQUIRE(timeline.clips[0].ProjectTimeAtContentTime(2) == Approx(0.5));
   REQUIRE(timeline.clips[1].ProjectTimeAtContentTime(6) == Approx(4));
   const Timeline *variants[]{&flattened, &timeline};
   for (const auto *variant : variants) for (const auto &item : expected) {
      const auto actual = EvaluateVideoLayers({variant}, item.at);
      REQUIRE(bool(actual.clip) == item.visible);
      if (!item.visible) continue;
      REQUIRE(actual.source.path == item.source);
      REQUIRE(actual.sourceTime == Approx(item.time));
      REQUIRE(actual.playbackRate == Approx(item.rate));
   }
}

TEST_CASE("Heal Split removes only continuous compatible seams and is repeatable", "[video-edits][video-join]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("source.mp4", 8, 30);
   timeline.clips[0].markers = {{std::string(32, 'a'), 6, "hidden flag"}};
   SplitClipAt(timeline, 3); SplitClipAt(timeline, 5);
   REQUIRE(HealVideoClips(timeline, 0, 8));
   REQUIRE(timeline.clips.size() == 1);
   REQUIRE(timeline.clips[0].id == 1);
   REQUIRE(timeline.clips[0].duration == 8);
   REQUIRE(timeline.clips[0].markers.size() == 1);
   REQUIRE_FALSE(HealVideoClips(timeline, 0, 8));
   SplitClipAt(timeline, 3); MoveClipTo(timeline, timeline.clips[1].id, 4);
   const auto before = VideoProjectSnapshot{{{"track", 0, timeline}}};
   REQUIRE_THROWS(HealVideoClips(timeline, 0, 9));
   REQUIRE(SameVideoProject(before, {{{"track", 0, timeline}}}));
}

TEST_CASE("Joined footage retains gaps source cuts outer handles and nested editability", "[video-edits][video-join]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("red.mp4", 10, 30);
   SplitClipAt(timeline, 4);
   TrimVideoClip(timeline, 1, 1, 3);
   TrimVideoClip(timeline, 2, 5, 8);
   const auto before = timeline;
   REQUIRE(JoinVideoClips(timeline, 0, 10));
   REQUIRE(timeline.clips.size() == 1);
   REQUIRE(timeline.clips[0].content);
   REQUIRE(timeline.clips[0].sourceStart == 1);
   REQUIRE_FALSE(PictureAt(timeline, 4).visible);
   REQUIRE(PictureAt(timeline, 5.5).sourceTime == Approx(5.5));
   REQUIRE_FALSE(JoinVideoClips(timeline, 0, 10));
   auto gapOnly = timeline;
   TrimVideoClip(gapOnly, 1, 3, 5);
   REQUIRE_THROWS(SeparateVideoClips(gapOnly, 0, 10));
   REQUIRE(gapOnly.clips[0].content);
   // Neither this join nor an outer trim can resurrect the deleted interior.
   TrimVideoClip(timeline, 1, 0, 10);
   REQUIRE_FALSE(PictureAt(timeline, 4).visible);
   REQUIRE(PictureAt(timeline, .5).sourceTime == Approx(.5));
   REQUIRE(PictureAt(timeline, 9).sourceTime == Approx(9));
   REQUIRE_FALSE(SampleVideoContent(timeline.clips[0], SourceFor(timeline, timeline.clips[0]), -1));
   REQUIRE_FALSE(SampleVideoContent(timeline.clips[0], SourceFor(timeline, timeline.clips[0]), 10));
   TrimVideoClip(timeline, 1, 1, 8);
   REQUIRE(SeparateVideoClips(timeline, 0, 10));
   REQUIRE(timeline.clips.size() == 2);
   for (size_t i{}; i < 2; ++i) {
      REQUIRE(timeline.clips[i].id == before.clips[i].id);
      REQUIRE(timeline.clips[i].start == before.clips[i].start);
      REQUIRE(timeline.clips[i].sourceStart == before.clips[i].sourceStart);
      REQUIRE(timeline.clips[i].duration == before.clips[i].duration);
   }
   REQUIRE_FALSE(SeparateVideoClips(timeline, 0, 10));
}

TEST_CASE("Compounds use actual leaf media in playback export copy and relink", "[video-edits][video-join]")
{
   using namespace VideoTimeline;
   auto upper = CreateVideoTimeline("red.mp4", 4, 30);
   auto blue = CreateVideoTimeline("blue.mp4", 3, 24);
   PasteVideoRange(upper, 5, blue, 3);
   REQUIRE(JoinVideoClips(upper, 0, 8));
   auto lower = CreateVideoTimeline("green.mp4", 10, 25);
   auto mix = ComposeVideoTracks({&upper, &lower});
   REQUIRE(SourceFor(mix, *ClipAt(mix, 2)).path == "red.mp4");
   REQUIRE(SourceFor(mix, *ClipAt(mix, 4.5)).path == "green.mp4");
   REQUIRE(SourceFor(mix, *ClipAt(mix, 6)).path == "blue.mp4");
   REQUIRE(PictureAt(mix, 6).sourceTime == Approx(1));
   auto copied = CopyVideoRange(upper, 2, 7);
   REQUIRE(copied.clips[0].content == upper.clips[0].content);
   REQUIRE(ReplaceVideoSource(copied, "blue.mp4", {"moved.mp4", 3, 24}));
   REQUIRE(EvaluateVideoLayers({&copied}, 4).source.path == "moved.mp4");
   REQUIRE(EvaluateVideoLayers({&upper}, 6).source.path == "blue.mp4");
   REQUIRE_THROWS(ReplaceVideoSource(copied, "moved.mp4", {"short.mp4", 1, 24}));
   REQUIRE(EvaluateVideoLayers({&copied}, 4).source.path == "moved.mp4");
   SplitClipAt(copied, 3.5);
   REQUIRE(copied.clips[1].content);
   REQUIRE(JoinVideoClips(copied, 0, 5));
   REQUIRE(copied.clips[0].content->parts[0].content);
   REQUIRE(SeparateVideoClips(copied, 0, 5));
   REQUIRE(copied.clips.size() == 3);
   REQUIRE(std::none_of(copied.clips.begin(), copied.clips.end(), [](const auto &clip) { return bool(clip.content); }));
   REQUIRE_FALSE(SeparateVideoClips(copied, 0, 5));
}

TEST_CASE("Compound flags keep occurrences hidden footage and edits after separation", "[video-edits][video-join]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("flags.mp4", 8, 30);
   timeline.clips[0].markers = {{std::string(32, 'a'), 1, "first"}, {std::string(32, 'b'), 6, "second"}};
   SplitClipAt(timeline, 4);
   timeline.clips[1].media = SourceFor(timeline, timeline.clips[1]);
   REQUIRE(JoinVideoClips(timeline, 0, 8));
   REQUIRE(timeline.clips[0].markers.size() == 2);
   ClipMarkers::PutMarker(timeline.clips[0].markers, {std::string(32, 'b'), 7, "changed"});
   TrimVideoClip(timeline, 1, 2, 8);
   REQUIRE(SeparateVideoClips(timeline, 0, 8));
   const auto first = std::find_if(timeline.clips[0].markers.begin(), timeline.clips[0].markers.end(),
      [](const auto &marker) { return marker.id == std::string(32, 'a'); });
   REQUIRE(first != timeline.clips[0].markers.end());
   REQUIRE(first->position == 1);
   const auto second = std::find_if(timeline.clips[1].markers.begin(), timeline.clips[1].markers.end(),
      [](const auto &marker) { return marker.id == std::string(32, 'b'); });
   REQUIRE(second != timeline.clips[1].markers.end());
   REQUIRE(second->text == "changed");
   REQUIRE(second->position == 7);
}

TEST_CASE("Structural plans converge around publication and reject stale nested content", "[video-edits][video-join]")
{
   using namespace VideoTimeline;
   for (const auto kind : {VideoEditKind::HealClips, VideoEditKind::JoinClips, VideoEditKind::SeparateClips}) {
      auto timeline = CreateVideoTimeline("source.mp4", 8, 30); SplitClipAt(timeline, 4);
      if (kind == VideoEditKind::SeparateClips) JoinVideoClips(timeline, 0, 8);
      const VideoProjectSnapshot original{{{"track", 1, timeline}}};
      for (const bool publish : {false, true}) {
         auto project = original;
         const std::vector<VideoEditRequest> request{{kind, "track", 0, 0, 8}};
         const auto plan = PlanVideoEdits(project, request);
         if (publish) ApplyVideoEditPlan(project, plan);
         const auto fresh = PlanVideoEdits(project, request);
         ApplyVideoEditPlan(project, fresh);
         REQUIRE(PlanVideoEdits(project, request).ChangedTrackIds().empty());
         REQUIRE(project.tracks[0].revision == 2);
      }
   }
   auto timeline = CreateVideoTimeline("source.mp4", 8, 30); SplitClipAt(timeline, 4);
   JoinVideoClips(timeline, 0, 8);
   VideoProjectSnapshot project{{{"track", 1, timeline}}};
   auto hostCopy = project;
   // The XML boundary may include representative media metadata on a joined
   // node. Only its owned leaves determine picture identity and stale edits.
   hostCopy.tracks[0].timeline.clips[0].media = {"representative.mp4", 99, 24};
   REQUIRE(SameVideoProject(project, hostCopy));
   const auto plan = PlanVideoEdits(project, {{VideoEditKind::MoveClip, "track", 1, 2}});
   ReplaceVideoSource(project.tracks[0].timeline, "source.mp4", {"changed.mp4", 8, 30});
   REQUIRE_THROWS(ApplyVideoEditPlan(project, plan));
}

TEST_CASE("Malformed compounds and excessive nesting cannot publish", "[video-edits][video-join]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("source.mp4", 8, 30);
   auto content = std::make_shared<VideoClipContent>(); content->duration = 8;
   content->parts = timeline.clips;
   timeline.clips[0].content = content;
   REQUIRE_THROWS(RequireWellFormedTimeline(timeline)); // Unresolved child source.
   content->parts[0].media = {"source.mp4", 8, 30};
   REQUIRE_NOTHROW(RequireWellFormedTimeline(timeline));
   for (int i{}; i < 18; ++i) {
      auto outer = std::make_shared<VideoClipContent>(); outer->duration = 8;
      outer->parts = timeline.clips; timeline.clips[0].content = std::move(outer);
   }
   REQUIRE_THROWS(RequireWellFormedTimeline(timeline));
}

TEST_CASE("Joined soundtrack windows retain physical source edges through complete hiding", "[video-edits][video-join]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("source.mp4", 4, 30);
   timeline.clips[0].linkGroup = std::string(32, 'a');
   SplitClipAt(timeline, 2); timeline.clips[1].linkGroup = std::string(32, 'b');
   JoinVideoClips(timeline, 0, 4); MoveClipTo(timeline, 1, 2);
   TrimVideoClip(timeline, 1, 5, 6);
   auto windows = VideoContentLinkWindows(timeline.clips[0]);
   REQUIRE(windows.size() == 2);
   REQUIRE(windows[0].start == 4);
   REQUIRE(windows[0].end == 4);
   REQUIRE(windows[1].start == 5);
   REQUIRE(windows[1].sourceOrigin == 2);
   REQUIRE(windows[1].sourceEnd == 6);
   TrimVideoClip(timeline, 1, 2, 6);
   windows = VideoContentLinkWindows(timeline.clips[0]);
   REQUIRE(windows[0].start == 2);
   REQUIRE(windows[0].end == 4);
   REQUIRE(windows[1].start == 4);
   REQUIRE(windows[1].end == windows[1].sourceEnd);
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
