// Dedicated composition geometry/eligibility cases, independent of pixel codecs.
#include "EvaluateVideoTimeline.h"
#include "RenderVideoComposition.h"
#include "PlanVideoExport.h"
#include "PlanVideoTransitions.h"
#include "../update/ChooseProductUpdate.h"
#include "JoinVideoClips.h"
#include <catch2/catch.hpp>
#include <limits>

using namespace VideoEditing;
TEST_CASE("Separating nested picture envelopes preserves their product and saved content clocks", "[video-composition][video-fades]")
{
   auto timeline = VideoTimeline::CreateVideoTimeline("neutral.mp4", 4, 30, 160, 100);
   timeline.clips.front().opacity = {0, 1, 4, 1};
   VideoTimeline::SplitClipAt(timeline, .5);
   VideoTimeline::JoinVideoClips(timeline, 0, 4);
   timeline.clips.front().opacity = {0, 2, 4, .5};
   const auto original = timeline;
   REQUIRE(VideoTimeline::SeparateVideoClips(timeline, 0, 4));
   REQUIRE_FALSE(VideoTimeline::SeparateVideoClips(timeline, 0, 4));
   for (const auto at : {.1, .4, .5, .75, 1.5, 2.5, 3.75, 3.99}) {
      const auto *a = VideoTimeline::ClipAt(original, at), *b = VideoTimeline::ClipAt(timeline, at);
      REQUIRE(a); REQUIRE(b);
      const auto before = VideoTimeline::SampleVideoContent(*a, VideoTimeline::SourceFor(original, *a), a->ContentTimeAtProjectTime(at));
      const auto after = VideoTimeline::SampleVideoContent(*b, VideoTimeline::SourceFor(timeline, *b), b->ContentTimeAtProjectTime(at));
      REQUIRE(after.opacity == Approx(before.opacity)); REQUIRE(after.sourceTime == Approx(before.sourceTime));
   }
   for (const auto &clip : timeline.clips) {
      REQUIRE_FALSE(clip.opacityModifiers.empty());
      REQUIRE(DecodeVideoOpacityRamps(EncodeVideoOpacityRamps(clip.opacityModifiers)) == clip.opacityModifiers);
   }
   for (const auto *text : {"1 0 0 1", "1 0 1 2", "2 0 1 1", "0 trailing", "999999", "1 nan 2 0"})
      REQUIRE_THROWS_AS(DecodeVideoOpacityRamps(text), std::runtime_error);
}
TEST_CASE("Picture opacity stays anchored through retained cuts and speed changes", "[video-composition][video-fades]")
{
   auto timeline = VideoTimeline::CreateVideoTimeline("neutral.mp4", 16, 30, 640, 360);
   auto clip = timeline.clips.front(); clip.start = 2; clip.duration = 8;
   clip.opacity = {0, 2, 8, 2};
   const auto source = VideoTimeline::SourceFor(timeline, clip);
   VideoTimeline::RequireUsableVideoContent(clip, source);
   for (int repeat{}; repeat < 2; ++repeat) {
      REQUIRE(VideoTimeline::SampleVideoContent(clip, source, .5).opacity == Approx(.25));
      auto right = clip; right.start = 5; right.sourceStart = 3; right.duration = 5;
      REQUIRE(VideoTimeline::SampleVideoContent(right, source, 3).opacity == 1);
      const auto fade = VideoTimeline::SampleVideoContent(right, source, 7);
      REQUIRE(fade.opacity == Approx(.5));
      REQUIRE(fade.opacityRamps.back().start == Approx(8));
      REQUIRE(fade.opacityRamps.back().end == Approx(10));
      auto fast = clip; fast.playbackRate = 2; fast.duration = 4;
      const auto sample = VideoTimeline::SampleVideoContent(fast, source, 1);
      REQUIRE(sample.opacity == Approx(.5));
      REQUIRE(sample.opacityRamps.front().start == Approx(2));
      REQUIRE(sample.opacityRamps.front().end == Approx(3));
   }
   auto different = clip; different.opacity.inDuration = 1;
   REQUIRE_FALSE(VideoTimeline::SameVideoContent(clip, different));
   clip.opacity.outEnd = 17;
   REQUIRE_THROWS(VideoTimeline::RequireUsableVideoContent(clip, source));
}

