/**********************************************************************

  Audacity: A Digital Audio Editor

  TestVideoPlayback.cpp

  Behavioral checks for audio-clock following, including interruption just
  before and after each externally visible player effect.

**********************************************************************/

#include "SyncVideoPlayback.h"
#include "ChooseVideoExport.h"
#include "RelinkVideoMedia.h"
#include "EditVideoTimeline.h"

#include <catch2/catch.hpp>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace VideoPreview;

TEST_CASE("Frame timecode keeps fractional source rates and rejects malformed input")
{
   for (const double rate : {24.0, 25.0, 30.0, 60.0, 24000.0 / 1001, 30000.0 / 1001}) {
      for (const auto frame : {0, 1, 29, 301, 123456}) {
         const auto time = frame / rate;
         REQUIRE(ParseFrameTimecode(FormatFrameTimecode(time, rate), rate) == Approx(time));
      }
   }
   for (const auto *bad : {"", "00:60:00:00", "00:00:00:30", "-1:00:00:00", "00:00:01:00 junk"})
      REQUIRE_THROWS(ParseFrameTimecode(bad, 30));
   REQUIRE_THROWS(FormatFrameTimecode(-1, 30));
   REQUIRE_THROWS(ParseFrameTimecode("00:00:00:00", 0));
}

TEST_CASE("Frame steps use source boundaries after moves and trims")
{
   const double start = 7.0, source = 1.0 / 60;
   REQUIRE(AdjacentSourceFrame(start, start, source, 60, 1) == Approx(start + 1.0 / 60));
   REQUIRE(AdjacentSourceFrame(start + 1.0 / 60, start, source, 60, -1) == Approx(start));
   REQUIRE(AdjacentSourceFrame(start + 0.001, start, source, 60, -1) == Approx(start));
   REQUIRE(AdjacentSourceFrame(0, 0, 0, 30, -1) == 0);
   REQUIRE_THROWS(AdjacentSourceFrame(0, 0, 0, 30, 0));
}

namespace {

struct Interrupted : std::runtime_error {
   Interrupted() : std::runtime_error{ "simulated interruption" } {}
};

class FakePlayer {
public:
   PlayerSnapshot state{ true, true, false, 0.0, 10.0, 30.0, 1.0 };
   std::vector<std::string> effects;
   std::optional<size_t> interruptBoundary;
   size_t boundary{};
   bool asynchronousSeek{};
   double seekTarget{};

   PlayerSnapshot ReadState() const { return state; }

   void SetPaused(bool value)
   {
      AtBoundary();
      state.paused = value;
      effects.push_back(value ? "pause" : "play");
      AtBoundary();
   }

   void Seek(double value)
   {
      REQUIRE(state.paused);
      REQUIRE_FALSE(state.seeking);
      AtBoundary();
      seekTarget = value;
      if (asynchronousSeek)
         state.seeking = true;
      else
         state.position = value;
      effects.push_back("seek");
      AtBoundary();
   }

