// Dedicated composition geometry/eligibility cases, independent of pixel codecs.
#include "EvaluateVideoTimeline.h"
#include "RenderVideoComposition.h"
#include "PlanVideoExport.h"
#include "JoinVideoClips.h"
#include <catch2/catch.hpp>
#include <limits>

using namespace VideoEditing;
namespace {
VideoProjectSnapshot Project()
{
   return {{{"front", 0, VideoTimeline::CreateVideoTimeline("front.mp4", 10, 30, 1920, 1080)},
      {"back", 0, VideoTimeline::CreateVideoTimeline("back.mp4", 20, 24, 1280, 720)}}};
}
VideoCompositionSettings Settings() { return {1920, 1080, 25}; }
}

TEST_CASE("Retimed picture plans and flat exports agree on source clocks", "[video-composition][video-speed]")
{
   auto project = Project();
   auto &front = project.tracks[0].timeline;
   VideoTimeline::MoveClipTo(front, 1, 2);
   VideoTimeline::ChangeVideoClipSpeed(front, 1, 2);
   const auto frame = EvaluateVideoTimeline(project, Settings(), 3.25);
   REQUIRE(frame.layers.size() == 2);
   REQUIRE(frame.layers.back().sourceTime == Approx(2.5));
   REQUIRE(frame.layers.back().playbackRate == 2);
   const auto flattened = VideoTimeline::ComposeVideoTracks({&front, &project.tracks[1].timeline});
   const auto exported = VideoTimeline::EvaluateVideoLayers({&flattened}, 3.25);
   REQUIRE(exported.source.path == frame.layers.back().source.path);
   REQUIRE(exported.sourceTime == Approx(frame.layers.back().sourceTime));
   REQUIRE(exported.playbackRate == frame.layers.back().playbackRate);
   REQUIRE(EvaluateVideoTimeline(project, Settings(), 7.5).layers.size() == 1);
}

TEST_CASE("Composition retains lower layers and resolves source frames across gaps", "[video-composition]")
{
   auto project = Project();
   VideoTimeline::MoveClipTo(project.tracks[0].timeline, 1, 2);
   const auto settings = Settings();
   const auto gap = EvaluateVideoTimeline(project, settings, 1);
   REQUIRE(gap.layers.size() == 1);
   REQUIRE(gap.layers[0].trackId == "back");
   const auto frame = EvaluateVideoTimeline(project, settings, 3);
   REQUIRE(frame.layers.size() == 2);
   REQUIRE(frame.layers[0].trackId == "back");
   REQUIRE(frame.layers[1].trackId == "front");
   REQUIRE(frame.layers[1].sourceTime == 1);
   REQUIRE(EvaluateVideoTimeline(project, settings, 24).layers.empty());
   REQUIRE(EvaluateVideoTimeline(project, settings, 25).layers.empty());
}

TEST_CASE("Spatial crop retains source pixels and timing independently of proxy resolution", "[video-composition]")
{
   auto project = Project();
   auto settings = Settings();
   project.tracks[0].timeline.clips[0].crop = {0.1, 0.2, 0.3, 0.1};
   const auto before = project;
   auto frame = EvaluateVideoTimeline(project, settings, 4);
   REQUIRE(frame.layers.back().crop.left == 0.1);
   REQUIRE(frame.layers.back().crop.bottom == 0.1);
   REQUIRE(frame.layers.back().rectangle.width == 1);
   REQUIRE(frame.layers.back().sourceTime == 4);
   REQUIRE(frame.duration == 25);
   REQUIRE(SameVideoProject(project, before));
   project.tracks[0].timeline.sourceWidth = 640;
   project.tracks[0].timeline.sourceHeight = 360;
   frame = EvaluateVideoTimeline(project, settings, 4);
   REQUIRE(frame.layers.back().crop.left == 0.1);
   REQUIRE(frame.layers.back().sourceTime == 4);
   project.tracks[0].timeline.clips[0].crop.right = 0.9;
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 4));
   project.tracks[0].timeline.clips[0].crop = {0, 0, 0, std::numeric_limits<double>::quiet_NaN()};
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 4));
}