TEST_CASE("Nested fades multiply in one project clock without moving source samples", "[video-composition][video-fades]")
{
   auto source = VideoTimeline::CreateVideoTimeline("neutral.mp4", 16, 30, 640, 360);
   auto child = source.clips.front(); child.start = 2; child.duration = 8; child.opacity = {0, 2, 8, 0};
   child.media = VideoTimeline::SourceFor(source, child);
   auto content = std::make_shared<VideoTimeline::VideoClipContent>();
   content->duration = 10; content->parts.push_back(child);
   VideoTimeline::Clip joined{1, 10, 0, 10}; joined.content = content; joined.opacity = {0, 4, 0, 0};
   VideoTimeline::RequireUsableVideoContent(joined, {});
   const auto sample = VideoTimeline::SampleVideoContent(joined, {}, 2.5);
   REQUIRE(sample.sourceTime == Approx(.5));
   REQUIRE(sample.opacity == Approx(.625 * .25));
   REQUIRE(sample.opacityRamps.size() == 2);
   REQUIRE(sample.opacityRamps[0].start == 10);
   REQUIRE(sample.opacityRamps[1].start == 12);
}

TEST_CASE("Crossfade plans reveal lower video and keep every output frame on the project clock", "[video-composition][video-fades]")
{
   auto top = VideoTimeline::CreateVideoTimeline("red.mp4", 8, 30, 640, 360);
   auto bottom = VideoTimeline::CreateVideoTimeline("blue.mp4", 12, 24, 640, 360);
   top.clips.front().opacity = {0, 2, 8, 2};
   const auto plan = PlanVideoTransitions({&top, &bottom}, 12, 30);
   REQUIRE(plan.size() == 4);
   REQUIRE(plan[0].firstFrame == 0); REQUIRE(plan[0].frameCount == 60);
   REQUIRE(plan[0].requiresBlend); REQUIRE(plan[0].layers.size() == 2);
   REQUIRE(plan[0].layers[0].source.path == "blue.mp4");
   REQUIRE(plan[0].layers[1].source.path == "red.mp4");
   REQUIRE_FALSE(plan[1].requiresBlend); REQUIRE(plan[1].layers.size() == 1);
   REQUIRE(plan[2].requiresBlend); REQUIRE(plan[2].firstFrame == 180);
   REQUIRE_FALSE(plan[3].requiresBlend); REQUIRE(plan[3].layers[0].source.path == "blue.mp4");
   std::int64_t frames{};
   for (const auto &span : plan) { REQUIRE(span.firstFrame == frames); frames += span.frameCount; }
   REQUIRE(frames == 360);
   top.clips[0].muted = true;
   for (const auto &span : PlanVideoTransitions({&top, &bottom}, 12, 30)) {
      REQUIRE_FALSE(span.requiresBlend); REQUIRE(span.layers.size() == 1);
      REQUIRE(span.layers[0].source.path == "blue.mp4");
   }
}

namespace {
VideoProjectSnapshot Project()
{
   return {{{"front", 0, VideoTimeline::CreateVideoTimeline("front.mp4", 10, 30, 1920, 1080)},
      {"back", 0, VideoTimeline::CreateVideoTimeline("back.mp4", 20, 24, 1280, 720)}}};
}
VideoCompositionSettings Settings() { return {1920, 1080, 25}; }
}

TEST_CASE("Picture crop handles retain an editable nonempty rectangle at every boundary", "[video-composition][video-crop]")
{
   const VideoFrameCrop original{.1, .2, .3, .1};
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto left = DragVideoCropHandle(original, CropLeft, 20, 0, .001, .002);
      REQUIRE(left.left == Approx(.699)); REQUIRE(left.right == original.right);
      const auto corner = DragVideoCropHandle(original, CropRight | CropBottom, -20, -20, .001, .002);
      REQUIRE(corner.right == Approx(.899)); REQUIRE(corner.bottom == Approx(.798));
      const auto restored = DragVideoCropHandle(left, CropLeft, -20, 0, .001, .002);
      REQUIRE(restored.left == 0); REQUIRE(restored.right == original.right);
      const auto moved = DragVideoCropHandle(original, CropMove, 20, -20, .001, .002);
      REQUIRE(moved.left == Approx(.4)); REQUIRE(moved.right == 0);
      REQUIRE(moved.top == 0); REQUIRE(moved.bottom == Approx(.3));
      REQUIRE(1 - moved.left - moved.right == Approx(1 - original.left - original.right));
      REQUIRE(1 - moved.top - moved.bottom == Approx(1 - original.top - original.bottom));
      REQUIRE(DragVideoCropHandle(original, CropMove, 0, 0, .001, .002) == original);
   }
   REQUIRE_THROWS(DragVideoCropHandle(original, 0, 0, 0, .001, .002));
   REQUIRE_THROWS(DragVideoCropHandle(original, CropLeft, 0, 0, 0, .002));
   REQUIRE_THROWS(DragVideoCropHandle(original, CropLeft, std::numeric_limits<double>::quiet_NaN(), 0, .001, .002));
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
   VideoTimeline::ChangeVideoClipMute(project.tracks[1].timeline, 1, true);
   REQUIRE(EvaluateVideoTimeline(project, settings, 1).layers.empty());
   settings.layers[1].muted = false;
   VideoTimeline::ChangeVideoClipMute(project.tracks.back().timeline, 1, true);
   frame = EvaluateVideoTimeline(project, settings, 1);
   REQUIRE(frame.layers.size() == 1);
   REQUIRE(frame.layers[0].trackId == "front");
}