   void SetSpeed(double value)
   {
      REQUIRE(state.paused);
      AtBoundary();
      state.speed = value;
      effects.push_back("speed");
      AtBoundary();
   }

private:
   void AtBoundary()
   {
      if (interruptBoundary && boundary++ == *interruptBoundary) {
         interruptBoundary.reset();
         throw Interrupted{};
      }
   }
};

TEST_CASE("Precise inspection seeks a single source frame that smooth playback can tolerate")
{
   FakePlayer player;
   player.state.frameRate = 60;
   player.state.position = 1;
   const TransportSnapshot cursor{1 + 1.0 / 60, 1, false};
   SynchronizeVideoPlayback(player, cursor, true);
   REQUIRE(player.state.position == Approx(cursor.position));
   REQUIRE(player.effects == std::vector<std::string>{"seek"});
   SynchronizeVideoPlayback(player, cursor, true);
   REQUIRE(player.effects.size() == 1);
}

void RunFreshCalls(FakePlayer &player, const TransportSnapshot &audio)
{
   for (int i = 0; i != 12; ++i) {
      try {
         SynchronizeVideoPlayback(player, audio);
      }
      catch (const Interrupted &) {
         // A new normal call uses observed state, without replaying commands.
      }
   }
}

void CheckInterruptionBoundaries(const PlayerSnapshot &initial,
   const TransportSnapshot &audio, double expectedPosition, bool expectedPaused)
{
   FakePlayer accepted;
   accepted.state = initial;
   RunFreshCalls(accepted, audio);
   REQUIRE(accepted.state.position == Approx(expectedPosition));
   REQUIRE(accepted.state.paused == expectedPaused);
   const auto acceptedEffects = accepted.effects;
   REQUIRE_FALSE(acceptedEffects.empty());

   for (size_t boundary = 0; boundary != acceptedEffects.size() * 2; ++boundary) {
      CAPTURE(boundary);
      FakePlayer interrupted;
      interrupted.state = initial;
      interrupted.interruptBoundary = boundary;
      RunFreshCalls(interrupted, audio);
      REQUIRE_FALSE(interrupted.interruptBoundary);
      REQUIRE(interrupted.state.position == Approx(expectedPosition));
      REQUIRE(interrupted.state.paused == expectedPaused);
      REQUIRE(interrupted.state.speed == Approx(accepted.state.speed));
      REQUIRE(interrupted.effects == acceptedEffects);

      const auto effects = interrupted.effects;
      RunFreshCalls(interrupted, audio);
      REQUIRE(interrupted.effects == effects);
   }
}

} // namespace

TEST_CASE("Video follows only its own project's transport", "[video-preview]")
{
   auto otherProject = TransportSnapshot::ForProjectPlayback(false, false, 8.0, 2.0);
   REQUIRE_FALSE(otherProject.playing);
   REQUIRE(otherProject.position == 2.0);
   auto owner = TransportSnapshot::ForProjectPlayback(true, false, 8.0, 2.0);
   REQUIRE(owner.playing);
   REQUIRE(owner.position == 8.0);
   auto paused = TransportSnapshot::ForProjectPlayback(true, true, 8.0, 2.0);
   REQUIRE_FALSE(paused.playing);
   REQUIRE(paused.position == 8.0);
}

TEST_CASE("Video play pause stop seek loop and speed survive interruption",
   "[video-preview][interruption]")
{
   PlayerSnapshot initial{ true, true, false, 0.0, 10.0, 30.0, 1.0 };
   SECTION("play") {
      CheckInterruptionBoundaries(initial, { 0.0, 1.0, true }, 0.0, false);
   }
   SECTION("pause") {
      initial.paused = false;
      initial.position = 2.0;
      CheckInterruptionBoundaries(initial, { 2.0, 1.0, false }, 2.0, true);
   }
   SECTION("stop returns to selection") {
      initial.paused = false;
      initial.position = 3.0;
      CheckInterruptionBoundaries(initial, { 0.0, 1.0, false }, 0.0, true);
   }
   SECTION("seek while playing") {
      initial.paused = false;
      initial.position = 2.0;
      CheckInterruptionBoundaries(initial, { 7.0, 1.0, true }, 7.0, false);
   }
   SECTION("loop wraps backwards") {
      initial.paused = false;
      initial.position = 9.0;
      CheckInterruptionBoundaries(initial, { 2.0, 1.0, true }, 2.0, false);
   }
   SECTION("speed change") {
      initial.paused = false;
      CheckInterruptionBoundaries(initial, { 0.0, 2.0, true }, 0.0, false);
   }
   SECTION("beyond video end holds last frame") {
      initial.paused = false;
      CheckInterruptionBoundaries(initial, { 20.0, 1.0, true }, 9.999, true);
   }
   SECTION("negative audio time holds first frame") {
      initial.paused = false;
      initial.position = 3.0;
      CheckInterruptionBoundaries(initial, { -2.0, 1.0, true }, 0.0, true);
   }
}

