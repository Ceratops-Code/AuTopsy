#include "EditCaptionCues.h"
#include "JoinVideoClips.h"
#include <catch2/catch.hpp>
#include <limits>

using namespace VideoCaptions;

TEST_CASE("SRT and WebVTT keep Hebrew, multiline text and millisecond timing", "[video-captions]")
{
   const std::vector<Cue> original{{1001, 2789, u8"שלום עולם\nBlue headphones & green dinosaurs <3"}, {3600001, 3602003, "Later"}};
   for (const auto format : {FileFormat::Srt, FileFormat::WebVtt}) for (int repeat{}; repeat < 2; ++repeat) {
      const auto saved = WriteCaptionCues(original, format);
      const auto read = ReadCaptionCues(saved, format);
      REQUIRE(read.size() == original.size());
      for (size_t i{}; i < read.size(); ++i) {
         REQUIRE(read[i].startMs == original[i].startMs); REQUIRE(read[i].endMs == original[i].endMs);
         REQUIRE(read[i].text == original[i].text);
      }
   }
}
TEST_CASE("Subtitle readers handle BOM, CRLF, cue IDs, tags and WebVTT metadata", "[video-captions]")
{
   const auto srt = ReadCaptionCues("\xef\xbb\xbf" "4\r\n00:00:01,250 --> 00:00:03,500\r\n<i>Hello</i> &amp; goodbye\r\n\r\n", FileFormat::Srt);
   REQUIRE(srt.size() == 1); REQUIRE(srt[0].text == "Hello & goodbye"); REQUIRE(srt[0].startMs == 1250);
   const auto vtt = ReadCaptionCues("WEBVTT a title\n\nNOTE ignored\nnotes\n\nSTYLE\n::cue { color: red }\n\nspeaker\n00:01.250 --> 00:03.500 align:start position:10%\n<v Roman>Hello</v>\nworld\n", FileFormat::WebVtt);
   REQUIRE(vtt.size() == 1); REQUIRE(vtt[0].endMs == 3500); REQUIRE(vtt[0].text == "Hello\nworld");
   REQUIRE(ReadCaptionCues("WEBVTT\n\n", FileFormat::WebVtt).empty());
   REQUIRE(ReadCaptionCues("1\n00:00:00,000 --> 00:00:01,000\n1 < 2 and 3 > 1\n", FileFormat::Srt)[0].text == "1 < 2 and 3 > 1");
   REQUIRE_THROWS(WriteCaptionCues({{0, 1000, "First\n\nSecond"}}, FileFormat::Srt));
}
TEST_CASE("Malformed or excessive subtitles cannot produce a partial caption plan", "[video-captions]")
{
   const std::string good = "1\n00:00:00,000 --> 00:00:01,000\nGood\n\n";
   for (const auto *bad : {"2\n00:00:02,000 --> 00:00:01,000\nBackwards\n", "2\n00:60:00,000 --> 01:00:01,000\nBad minute\n",
         "2\n00:00:01,000 --> 00:00:01,000\nEmpty range\n", "2\n-00:00:01,000 --> 00:00:03,000\nNegative\n",
         "2\n00:00:02,000 --> 00:00:03,000\n<i></i>\n", "2\nno timing\nText\n"}) {
      for (int repeat{}; repeat < 2; ++repeat) REQUIRE_THROWS(ReadCaptionCues(good+bad, FileFormat::Srt));
   }
   REQUIRE_THROWS(ReadCaptionCues("WEBVTT\nX-TIMESTAMP-MAP=MPEGTS:900000,LOCAL:00:00:00.000\n\n", FileFormat::WebVtt));
   REQUIRE_THROWS(ReadCaptionCues(std::string(MaximumCaptionFileBytes+1, 'x'), FileFormat::Srt));
   REQUIRE_THROWS(ReadCaptionCues(good+std::string(1, '\0'), FileFormat::Srt));
}
TEST_CASE("Overlapping captions keep every active speaker on one non-overlapping track", "[video-captions]")
{
   const std::vector<Cue> input{{0, 3000, "First"}, {1000, 2000, "Second"}, {2000, 4000, "Third"}};
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto combined = CombineCaptionOverlaps(input);
      REQUIRE(combined.size() == 4); REQUIRE(combined[1].text == "First\nSecond");
      REQUIRE(combined[2].text == "First\nThird"); REQUIRE(combined[3].text == "Third");
      const auto plan = PlanCaptionClips(input, 1280, 288);
      REQUIRE(plan.clips.size() == 4); VideoTimeline::RequireWellFormedTimeline(plan);
      REQUIRE(plan.clips.back().End() == Approx(4)); REQUIRE(input.size() == 3);
   }
}
TEST_CASE("Recognition times are offset and bounded by the rendered project range", "[video-captions]")
{
   const std::vector<Cue> input{{0, 1800, "First"}, {1800, 3500, "Second"}, {6000, 7000, "Outside"}};
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto result = PlaceCaptionCues(input, 12, 12.5, 15);
      REQUIRE(result.size() == 2); REQUIRE(result[0].startMs == 12500); REQUIRE(result[1].endMs == 15000);
      REQUIRE(input[0].startMs == 0); REQUIRE(input[1].endMs == 3500);
      REQUIRE_THROWS(PlaceCaptionCues(input, 0, 5, 2));
   }
}
TEST_CASE("Caption export follows split, trim, move and speed instead of retained source timing", "[video-captions]")
{
   for (int repeat{}; repeat < 2; ++repeat) {
      auto timeline = PlanCaptionClips({{5000, 9000, "Words"}}, 1280, 288);
      const auto untouched = timeline;
      REQUIRE(VideoTimeline::SplitClipAt(timeline, 7));
      VideoTimeline::EraseVideoRange(timeline, 5, 6, false);
      REQUIRE(VideoTimeline::MoveClipTo(timeline, timeline.clips.front().id, 3));
      REQUIRE(VideoTimeline::ChangeVideoClipSpeed(timeline, timeline.clips.front().id, 2));
      const auto captions = ReadTimelineCaptions(timeline);
      REQUIRE(captions.size() == 2); REQUIRE(captions[0].startMs == 3000); REQUIRE(captions[0].endMs == 3500);
      REQUIRE(captions[1].startMs == 7000); REQUIRE(captions[1].endMs == 9000);
      REQUIRE(ReadTimelineCaptions(untouched)[0].startMs == 5000);
   }
}
TEST_CASE("Joined caption export retains word order through reverse and omits muted clips", "[video-captions]")
{
   for (int repeat{}; repeat < 2; ++repeat) {
      auto timeline = PlanCaptionClips({{0, 1000, "One"}, {2000, 3000, "Two"}}, 1280, 288);
      REQUIRE(VideoTimeline::JoinVideoClips(timeline, 0, 3));
      auto cues = ReadTimelineCaptions(timeline);
      REQUIRE(cues.size() == 2); REQUIRE(cues[0].text == "One"); REQUIRE(cues[1].startMs == 2000);
      REQUIRE(VideoTimeline::ReverseVideoClip(timeline, timeline.clips.front().id));
      cues = ReadTimelineCaptions(timeline); REQUIRE(cues.size() == 2); REQUIRE(cues[0].text == "Two");
      timeline.clips.front().muted = true; REQUIRE(ReadTimelineCaptions(timeline).empty());
   }
}
TEST_CASE("Recognition follows fast, slow and changing project clocks before clipping", "[video-captions]")
{
   const std::vector<Cue> cues{{1000, 2500, "First"}, {16000, 19000, "Late"}};
   for (int repeat{}; repeat < 2; ++repeat) {
      auto fast = MapRecognitionCues(cues, 12, 22, [](double t) { return 12+2*t; });
      REQUIRE(fast.size() == 1); REQUIRE(fast[0].startMs == 14000); REQUIRE(fast[0].endMs == 17000);
      auto slow = MapRecognitionCues(cues, 12, 22, [](double t) { return 12+.5*t; });
      REQUIRE(slow.size() == 2); REQUIRE(slow[1].startMs == 20000); REQUIRE(slow[1].endMs == 21500);
      auto changing = MapRecognitionCues(cues, 12, 22, [](double t) { return 12+t+.1*t*t; });
      REQUIRE(changing.size() == 1); REQUIRE(changing[0].startMs == 13100); REQUIRE(changing[0].endMs == 15125);
      REQUIRE_THROWS(MapRecognitionCues(cues, 12, 22, {}));
      REQUIRE_THROWS(MapRecognitionCues(cues, 12, 22, [](double t) { return 22-t; }));
      REQUIRE_THROWS(MapRecognitionCues(cues, 12, 22, [](double) { return std::numeric_limits<double>::quiet_NaN(); }));
      REQUIRE(cues[0].startMs == 1000); REQUIRE(cues[1].endMs == 19000);
   }
}