TEST_CASE("Clip picture mute retains source ranges and exposes lower layers", "[video-composition][video-clip-mute]")
{
   using namespace VideoTimeline;
   auto project = Project();
   auto &front = project.tracks[0].timeline;
   auto &back = project.tracks[1].timeline;
   REQUIRE(SplitClipAt(front, 5));
   const auto original = project;
   REQUIRE(ChangeVideoClipMute(front, front.clips[0].id, true));
   const auto muted = project;
   for (int repeat = 0; repeat < 3; ++repeat) {
      REQUIRE_FALSE(ChangeVideoClipMute(front, front.clips[0].id, true));
      REQUIRE(SameVideoProject(project, muted));
      REQUIRE(front.End() == original.tracks[0].timeline.End());
      REQUIRE(front.clips[0].sourceStart == original.tracks[0].timeline.clips[0].sourceStart);
      REQUIRE(VideoContentSources(front.clips[0], SourceFor(front, front.clips[0])).size() == 1);
      REQUIRE_FALSE(KeepsOriginalPicture(front));
      REQUIRE(EvaluateVideoLayers({&front, &back}, 1).timeline == &back);
      REQUIRE(EvaluateVideoLayers({&front, &back}, 6).timeline == &front);
      REQUIRE(EvaluateVideoTimeline(project, Settings(), 1).layers.size() == 1);
      const auto flattened = ComposeVideoTracks({&front, &back});
      REQUIRE(EvaluateVideoLayers({&flattened}, 1).source.path == "back.mp4");
      REQUIRE(EvaluateVideoLayers({&flattened}, 6).source.path == "front.mp4");
      REQUIRE_THROWS(ChangeVideoClipMute(front, 99999, false));
      REQUIRE(SameVideoProject(project, muted));
   }
   REQUIRE(CopyVideoRange(front, 0, 5).clips[0].muted);
   REQUIRE_THROWS(HealVideoClips(front, 0, 10));
   REQUIRE(SameVideoProject(project, muted));
   REQUIRE(JoinVideoClips(front, 0, 10));
   REQUIRE_FALSE(front.clips[0].muted);
   REQUIRE(front.clips[0].content->parts[0].muted);
   REQUIRE_FALSE(front.clips[0].content->parts[1].muted);
   REQUIRE(EvaluateVideoLayers({&front, &back}, 1).timeline == &back);
   REQUIRE(EvaluateVideoLayers({&front, &back}, 6).timeline == &front);
   REQUIRE(SeparateVideoClips(front, 0, 10));
   REQUIRE(front.clips[0].muted);
   REQUIRE_FALSE(front.clips[1].muted);
   REQUIRE(JoinVideoClips(front, 0, 10));
   REQUIRE(ChangeVideoClipMute(front, front.clips[0].id, true));
   REQUIRE(SeparateVideoClips(front, 0, 10));
   REQUIRE(std::all_of(front.clips.begin(), front.clips.end(), [](const auto &clip) { return clip.muted; }));
   REQUIRE(ComposeVideoTracks({&front}).clips.empty());
   REQUIRE(front.End() == 10);
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


TEST_CASE("Embedded render plans resolve nested rectangles and suppress children in parent gaps", "[video-composition][video-embedding]")
{
   auto parent = VideoTimeline::CreateVideoTimeline("parent.mp4", 6, 30, 640, 360);
   auto child = VideoTimeline::CreateVideoTimeline("child.mp4", 6, 24, 160, 100);
   auto nested = VideoTimeline::CreateVideoTimeline("nested.mp4", 6, 25, 320, 180);
   parent.placement.id = "parent";
   child.placement = {"child", "parent", {.5, .1, .4, .5}};
   nested.placement = {"nested", "child", {.25, .25, .5, .5}};
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto plan = PlanVideoTransitions({&child, &parent, &nested}, 6, 30);
      REQUIRE(plan.size() == 1); REQUIRE(plan[0].requiresBlend);
      REQUIRE(plan[0].layers.size() == 3);
      REQUIRE(plan[0].layers[0].source.path == "parent.mp4");
      REQUIRE(plan[0].layers[1].source.path == "child.mp4");
      const auto &rect = plan[0].layers[2].rectangle;
      REQUIRE(rect.x == Approx(.6)); REQUIRE(rect.y == Approx(.225));
      REQUIRE(rect.width == Approx(.2)); REQUIRE(rect.height == Approx(.25));
      const auto pixels = CalculateVideoPlacementRectangle(plan[0].layers[1].rectangle, 640, 360);
      REQUIRE(pixels.x == 320); REQUIRE(pixels.y == 36);
      REQUIRE(pixels.width == 256); REQUIRE(pixels.height == 180);
   }
   VideoTimeline::EraseVideoRange(parent, 2, 4, false);
   const auto plan = PlanVideoTransitions({&nested, &child, &parent}, 6, 30);
   REQUIRE(plan.size() == 3);
   REQUIRE(plan[1].layers.empty()); // Children cannot become orphan overlays.
   REQUIRE(plan[0].layers.size() == 3); REQUIRE(plan[2].layers.size() == 3);
   child.placement.parentId = "nested";
   REQUIRE_THROWS(PlanVideoTransitions({&nested, &child, &parent}, 6, 30));
}