TEST_CASE("An unfinished seek cannot resume video or submit another seek",
   "[video-preview]")
{
   FakePlayer player;
   player.asynchronousSeek = true;
   player.state.paused = false;
   SynchronizeVideoPlayback(player, { 4.0, 1.0, true });
   REQUIRE((player.effects == std::vector<std::string>{ "pause", "seek" }));
   REQUIRE(player.state.seeking);
   const auto effects = player.effects;
   RunFreshCalls(player, { 4.0, 1.0, true });
   REQUIRE(player.effects == effects);

   player.state.position = player.seekTarget;
   player.state.seeking = false; // The player reports completion.
   RunFreshCalls(player, { 4.0, 1.0, true });
   REQUIRE_FALSE(player.state.paused);
   REQUIRE(player.effects.back() == "play");
}

TEST_CASE("Frame quantization and small clock drift do not cause seek loops",
   "[video-preview]")
{
   FakePlayer player;
   player.state.frameRate = 1.0;
   player.state.position = 2.0;
   RunFreshCalls(player, { 2.9, 1.0, false });
   REQUIRE(player.effects.empty());
   player.state.frameRate = 30.0;
   player.state.paused = false;
   RunFreshCalls(player, { 2.04, 1.0, true });
   REQUIRE(player.effects.empty());
   RunFreshCalls(player, { 2.5, 1.0, true });
   REQUIRE(player.state.position == 2.5);
   REQUIRE_FALSE(player.state.paused);
}

TEST_CASE("Video waits for loading and rejects invalid clock values safely",
   "[video-preview]")
{
   FakePlayer player;
   player.state.ready = false;
   RunFreshCalls(player, { 3.0, 1.0, true });
   REQUIRE(player.effects.empty());
   player.state.ready = true;
   player.state.paused = false;
   RunFreshCalls(player,
      { std::numeric_limits<double>::quiet_NaN(), 1.0, true });
   REQUIRE(player.state.paused);
   REQUIRE((player.effects == std::vector<std::string>{ "pause" }));
}

TEST_CASE("Video splits produce persistent independent identities without changing picture timing",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("movie.mp4", 4.0, 30.0);
   REQUIRE(KeepsOriginalPicture(timeline));
   REQUIRE(SplitClipAt(timeline, 2.0));
   REQUIRE(timeline.clips.size() == 2);
   REQUIRE(timeline.clips[0].id == 1);
   REQUIRE(timeline.clips[1].id == 2);
   REQUIRE(timeline.clips[0].duration == 2.0);
   REQUIRE(timeline.clips[1].sourceStart == 2.0);
   REQUIRE(KeepsOriginalPicture(timeline));
   REQUIRE_FALSE(SplitClipAt(timeline, 2.0));
   REQUIRE(timeline.nextId == 3);
   REQUIRE_FALSE(SplitClipAt(timeline, 0.0));
   REQUIRE_FALSE(SplitClipAt(timeline, 4.0));
   REQUIRE(timeline.clips.size() == 2);
}

TEST_CASE("Owned video timing maps moved clips and black gaps independently of a host track",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("movie.mp4", 4.0, 30.0);
   SplitClipAt(timeline, 2.0);
   REQUIRE(MoveClipTo(timeline, 2, 3.0));
   REQUIRE_FALSE(MoveClipTo(timeline, 2, 3.0));
   REQUIRE_FALSE(KeepsOriginalPicture(timeline));
   REQUIRE(timeline.End() == 5.0);
   REQUIRE_FALSE(PictureAt(timeline, -1.0).visible);
   REQUIRE_FALSE(PictureAt(timeline, 2.5).visible);
   REQUIRE_FALSE(PictureAt(timeline, 5.0).visible);
   const auto picture = PictureAt(timeline, 3.25);
   REQUIRE(picture.visible);
   REQUIRE(picture.clipId == 2);
   REQUIRE(picture.sourceTime == 2.25);
   REQUIRE(RemoveClip(timeline, 1));
   REQUIRE_FALSE(RemoveClip(timeline, 1));
   REQUIRE_FALSE(PictureAt(timeline, 0.5).visible);
   REQUIRE(PictureAt(timeline, 3.25).sourceTime == 2.25);
}

