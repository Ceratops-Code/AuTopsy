// Dedicated composition geometry/eligibility cases, independent of pixel codecs.
#include "EvaluateVideoTimeline.h"
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
   settings.layers.push_back({"front", {}, false, false, {{1, {0.1, 0.2, 0.3, 0.1}}}});
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
   settings.layers[0].clips[0].crop.right = 0.9;
   REQUIRE_THROWS(EvaluateVideoTimeline(project, settings, 4));
   settings.layers[0].clips[0].crop = {0, 0, 0, std::numeric_limits<double>::quiet_NaN()};
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
   settings.layers.push_back({"back", {}, false, false, {{1, {}, true}}});
   REQUIRE(EvaluateVideoTimeline(project, settings, 1).layers.empty());
   settings.layers[1].muted = false;
   settings.layers[0].clips = {{1, {}, true}};
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
   settings.layers = {{"front", {}, false, false, {{1, {0.1, 0.1, 0.1, 0.1}}}}};
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