TEST_CASE("Embedded layers resolve nested rectangles by parent identity and hide with parent gaps", "[video-composition]")
{
   auto project = Project();
   project.tracks.push_back({"child", 0, VideoTimeline::CreateVideoTimeline("child.mp4", 10, 25), "front"});
   project.tracks.push_back({"nested", 0, VideoTimeline::CreateVideoTimeline("nested.mp4", 10, 60), "child"});
   auto settings = Settings();
   settings.layers = {{"child", {0.5, 0.5, 0.4, 0.4}}, {"nested", {0.25, 0, 0.5, 0.5}}};
   const auto frame = EvaluateVideoTimeline(project, settings, 1);
   REQUIRE(frame.layers.size() == 4);
   REQUIRE(frame.layers[0].trackId == "back");
   REQUIRE(frame.layers[1].trackId == "front");
   REQUIRE(frame.layers[2].trackId == "child");
   REQUIRE(frame.layers[3].trackId == "nested");
   REQUIRE(frame.layers[3].rectangle.x == Approx(0.6));
   REQUIRE(frame.layers[3].rectangle.y == Approx(0.5));
   REQUIRE(frame.layers[3].rectangle.width == Approx(0.2));
   VideoTimeline::MoveClipTo(project.tracks[0].timeline, 1, 3);
   REQUIRE(EvaluateVideoTimeline(project, settings, 1).layers.size() == 1);
   project.tracks[2].parentTrackId = "nested";
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 1));
}

TEST_CASE("Mute wins over solo and never changes declared output duration", "[video-composition]")
{
   auto project = Project();
   project.tracks.push_back({"child", 0, VideoTimeline::CreateVideoTimeline("child.mp4", 10, 25), "front"});
   auto settings = Settings();
   settings.layers = {{"child", {}, false, true}};
   auto frame = EvaluateVideoTimeline(project, settings, 1);
   REQUIRE(frame.layers.size() == 2); // bound group solos together
   REQUIRE(frame.layers[0].trackId == "front");
   settings.layers.push_back({"front", {}, true});
   frame = EvaluateVideoTimeline(project, settings, 1);
   REQUIRE(frame.layers.empty());
   REQUIRE(frame.duration == 25);
   REQUIRE(frame.canvasWidth == 1920);
   settings.layers[0].solo = false;
   settings.layers.push_back({"back", {}, false, false, {{1, true}}});
   REQUIRE(EvaluateVideoTimeline(project, settings, 1).layers.empty());
   settings.layers[1].muted = false;
   settings.layers[0].clips = {{1, true}};
   frame = EvaluateVideoTimeline(project, settings, 1);
   REQUIRE(frame.layers.size() == 1);
   REQUIRE(frame.layers[0].trackId == "front");
}

TEST_CASE("Native layer selection shares mute and solo precedence with composition and flattened export", "[video-composition]")
{
   auto project = Project();
   auto settings = Settings();
   settings.layers = {{"front"}, {"back"}};
   const auto before = project;
   for (const auto front : {VideoTimeline::VideoLayerVisibility{},
         VideoTimeline::VideoLayerVisibility{true, false}, VideoTimeline::VideoLayerVisibility{false, true},
         VideoTimeline::VideoLayerVisibility{true, true}}) {
      for (const auto back : {VideoTimeline::VideoLayerVisibility{},
            VideoTimeline::VideoLayerVisibility{false, true}, VideoTimeline::VideoLayerVisibility{true, true}}) {
         settings.layers[0].muted = front.muted; settings.layers[0].solo = front.solo;
         settings.layers[1].muted = back.muted; settings.layers[1].solo = back.solo;
         for (int repeat = 0; repeat < 3; ++repeat) {
            const auto visible = VideoTimeline::VisibleVideoLayers({
               {&project.tracks[0].timeline, front}, {&project.tracks[1].timeline, back}});
            const auto preview = VideoTimeline::EvaluateVideoLayers(visible, 4);
            const auto exportTimeline = VideoTimeline::ComposeVideoTracks(visible);
            const auto exported = VideoTimeline::EvaluateVideoLayers({&exportTimeline}, 4);
            const auto composition = EvaluateVideoTimeline(project, settings, 4);
            REQUIRE(bool(preview.clip) == !composition.layers.empty());
            REQUIRE(bool(exported.clip) == bool(preview.clip));
            if (preview.clip) {
               REQUIRE(preview.source.path == composition.layers.back().source.path);
               REQUIRE(exported.source.path == preview.source.path);
               REQUIRE(exported.sourceTime == Approx(preview.sourceTime));
            }
            REQUIRE(composition.duration == 25);
            REQUIRE(SameVideoProject(project, before));
         }
      }
   }
   project.tracks[0].timeline.clips.clear();
   const auto emptySolo = VideoTimeline::VisibleVideoLayers({
      {&project.tracks[0].timeline, {false, true}}, {&project.tracks[1].timeline, {}}});
   REQUIRE_FALSE(VideoTimeline::EvaluateVideoLayers(emptySolo, 4).clip);
   REQUIRE_THROWS(VideoTimeline::VisibleVideoLayers({{nullptr, {true, false}}}));
}