TEST_CASE("Rejected video edits leave the complete prior timeline intact",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("movie.mp4", 4.0, 30.0);
   SplitClipAt(timeline, 2.0);
   const auto checkOriginal = [&] {
      REQUIRE(timeline.sourcePath == "movie.mp4");
      REQUIRE(timeline.nextId == 3);
      REQUIRE(timeline.clips.size() == 2);
      REQUIRE(timeline.clips[0].start == 0.0);
      REQUIRE(timeline.clips[1].start == 2.0);
      REQUIRE(timeline.clips[1].sourceStart == 2.0);
      REQUIRE(timeline.clips[1].duration == 2.0);
   };
   checkOriginal(); // interruption before the single model publication
   REQUIRE_THROWS(MoveClipTo(timeline, 2, 1.0)); checkOriginal();
   REQUIRE_THROWS(MoveClipTo(timeline, 1, -1.0)); checkOriginal();
   REQUIRE_THROWS(MoveClipTo(timeline, 999, 10.0)); checkOriginal();
   REQUIRE_THROWS(SplitClipAt(timeline, std::numeric_limits<double>::quiet_NaN())); checkOriginal();
   REQUIRE_FALSE(SplitClipAt(timeline, 2.0)); checkOriginal(); // fresh call after publication
}

TEST_CASE("Saved video model contracts reject malformed source ranges and duplicate identities",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   REQUIRE_THROWS(CreateVideoTimeline("movie.mp4", -1, 30));
   REQUIRE_THROWS(CreateVideoTimeline("movie.mp4", 4, 0));
   auto timeline = CreateVideoTimeline("movie.mp4", 4, 30);
   timeline.clips[0].sourceStart = 1;
   REQUIRE_THROWS(RequireWellFormedTimeline(timeline));
   timeline = CreateVideoTimeline("movie.mp4", 4, 30);
   SplitClipAt(timeline, 2);
   timeline.clips[1].id = 1;
   REQUIRE_THROWS(RequireWellFormedTimeline(timeline));
   timeline.clips[1].id = 2;
   timeline.clips[1].start = 1.5;
   REQUIRE_THROWS(RequireWellFormedTimeline(timeline));
}

TEST_CASE("Video split boundaries follow source frames and empty timelines remain explicit",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("movie.mp4", 4, 30);
   REQUIRE(SplitClipAt(timeline, 1.01));
   REQUIRE(timeline.clips[0].duration == 1.0);
   REQUIRE(timeline.clips[1].sourceStart == 1.0);
   REQUIRE(RemoveClip(timeline, 1));
   REQUIRE(RemoveClip(timeline, 2));
   REQUIRE(timeline.End() == 0);
   REQUIRE_FALSE(KeepsOriginalPicture(timeline));
   REQUIRE_FALSE(SplitClipAt(timeline, 1.0));
   REQUIRE_FALSE(PictureAt(timeline, 1.0).visible);
   REQUIRE_NOTHROW(RequireWellFormedTimeline(timeline));
}

TEST_CASE("Video clipboard ranges carry sources across tracks and combine different movies",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   const auto red = CreateVideoTimeline("red.mp4", 4, 30, 1920, 1080);
   const auto blue = CreateVideoTimeline("blue.mp4", 2, 25, 720, 1280);
   const auto copied = CopyVideoRange(red, 1, 3);
   REQUIRE(copied.clips.size() == 1);
   REQUIRE(SourceFor(copied, copied.clips[0]).path == "red.mp4");
   REQUIRE(copied.clips[0].sourceStart == 1);
   Timeline destination;
   PasteVideoRange(destination, 2, copied, 2);
   REQUIRE(destination.clips[0].start == 2);
   PasteVideoRange(destination, 0, blue, 2);
   REQUIRE(destination.clips.size() == 2);
   REQUIRE(SourceFor(destination, destination.clips[0]).path == "blue.mp4");
   REQUIRE(SourceFor(destination, destination.clips[1]).path == "red.mp4");
   REQUIRE(destination.clips[1].start == 4);
   REQUIRE(destination.clips[1].sourceStart == 1);
   REQUIRE(destination.End() == 6);
   REQUIRE_FALSE(KeepsOriginalPicture(destination));
   REQUIRE(SplitClipAt(destination, 1.03));
   REQUIRE(destination.clips[0].duration == Approx(1.04));
   REQUIRE(SourceFor(destination, destination.clips[1]).path == "blue.mp4");
}