TEST_CASE("Embedded family visibility uses stable identities and one shared solo scope", "[video-composition][video-embedding]")
{
   auto root = VideoTimeline::CreateVideoTimeline("base.mp4", 4, 30, 640, 360);
   auto child = root, other = root;
   root.placement.id = "root"; child.placement = {"child", "root", {.5, .5, .5, .5}};
   other.placement.id = "other";
   using VideoTimeline::VideoTimelineLayer;
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto family = VideoTimeline::VisibleVideoLayers({
         {&child, {false, true}}, {&other, {}}, {&root, {}}});
      REQUIRE(family == std::vector<const VideoTimeline::Timeline*>{&child, &root});
      REQUIRE(VideoTimeline::VisibleVideoLayers({{&root, {true, false}}, {&child, {false, true}}}).empty());
      REQUIRE(VideoTimeline::VisibleVideoLayers({{&child, {}}, {&other, {}}}) ==
         std::vector<const VideoTimeline::Timeline*>{&other});
   }
   root.placement.parentId = "child";
   REQUIRE_THROWS(VideoTimeline::VisibleVideoLayers({{&root, {}}, {&child, {}}}));
}

TEST_CASE("Embedding placement rejects invalid state and stale edit publication", "[video-composition][video-embedding]")
{
   for (const auto rectangle : {VideoFrameRectangle{-1,0,1,1}, {0,0,0,1}, {.8,0,.3,1},
         {0,0,std::numeric_limits<double>::quiet_NaN(),1}}) {
      REQUIRE_THROWS(RequireUsableVideoPlacement({"child", "parent", rectangle}));
      REQUIRE_THROWS(CalculateVideoPlacementRectangle(rectangle, 640, 360));
   }
   REQUIRE_THROWS(RequireUsableVideoPlacement({"same", "same", {}}));
   REQUIRE_THROWS(RequireUsableVideoPlacement({"", "parent", {}}));
   const auto tiny = CalculateVideoPlacementRectangle({.999, .999, .001, .001}, 160, 90);
   REQUIRE(tiny.x + tiny.width <= 160); REQUIRE(tiny.y + tiny.height <= 90);
   REQUIRE(tiny.width == 2); REQUIRE(tiny.height == 2);
   auto project = Project();
   const auto plan = PlanVideoEdits(project, {{VideoEditKind::SplitClip, "front", 0, 5}});
   project.tracks[0].timeline.placement.rectangle = {.5, .5, .5, .5};
   const auto before = project;
   REQUIRE_THROWS(ApplyVideoEditPlan(project, plan));
   REQUIRE(SameVideoProject(project, before));
}