TEST_CASE("Composition rejects stale properties and invalid geometry before producing a frame", "[video-composition]")
{
   const auto project = Project();
   auto settings = Settings();
   settings.layers = {{"front", {0.8, 0, 0.4, 1}}};
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 0));
   settings.layers = {{"missing"}};
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 0));
   settings.layers = {{"front"}, {"front"}};
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 0));
   settings.layers = {{"front", {}, false, false, {{22}}}};
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 0));
   settings.layers.clear(); settings.canvasWidth = 0;
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 0));
}

TEST_CASE("Frame planning repeats and interruptions cannot mutate authoritative project state", "[video-composition]")
{
   auto project = Project();
   auto settings = Settings();
   project.tracks[0].timeline.clips[0].crop = {0.1, 0.1, 0.1, 0.1};
   const auto before = project;
   for (int i = 0; i < 50; ++i) {
      // Drop a plan before consumption, or consume its complete owned values.
      auto frame = EvaluateVideoTimeline(project, settings, 1.25);
      REQUIRE(frame.layers.back().sourceTime == 1.25);
      REQUIRE(frame.layers.back().crop.top == 0.1);
      REQUIRE(SameVideoProject(project, before));
   }
   auto owned = EvaluateVideoTimeline(project, settings, 1);
   project.tracks.clear();
   REQUIRE(owned.layers.back().source.path == "front.mp4");
   REQUIRE(owned.layers.back().clipId == 1);
}

TEST_CASE("Picture crop belongs to retained clips through join separate and copying", "[video-composition][video-crop]")
{
   auto project = Project();
   auto &timeline = project.tracks[0].timeline;
   timeline.clips[0].duration = 5;
   timeline.clips[0].crop = {0.25, 0, 0, 0};
   auto second = timeline.clips[0];
   second.id = timeline.nextId++; second.start = 5; second.sourceStart = 5;
   second.crop = {0, 0, 0.5, 0};
   timeline.clips.push_back(second);
   REQUIRE(VideoTimeline::JoinVideoClips(timeline, 0, 10));
   REQUIRE(VideoTimeline::ChangeVideoClipCrop(timeline, timeline.clips[0].id, {0, 0.2, 0, 0.1}));
   const auto before = project;
   const auto left = EvaluateVideoTimeline(project, Settings(), 1).layers.back();
   const auto right = EvaluateVideoTimeline(project, Settings(), 6).layers.back();
   REQUIRE(left.crop.left == Approx(0.25));
   REQUIRE(right.crop.right == Approx(0.5));
   REQUIRE(left.crop.top == Approx(0.2));
   REQUIRE_FALSE(VideoTimeline::ChangeVideoClipCrop(timeline, timeline.clips[0].id, {0, 0.2, 0, 0.1}));
   REQUIRE(SameVideoProject(project, before));
   REQUIRE(VideoTimeline::SeparateVideoClips(timeline, 0, 10));
   REQUIRE_FALSE(VideoTimeline::SeparateVideoClips(timeline, 0, 10));
   REQUIRE(EvaluateVideoTimeline(project, Settings(), 1).layers.back().crop == left.crop);
   REQUIRE(EvaluateVideoTimeline(project, Settings(), 6).layers.back().crop == right.crop);
   const auto copied = VideoTimeline::CopyVideoRange(timeline, 0.5, 9);
   REQUIRE(VideoTimeline::EvaluateVideoLayers({&copied}, 0.5).crop == left.crop);
   REQUIRE(VideoTimeline::EvaluateVideoLayers({&copied}, 5.5).crop == right.crop);
}