TEST_CASE("Video range deletion and insertion preserve trims and honor gap behavior",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   auto original = CreateVideoTimeline("movie.mp4", 4, 30);
   auto gap = original;
   EraseVideoRange(gap, 1, 2, false);
   REQUIRE(gap.clips.size() == 2);
   REQUIRE(gap.clips[0].duration == 1);
   REQUIRE(gap.clips[1].start == 2);
   REQUIRE(gap.clips[1].sourceStart == 2);
   REQUIRE(gap.clips[1].id != gap.clips[0].id);
   REQUIRE_FALSE(PictureAt(gap, 1.5).visible);
   auto ripple = original;
   EraseVideoRange(ripple, 1, 2, true);
   REQUIRE(ripple.End() == 3);
   REQUIRE(PictureAt(ripple, 1.5).sourceTime == 2.5);
   InsertVideoGap(original, 1, 2);
   REQUIRE(original.End() == 6);
   REQUIRE(original.clips[1].start == 3);
   REQUIRE(original.clips[1].sourceStart == 1);
   REQUIRE_FALSE(PictureAt(original, 2).visible);
}

TEST_CASE("Video edge trimming is frame snapped, reversible and atomically rejects collisions",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   auto timeline = CreateVideoTimeline("movie.mp4", 4, 30);
   REQUIRE(TrimVideoClip(timeline, 1, 1.01, 3.01));
   REQUIRE(timeline.clips[0].start == Approx(1));
   REQUIRE(timeline.clips[0].sourceStart == Approx(1));
   REQUIRE(timeline.clips[0].duration == Approx(2));
   REQUIRE_FALSE(TrimVideoClip(timeline, 1, 1, 3));
   REQUIRE_THROWS(TrimVideoClip(timeline, 1, -1, 3));
   REQUIRE(timeline.clips[0].start == Approx(1));
   REQUIRE(TrimVideoClip(timeline, 1, 0, 4));
   REQUIRE(KeepsOriginalPicture(timeline));
   SplitClipAt(timeline, 2);
   REQUIRE_THROWS(TrimVideoClip(timeline, 1, 0, 3));
   REQUIRE(timeline.clips[0].duration == 2);
   REQUIRE(timeline.clips[1].start == 2);

   // Pointer gestures cover both directions, overshoot and fresh repeated
   // releases without destroying the hidden source frames.
   auto dragged = CreateVideoTimeline("movie.mp4", 4, 30);
   REQUIRE(DragVideoClipEdge(dragged, 1, true, 1));
   REQUIRE_FALSE(DragVideoClipEdge(dragged, 1, true, 1));
   REQUIRE(DragVideoClipEdge(dragged, 1, true, -10));
   REQUIRE(dragged.clips[0].start == Approx(0));
   REQUIRE(dragged.clips[0].sourceStart == Approx(0));
   REQUIRE(DragVideoClipEdge(dragged, 1, false, 3));
   REQUIRE(DragVideoClipEdge(dragged, 1, false, 20));
   REQUIRE(KeepsOriginalPicture(dragged));
   SplitClipAt(dragged, 2);
   REQUIRE(DragVideoClipEdge(dragged, 2, true, 3));
   REQUIRE(DragVideoClipEdge(dragged, 2, true, -10));
   REQUIRE(dragged.clips[1].start == Approx(2));
   REQUIRE(dragged.clips[1].sourceStart == Approx(2));
   REQUIRE(DragVideoClipEdge(dragged, 1, false, -10));
   REQUIRE(dragged.clips[0].duration == Approx(1.0 / 30));
   REQUIRE(DragVideoClipEdge(dragged, 1, false, 20));
   REQUIRE(KeepsOriginalPicture(dragged));
   const auto before = dragged;
   REQUIRE_THROWS(DragVideoClipEdge(dragged, 1, true, std::numeric_limits<double>::quiet_NaN()));
   REQUIRE_THROWS(DragVideoClipEdge(dragged, 999, true, 1));
   REQUIRE(dragged.clips[0].duration == before.clips[0].duration);
   REQUIRE(dragged.clips[1].sourceStart == before.clips[1].sourceStart);
}