TEST_CASE("Picture generators retain handles and reject invalid edits atomically", "[video-composition][video-generators]")
{
   auto timeline = VideoTimeline::CreateVideoTimeline("source.mp4", 8, 30, 640, 360);
   timeline.clips.front().sourceStart = 1; timeline.clips.front().duration = 3;
   const auto original = timeline;
   REQUIRE(VideoTimeline::ReverseVideoClip(timeline, 1));
   REQUIRE(timeline.clips.front().sourceStart == 4);
   REQUIRE(VideoTimeline::VideoContentDuration(timeline.clips.front(), {}) == 8);
   REQUIRE(VideoTimeline::HasGeneratedVideoContent(timeline.clips.front()));
   REQUIRE_FALSE(VideoTimeline::ReverseVideoClip(timeline, 1));
   REQUIRE(VideoTimeline::TrimVideoClip(timeline, 1, 1, 3));
   REQUIRE(VideoTimeline::TrimVideoClip(timeline, 1, 0, 3));
   REQUIRE(timeline.clips.front().sourceStart == 4);
   const auto sample = VideoTimeline::SampleVideoContent(timeline.clips.front(), {}, 4.5);
   REQUIRE(sample.generated); REQUIRE(sample.sourceTime == 4.5);
   REQUIRE(sample.generated->parts.front().media.path == "source.mp4");
   for (int repeat{}; repeat < 2; ++repeat) {
      auto frozen = original;
      REQUIRE(VideoTimeline::FreezeVideoClip(frozen, 1, 1.5, 2));
      REQUIRE(frozen.clips.front().content->heldTime == 2.5);
      REQUIRE(frozen.clips.front().duration == 2);
      REQUIRE_FALSE(VideoTimeline::FreezeVideoClip(frozen, 1, 1, 2));
      REQUIRE_THROWS(VideoTimeline::FreezeVideoClip(frozen, 1, 2, 3));
      REQUIRE(frozen.clips.front().duration == 2);
      auto collision = original; collision.clips.push_back({2, 4, 4, 2}); collision.nextId = 3;
      REQUIRE_THROWS(VideoTimeline::FreezeVideoClip(collision, 1, 1, 5));
      REQUIRE(collision.clips.front().duration == 3);
   }
}

TEST_CASE("Still pictures and titles use bounded saved content rather than cache paths", "[video-composition][video-generators]")
{
   for (int repeat{}; repeat < 2; ++repeat) {
      auto title = VideoTimeline::MakeTitleVideoClip(1, 2, 4, "A neutral title", 640, 360);
      auto image = VideoTimeline::MakeStillVideoClip(2, 6, 3, "picture.png", 320, 180);
      REQUIRE(VideoTimeline::VideoContentSources(title, {}).empty());
      REQUIRE(VideoTimeline::VideoContentSources(image, {}).front().path == "picture.png");
      REQUIRE(VideoTimeline::ReplaceVideoContentSource(image, {}, "picture.png", {"moved.png", 3, 30, 640, 360}));
      REQUIRE(VideoTimeline::VideoContentSources(image, {}).front().path == "moved.png");
      const auto before = title;
      auto changed = std::make_shared<VideoTimeline::VideoClipContent>(*title.content); changed->title = "Another title";
      title.content = changed; REQUIRE_FALSE(VideoTimeline::SameVideoContent(before, title));
      REQUIRE_THROWS(VideoTimeline::MakeTitleVideoClip(1, 0, 4, "", 640, 360));
      REQUIRE_THROWS(VideoTimeline::MakeStillVideoClip(1, 0, 4, "", 640, 360));
      REQUIRE_THROWS(VideoTimeline::MakeTitleVideoClip(1, 0, 4, "title", 99999, 360));
      changed->operation = static_cast<VideoTimeline::VideoClipContent::Operation>(999);
      REQUIRE_THROWS(VideoTimeline::RequireUsableVideoContent(title, {}));
   }
}

TEST_CASE("Picture generators preserve the separate soundtrack source clock", "[video-composition][video-generators]")
{
   for (int repeat{}; repeat < 2; ++repeat) {
      auto original = VideoTimeline::CreateVideoTimeline("source.mp4", 8, 30, 640, 360);
      auto &clip = original.clips.front(); clip.sourceStart = 1; clip.duration = 1; clip.playbackRate = 2;
      clip.linkGroup = std::string(32, 'a');
      REQUIRE_THROWS(VideoTimeline::VideoContentLinkWindows(clip));
      clip.media = VideoTimeline::SourceFor(original, clip);
      const auto before = VideoTimeline::VideoContentLinkWindows(clip).front();
      for (const bool freeze : {false, true}) {
         auto timeline = original;
         if (freeze) VideoTimeline::FreezeVideoClip(timeline, 1, .25, 1);
         else VideoTimeline::ReverseVideoClip(timeline, 1);
         auto linked = VideoTimeline::VideoContentLinkWindows(timeline.clips.front()).front();
         REQUIRE(linked.sourceOrigin == before.sourceOrigin); REQUIRE(linked.sourceEnd == before.sourceEnd);
         REQUIRE(linked.playbackRate == before.playbackRate); REQUIRE(linked.group == before.group);
         REQUIRE(VideoTimeline::MoveClipTo(timeline, 1, 2));
         linked = VideoTimeline::VideoContentLinkWindows(timeline.clips.front()).front();
         REQUIRE(linked.sourceOrigin == before.sourceOrigin + 2);
      }
   }
}