TEST_CASE("Rejected picture crops publish no edit and cropped pictures cannot be copied unchanged", "[video-composition][video-crop]")
{
   auto project = Project();
   auto &timeline = project.tracks[0].timeline;
   const auto before = project;
   REQUIRE_THROWS(VideoTimeline::ChangeVideoClipCrop(timeline, 1, {0.5, 0, 0.5, 0}));
   REQUIRE_THROWS(VideoTimeline::ChangeVideoClipCrop(timeline, 1, {0, -0.1, 0, 0}));
   REQUIRE(SameVideoProject(project, before));
   REQUIRE(VideoTimeline::KeepsOriginalPicture(timeline));
   for (int repeat = 0; repeat < 3; ++repeat) {
      auto candidate = timeline;
      REQUIRE(VideoTimeline::ChangeVideoClipCrop(candidate, 1, {0.1, 0.2, 0, 0}));
      REQUIRE(SameVideoProject(project, before)); // discard before publication
   }
   REQUIRE(VideoTimeline::ChangeVideoClipCrop(timeline, 1, {0.1, 0.2, 0, 0}));
   REQUIRE_FALSE(VideoTimeline::KeepsOriginalPicture(timeline));
   const auto flattened = VideoTimeline::ComposeVideoTracks({&timeline});
   REQUIRE(VideoTimeline::EvaluateVideoLayers({&flattened}, 2).crop ==
      VideoTimeline::EvaluateVideoLayers({&timeline}, 2).crop);
}

TEST_CASE("Oriented picture crops map to original and proxy decoded pixels", "[video-composition][video-crop]")
{
   const VideoFrameCrop crop{0.1, 0.2, 0.3, 0.1};
   const auto original = CalculateVideoCropRectangle(crop, 100, 50, 0);
   const auto proxy = CalculateVideoCropRectangle(crop, 50, 25, 0);
   REQUIRE(original.x == 10); REQUIRE(original.y == 10);
   REQUIRE(original.width == 60); REQUIRE(original.height == 35);
   REQUIRE(proxy.x == 5); REQUIRE(proxy.y == 5);
   REQUIRE(proxy.width == 30); REQUIRE(proxy.height == 18);
   const auto rotated = CalculateVideoCropRectangle({0, 0, 0, 0.5}, 160, 90, 270);
   REQUIRE(rotated.x == 80); REQUIRE(rotated.y == 0);
   REQUIRE(rotated.width == 80); REQUIRE(rotated.height == 90);
   const auto other = CalculateVideoCropRectangle({0.2, 0.1, 0.3, 0.4}, 160, 90, 90);
   REQUIRE(other.x == 16); REQUIRE(other.y == 27);
   REQUIRE(other.width == 80); REQUIRE(other.height == 45);
   REQUIRE_THROWS(CalculateVideoCropRectangle(crop, 0, 50, 0));
   REQUIRE_THROWS(CalculateVideoCropRectangle(crop, 100, 50, 45));
}

TEST_CASE("Automatic export canvas removes cropped margins rather than restoring them", "[video-composition][video-crop]")
{
   using VideoExport::CalculateVideoExportCanvas;
   REQUIRE((CalculateVideoExportCanvas(720, 1280, {0, 0.2, 0, 0}) == std::pair<int, int>{720, 1024}));
   REQUIRE((CalculateVideoExportCanvas(1920, 1080, {0.25, 0, 0.25, 0}) == std::pair<int, int>{960, 1080}));
   REQUIRE((CalculateVideoExportCanvas(100, 50, {0.1, 0.2, 0.3, 0.1}) == std::pair<int, int>{60, 34}));
   REQUIRE((CalculateVideoExportCanvas(1, 1, {}) == std::pair<int, int>{2, 2}));
   REQUIRE_THROWS(CalculateVideoExportCanvas(720, 1280, {0.5, 0, 0.5, 0}));
}