TEST_CASE("Native video track order composes upper pictures while exposing lower gaps",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   auto lower = CreateVideoTimeline("lower.mp4", 4, 30);
   auto upper = CreateVideoTimeline("upper.mp4", 2, 25);
   MoveClipTo(upper, 1, 1);
   const auto mix = ComposeVideoTracks({&upper, &lower});
   REQUIRE(mix.End() == 4);
   REQUIRE(mix.clips.size() == 3);
   REQUIRE(SourceFor(mix, *ClipAt(mix, .5)).path == "lower.mp4");
   REQUIRE(SourceFor(mix, *ClipAt(mix, 1.5)).path == "upper.mp4");
   REQUIRE(SourceFor(mix, *ClipAt(mix, 3.5)).path == "lower.mp4");
   REQUIRE(PictureAt(mix, 3.5).sourceTime == 3.5);
   REQUIRE_FALSE(KeepsOriginalPicture(mix));
   REQUIRE(ComposeVideoTracks({}).clips.empty());
}

TEST_CASE("Preview evaluation and export choose the same source frames across layered edits",
   "[video-timeline]")
{
   using namespace VideoTimeline;
   auto lower = CreateVideoTimeline("lower.mp4", 6, 30, 1920, 1080);
   SplitClipAt(lower, 3);
   MoveClipTo(lower, 2, 4);
   auto upper = CreateVideoTimeline("upper.mp4", 2, 25, 720, 1280);
   TrimVideoClip(upper, 1, .4, 1.6);
   MoveClipTo(upper, 1, 2);
   const std::vector<const Timeline*> layers{&upper, &lower};
   const auto exported = ComposeVideoTracks(layers);
   struct Sample { double projectTime; const char *source; double sourceTime; };
   const std::vector<Sample> samples{
      {0, "lower.mp4", 0}, {.5, "lower.mp4", .5},
      {2, "upper.mp4", .4}, {3, "upper.mp4", 1.4},
      {3.5, "", 0}, {4, "lower.mp4", 3}, {6.5, "lower.mp4", 5.5},
      {-1, "", 0}, {7, "", 0}
   };
   for (int repeat = 0; repeat != 12; ++repeat) for (const auto &sample : samples) {
      const auto preview = EvaluateVideoLayers(layers, sample.projectTime);
      const auto output = PictureAt(exported, sample.projectTime);
      const bool visible = sample.source[0] != 0;
      REQUIRE(bool(preview.clip) == visible);
      REQUIRE(output.visible == visible);
      if (!visible) continue;
      REQUIRE(preview.sourceTime == Approx(sample.sourceTime));
      REQUIRE(output.sourceTime == Approx(sample.sourceTime));
      const auto previewSource = SourceFor(*preview.timeline, *preview.clip);
      const auto outputSource = SourceFor(exported, *ClipAt(exported, sample.projectTime));
      REQUIRE(previewSource.path == sample.source);
      REQUIRE(outputSource.path == sample.source);
      REQUIRE(outputSource.frameRate == previewSource.frameRate);
      REQUIRE(outputSource.width == previewSource.width);
      REQUIRE(outputSource.height == previewSource.height);
   }
   // These are borrowed queries: original identity, source handles and track
   // membership survive both successful composition and rejected inputs.
   REQUIRE(lower.clips[1].id == 2);
   REQUIRE(lower.clips[1].sourceStart == 3);
   REQUIRE(lower.clips[1].start == 4);
   REQUIRE(upper.clips[0].sourceStart == Approx(.4));
   REQUIRE(upper.clips[0].duration == Approx(1.2));
   REQUIRE(EvaluateVideoLayers(layers, 4).clip == &lower.clips[1]);
   REQUIRE_FALSE(EvaluateVideoLayers({}, 1).clip);
   REQUIRE_FALSE(EvaluateVideoLayers(layers,
      std::numeric_limits<double>::quiet_NaN()).clip);
   REQUIRE_THROWS(EvaluateVideoLayers({&upper, nullptr}, 2));
   REQUIRE_THROWS(ComposeVideoTracks({&upper, nullptr}));
   REQUIRE(ComposeVideoTracks(layers).End() == 7);
}