TEST_CASE("Generated picture windows never expand a short edit to its hidden movie handles", "[video-composition][video-generators]")
{
   auto timeline = VideoTimeline::CreateVideoTimeline("long.mp4", 7200, 30, 640, 360);
   timeline.clips.front().sourceStart = 600; timeline.clips.front().duration = 2;
   VideoTimeline::ReverseVideoClip(timeline, 1);
   const auto &clip = timeline.clips.front();
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto window = PlanVideoGeneratorWindow(*clip.content, clip.sourceStart, clip.sourceStart + clip.duration);
      REQUIRE(window.duration == 2); REQUIRE(window.frameCount == 60);
      REQUIRE(window.inputStart == 600); REQUIRE(window.inputEnd == 602);
      const auto frame = PlanVideoGeneratorWindow(*clip.content, clip.sourceStart + .5, clip.sourceStart + .5 + 1.0 / 30);
      REQUIRE(frame.frameCount == 1);
      REQUIRE_THROWS(PlanVideoGeneratorWindow(*clip.content, -1, 1));
      REQUIRE_THROWS(PlanVideoGeneratorWindow(*clip.content, 0, 8000));
   }
   auto cropped = VideoTimeline::CreateVideoTimeline("source.mp4", 8, 30, 640, 360);
   cropped.clips.front().crop = {.1,.2,.1,0}; cropped.clips.front().muted = true;
   VideoTimeline::ReverseVideoClip(cropped, 1);
   REQUIRE(cropped.clips.front().crop == VideoFrameCrop{.1,.2,.1,0});
   REQUIRE(cropped.clips.front().muted);
   REQUIRE_FALSE(cropped.clips.front().content->parts.front().muted);
   REQUIRE(cropped.clips.front().content->parts.front().crop == VideoFrameCrop{});
   REQUIRE(cropped.clips.front().content->width == 640); REQUIRE(cropped.clips.front().content->height == 360);
}

TEST_CASE("Repeat picture fills only missing matching slots and preserves collision boundaries", "[video-composition][video-generators]")
{
   for (int repeat{}; repeat < 2; ++repeat) {
      auto timeline = VideoTimeline::CreateVideoTimeline("source.mp4", 2, 30, 640, 360);
      timeline.clips.front().linkGroup = std::string(32, 'a');
      REQUIRE(VideoTimeline::RepeatVideoClip(timeline, 1, 3));
      REQUIRE(timeline.clips.size() == 4); REQUIRE(timeline.End() == 8);
      REQUIRE(timeline.clips[1].linkGroup.empty());
      const auto next = timeline.nextId;
      REQUIRE_FALSE(VideoTimeline::RepeatVideoClip(timeline, 1, 3)); REQUIRE(timeline.nextId == next);
      REQUIRE_THROWS(VideoTimeline::RepeatVideoClip(timeline, 1, 101)); REQUIRE(timeline.End() == 8);
      auto occupied = VideoTimeline::CreateVideoTimeline("source.mp4", 8, 30, 640, 360);
      occupied.clips.front().duration = 2; occupied.clips.push_back({2, 3, 0, 1}); occupied.nextId = 3;
      REQUIRE_THROWS(VideoTimeline::RepeatVideoClip(occupied, 1, 3));
      REQUIRE(occupied.clips.size() == 2); REQUIRE(occupied.nextId == 3);
   }
}

TEST_CASE("AuTopsy release identities order numeric previews and stable versions", "[product-updates]")
{
   using namespace AuTopsy::Updates;
   REQUIRE(*ParseReleaseVersion("v3.7.10-autopsy.9") < *ParseReleaseVersion("3.7.10-autopsy.10"));
   REQUIRE(*ParseReleaseVersion("v3.7.10-autopsy.99") < *ParseReleaseVersion("v3.7.10"));
   REQUIRE(*ParseReleaseVersion("v3.7.10") < *ParseReleaseVersion("v3.8.0-autopsy.1"));
   for (const auto *bad : {"", "v3.7", "v3.07.10", "v3.7.10-autopsy.0", "v3.7.10-autopsy.01",
         "v3.7.10-autopsy.1/extra", "v9999999999999999999.7.10", "v3.7.10-other.1"})
      REQUIRE_FALSE(ParseReleaseVersion(bad));
}