TEST_CASE("Export spans share a global frame clock across cuts gaps and rate changes", "[video-composition][video-export-timing]")
{
   using namespace VideoTimeline;
   for (const auto outputRate : {24.0, 30000.0 / 1001, 30.0, 60.0}) {
      auto timeline = CreateVideoTimeline("clock.mp4", 10, 24000.0 / 1001);
      for (int frame = 6; frame < 216; frame += 6) SplitClipAt(timeline, frame * 1001.0 / 24000);
      const auto original = timeline;
      for (int repeat = 0; repeat < 3; ++repeat) {
         const auto plan = VideoExport::PlanVideoExportFrames(timeline, 10, outputRate);
         int64_t cursor{};
         for (const auto &span : plan) {
            REQUIRE(span.firstFrame == cursor);
            REQUIRE(span.frameCount > 0);
            cursor += span.frameCount;
            const auto clip = std::find_if(timeline.clips.begin(), timeline.clips.end(),
               [&](const auto &value) { return value.id == span.clipId; });
            REQUIRE(clip != timeline.clips.end());
            REQUIRE(span.sourceStart == Approx(clip->ContentTimeAtProjectTime(span.firstFrame / outputRate)));
         }
         REQUIRE(cursor == static_cast<int64_t>(std::ceil(10 * outputRate - 1e-7)));
         REQUIRE(timeline.clips.size() == original.clips.size());
         REQUIRE(timeline.clips.back().start == original.clips.back().start);
      }
   }
   auto timeline = CreateVideoTimeline("gap.mp4", 2, 30);
   // Saved/mixed-rate project positions need not lie on the output frame grid.
   timeline.clips[0].start = 0.25;
   ChangeVideoClipSpeed(timeline, 1, 2);
   const auto plan = VideoExport::PlanVideoExportFrames(timeline, 2, 30);
   REQUIRE(plan.size() == 3);
   REQUIRE(plan[0].clipId == 0); REQUIRE(plan[0].frameCount == 8);
   REQUIRE(plan[1].firstFrame == 8); REQUIRE(plan[1].frameCount == 30);
   REQUIRE(plan[1].sourceStart == Approx((8.0 / 30 - 0.25) * 2));
   REQUIRE(plan[2].clipId == 0); REQUIRE(plan[2].firstFrame == 38); REQUIRE(plan[2].frameCount == 22);
   REQUIRE_THROWS(VideoExport::PlanVideoExportFrames(timeline, 2, 0));
   REQUIRE_THROWS(VideoExport::PlanVideoExportFrames(timeline, 0.5, 30));
   REQUIRE_THROWS(VideoExport::PlanVideoExportFrames(timeline, std::numeric_limits<double>::max(), 30));
}

TEST_CASE("Automatic export retains positive source rates below one frame per second", "[video-composition][video-export-timing]")
{
   for (const auto rate : {0.5, 0.125, 1e-12}) {
      auto timeline = VideoTimeline::CreateVideoTimeline("timelapse.mp4", 10, rate);
      timeline.clips[0].crop.top = 0.2;
      for (int repeat = 0; repeat < 2; ++repeat) {
         const auto plan = VideoExport::PlanVideoExportFrames(timeline, 10, rate);
         REQUIRE(plan.size() == 1);
         REQUIRE(plan[0].clipId == timeline.clips[0].id);
         REQUIRE(plan[0].firstFrame == 0);
         REQUIRE(plan[0].frameCount == static_cast<int64_t>(std::ceil(10 * rate)));
         REQUIRE(plan[0].sourceStart == 0);
         REQUIRE(plan[0].sourceEnd == 10);
      }
   }
}