TEST_CASE("Export settings preserve stream copy only when picture settings are unchanged")
{
   VideoExport::ExportSettings settings;
   REQUIRE(VideoExport::AllowsOriginalPictureCopy(settings));
   settings.frameRate = 30000.0 / 1001;
   REQUIRE_FALSE(VideoExport::AllowsOriginalPictureCopy(settings));
   settings.frameRate = 0; settings.width = 1920; settings.height = 1080;
   REQUIRE_FALSE(VideoExport::AllowsOriginalPictureCopy(settings));
   settings.width = 1919;
   REQUIRE_THROWS(VideoExport::RequireUsableExportSettings(settings));
   settings.width = 0;
   REQUIRE_THROWS(VideoExport::RequireUsableExportSettings(settings));
   settings = {}; settings.preset = "untrusted-external-encoder";
   REQUIRE_THROWS(VideoExport::VideoEncoderArguments(settings));
   settings = {}; settings.frameRate = std::numeric_limits<double>::quiet_NaN();
   REQUIRE_THROWS(VideoExport::RequireUsableExportSettings(settings));
}

TEST_CASE("Relinking preserves moves, trims and IDs and rejects too-short media")
{
   auto timeline = VideoTimeline::CreateVideoTimeline("old.mp4", 10, 30, 1920, 1080);
   VideoTimeline::SplitClipAt(timeline, 3);
   VideoTimeline::MoveClipTo(timeline, 2, 6);
   const auto before = timeline;
   REQUIRE_THROWS(VideoTimeline::ReplaceVideoSource(timeline, "old.mp4",
      {"short.mp4", 2, 30, 1920, 1080}));
   REQUIRE(timeline.sourcePath == before.sourcePath);
   REQUIRE(timeline.clips[1].start == before.clips[1].start);
   REQUIRE(VideoTimeline::ReplaceVideoSource(timeline, "old.mp4",
      {"moved.mp4", 10, 30, 1920, 1080}));
   REQUIRE(timeline.clips[1].id == before.clips[1].id);
   REQUIRE(timeline.clips[1].start == before.clips[1].start);
   REQUIRE(timeline.clips[1].sourceStart == before.clips[1].sourceStart);
   REQUIRE_FALSE(VideoTimeline::ReplaceVideoSource(timeline, "old.mp4",
      {"moved.mp4", 10, 30, 1920, 1080}));
}

TEST_CASE("Clip flags retain content-relative placement across video moves, splits and clipboard edits")
{
   auto timeline = VideoTimeline::CreateVideoTimeline("flags.mp4", 10, 30);
   const std::string id(32, 'a');
   REQUIRE(ClipMarkers::PutMarker(timeline.clips[0].markers, {id, 4, "Sync clap"}));
   REQUIRE_FALSE(ClipMarkers::PutMarker(timeline.clips[0].markers, {id, 4, "Sync clap"}));
   VideoTimeline::SplitClipAt(timeline, 3);
   REQUIRE(timeline.clips[0].markers.size() == 1);
   REQUIRE(timeline.clips[1].markers.size() == 1);
   VideoTimeline::MoveClipTo(timeline, 2, 8);
   const auto &right = timeline.clips[1];
   REQUIRE(ClipMarkers::ProjectMarkerTime(right.markers[0], right.start-right.sourceStart, 1) == Approx(9));
   auto clipboard = VideoTimeline::CopyVideoRange(timeline, 8, 10);
   REQUIRE(clipboard.clips[0].markers[0].text == "Sync clap");
   REQUIRE(ClipMarkers::ProjectMarkerTime(clipboard.clips[0].markers[0],
      clipboard.clips[0].start-clipboard.clips[0].sourceStart, 1) == Approx(1));
   auto before = clipboard.clips[0].markers;
   REQUIRE_THROWS(ClipMarkers::PutMarker(clipboard.clips[0].markers, {"bad-id", 1, "Invalid"}));
   REQUIRE(clipboard.clips[0].markers[0].id == before[0].id);
   REQUIRE(ClipMarkers::ProjectMarkerTime(before[0], 2, 0.5) == Approx(4));
}