TEST_CASE("AuTopsy update selection excludes drafts foreign installers and older local builds", "[product-updates]")
{
   using namespace AuTopsy::Updates;
   const auto offer = [](const std::string &tag) {
      return ReleaseOffer{tag, "Release notes", "https://github.com/Ceratops-Code/AuTopsy/releases/tag/" + tag,
         "https://github.com/Ceratops-Code/AuTopsy/releases/download/" + tag + "/autopsy-win-3.7.10-test-x64.exe", false, true};
   };
   const auto current = *ParseReleaseVersion("v3.7.10-autopsy.13");
   for (int repeat{}; repeat < 2; ++repeat) {
      auto wanted = offer("v3.7.10-autopsy.14"), draft = offer("v4.0.0-autopsy.1"); draft.draft = true;
      auto foreign = offer("v3.9.0-autopsy.1"); foreign.download = "https://example.org/installer.exe";
      auto arm = offer("v3.9.1-autopsy.1"); arm.download.replace(arm.download.size()-7, 7, "arm.exe");
      const std::vector<ReleaseOffer> choices{offer("v3.7.10-autopsy.12"), foreign, draft, arm, wanted};
      REQUIRE(ChooseProductUpdate(choices, current, true)->tag == wanted.tag);
      REQUIRE_FALSE(ChooseProductUpdate(choices, current, false));
      REQUIRE_FALSE(ChooseProductUpdate(choices, *ParseReleaseVersion("v3.7.10-autopsy.15"), true));
      wanted.page += "/../../outside"; REQUIRE_FALSE(ChooseProductUpdate({wanted}, current, true));
      wanted = offer("v3.7.10-autopsy.14"); wanted.download += "?redirect=elsewhere";
      REQUIRE_FALSE(ChooseProductUpdate({wanted}, current, true));
      REQUIRE_FALSE(ShouldCheckProductUpdates(false, 999999, 0, 86400, false));
      REQUIRE_FALSE(ShouldCheckProductUpdates(true, 999999, 0, 86400, true));
      REQUIRE_FALSE(ShouldCheckProductUpdates(true, 100, 90, 86400, false));
      REQUIRE(ShouldCheckProductUpdates(true, 86400, 0, 86400, false));
      REQUIRE(ShouldCheckProductUpdates(true, 86500, 100, 86400, false));
      REQUIRE(ShouldCheckProductUpdates(true, 90, 100, 86400, false));
   }
}

TEST_CASE("Timeline thumbnails resolve reverse freeze and nested picture clocks without rendering", "[video-composition][video-generators]")
{
   auto timeline = VideoTimeline::CreateVideoTimeline("movie.mp4", 5, 30, 320, 180);
   REQUIRE(VideoTimeline::ReverseVideoClip(timeline, 1));
   const auto &clip = timeline.clips.front();
   for (const double at : {0.0, .25, 2.75, 4.99}) {
      const auto sample = VideoTimeline::SampleVideoThumbnail(clip, {}, at);
      REQUIRE(sample);
      REQUIRE_FALSE(sample.generated);
      REQUIRE(sample.source.path == "movie.mp4");
      REQUIRE(sample.source.duration == 5);
      REQUIRE(sample.sourceTime == Approx((149 - std::floor(at * 30 + 1e-8)) / 30));
   }
   auto frozen = VideoTimeline::CreateVideoTimeline("movie.mp4", 5, 30, 320, 180);
   REQUIRE(VideoTimeline::FreezeVideoClip(frozen, 1, 1.25, 2));
   for (const double at : {0.0, .25, 1.99}) {
      const auto sample = VideoTimeline::SampleVideoThumbnail(frozen.clips.front(), {}, at);
      REQUIRE(sample);
      REQUIRE(sample.source.duration == 5);
      REQUIRE(sample.sourceTime == Approx(37.0 / 30));
   }
   REQUIRE(VideoTimeline::ReverseVideoClip(frozen, 1));
   REQUIRE(VideoTimeline::SampleVideoThumbnail(frozen.clips.front(), {}, .25).sourceTime == Approx(37.0 / 30));
}

TEST_CASE("Text overlays retain their appearance and alpha through native clip models", "[video-composition][video-text]")
{
   for (int repeat{}; repeat < 2; ++repeat) {
      VideoTextAppearance style{"", .35, 0xffcc44, true, false, true};
      VideoTimeline::Timeline text;
      text.clips.push_back(VideoTimeline::MakeTextVideoClip(1, 1, 4, "A caption\nSecond line", 640, 144, style));
      text.nextId = 2;
      const auto original = text;
      REQUIRE(VideoTimeline::VideoContentUsesAlpha(text.clips.front()));
      REQUIRE(VideoTimeline::IsStaticVideoContent(*text.clips.front().content));
      REQUIRE(VideoTimeline::SourceFor(text, text.clips.front()).width == 640);
      REQUIRE(VideoTimeline::SplitClipAt(text, 3));
      REQUIRE_FALSE(VideoTimeline::SplitClipAt(text, 3));
      REQUIRE(text.clips.back().content == original.clips.front().content);
      REQUIRE(VideoTimeline::JoinVideoClips(text, 1, 5));
      REQUIRE(VideoTimeline::VideoContentUsesAlpha(text.clips.front()));
      REQUIRE(VideoTimeline::ReverseVideoClip(text, 1));
      REQUIRE(VideoTimeline::VideoContentUsesAlpha(text.clips.front()));
      const auto sample = VideoTimeline::SampleVideoThumbnail(text.clips.front(), {}, 1);
      REQUIRE(sample.generated);
      REQUIRE(sample.generated->operation == VideoTimeline::VideoClipContent::Operation::Text);
      REQUIRE(sample.generated->textAppearance == style);
      auto frozen = original;
      REQUIRE(VideoTimeline::FreezeVideoClip(frozen, 1, 2, 3));
      REQUIRE(VideoTimeline::VideoContentUsesAlpha(frozen.clips.front()));
      REQUIRE(VideoTimeline::FindEditableVideoTextContent(frozen.clips.front()));
      auto changed = style; changed.rgb = 0x4488ff; changed.fontFace = "Example font family";
      REQUIRE(VideoTimeline::ChangeVideoTextContent(frozen.clips.front(), "Changed", changed));
      REQUIRE_FALSE(VideoTimeline::ChangeVideoTextContent(frozen.clips.front(), "Changed", changed));
      REQUIRE(VideoTimeline::FindEditableVideoTextContent(frozen.clips.front())->textAppearance == changed);
      REQUIRE(original.clips.front().content->title == "A caption\nSecond line");
      REQUIRE(original.clips.front().content->textAppearance == style);
   }
}

TEST_CASE("A full frame transparent text layer keeps the parent picture in the composition", "[video-composition][video-text]")
{
   auto background = VideoTimeline::CreateVideoTimeline("neutral.mp4", 5, 30, 640, 360);
   background.placement.id = "background";
   VideoTimeline::Timeline text;
   text.placement = {"caption", "background", {}};
   text.clips.push_back(VideoTimeline::MakeTextVideoClip(1, 1, 2, "Caption", 640, 360, {})); text.nextId = 2;
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto spans = PlanVideoTransitions({&background, &text}, 5, 30);
      REQUIRE(spans.size() == 3);
      REQUIRE(spans[0].layers.size() == 1);
      REQUIRE(spans[1].firstFrame == 30);
      REQUIRE(spans[1].frameCount == 60);
      REQUIRE(spans[1].layers.size() == 2);
      REQUIRE(spans[1].layers.front().source.path == "neutral.mp4");
      REQUIRE(spans[1].layers.back().transparent);
      REQUIRE(spans[2].layers.size() == 1);
   }
}

TEST_CASE("Invalid text changes cannot alter shared content or caption timing", "[video-composition][video-text]")
{
   auto clip = VideoTimeline::MakeTextVideoClip(1, 2, 3, "Original", 320, 90, {});
   const auto original = clip;
   for (int repeat{}; repeat < 2; ++repeat) {
      for (const auto *bad : {"", " \t\n"}) REQUIRE_THROWS(VideoTimeline::ChangeVideoTextContent(clip, bad, {}));
      REQUIRE_THROWS(VideoTimeline::ChangeVideoTextContent(clip, std::string("A\0B", 3), {}));
      for (const auto size : {0.0, -.1, 1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
         VideoTextAppearance style; style.heightFraction = size;
         REQUIRE_THROWS(VideoTimeline::ChangeVideoTextContent(clip, "Changed", style));
      }
      VideoTextAppearance style; style.fontFace = "Bad\nfont";
      REQUIRE_THROWS(VideoTimeline::ChangeVideoTextContent(clip, "Changed", style));
      style = {}; style.rgb = 0x1000000;
      REQUIRE_THROWS(VideoTimeline::ChangeVideoTextContent(clip, "Changed", style));
      REQUIRE(clip.content == original.content);
      REQUIRE(clip.start == 2); REQUIRE(clip.duration == 3);
   }
}
