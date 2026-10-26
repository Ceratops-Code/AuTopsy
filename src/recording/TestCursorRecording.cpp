#include "PlanCursorRecording.h"
#include <catch2/catch.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace CursorRecording;

TEST_CASE("Cursor recording chooses the gap instead of the track end")
{
   const std::vector<OccupiedRange> clips{{0, 2}, {6, 8}, {10, 12}};
   for (int repeat = 0; repeat != 3; ++repeat) {
      const auto plan = PlanRecordingAtCursor(3, {}, 48000, clips);
      REQUIRE(plan.start == 3);
      REQUIRE(plan.stop == 6);
   }
   REQUIRE(clips[1].start == 6);
   REQUIRE(PlanRecordingAtCursor(2, {}, 48000, clips).stop == 6);
   REQUIRE_FALSE(PlanRecordingAtCursor(12, {}, 48000, clips).stop);
}

TEST_CASE("Cursor recording rejects occupied starts without changing input")
{
   const std::vector<OccupiedRange> clips{{1, 3}, {7, 9}};
   for (int repeat = 0; repeat != 2; ++repeat) {
      REQUIRE_THROWS(PlanRecordingAtCursor(1, {}, 44100, clips));
      REQUIRE_THROWS(PlanRecordingAtCursor(2, {}, 44100, clips));
      REQUIRE(PlanRecordingAtCursor(3, {}, 44100, clips).stop == 7);
   }
   REQUIRE(clips[0].end == 3);
}

TEST_CASE("Cursor recording respects every capture track and selection end")
{
   const std::vector<OccupiedRange> clips{{9, 12}, {5, 6}, {0, 2}, {4, 4}};
   REQUIRE(PlanRecordingAtCursor(3, {}, 48000, clips).stop == 5);
   REQUIRE(PlanRecordingAtCursor(3, 4, 48000, clips).stop == 4);
   REQUIRE(PlanRecordingAtCursor(3, 8, 48000, clips).stop == 5);
   REQUIRE(PlanRecordingAtCursor(3, 4, 48000, {}).stop == 4);
}

TEST_CASE("Cursor recording sample edges never overwrite the next clip")
{
   auto plan = PlanRecordingAtCursor(1.0004, {}, 1000, {{1.0028, 2}});
   REQUIRE(plan.start == Approx(1));
   REQUIRE(*plan.stop == Approx(1.002));
   REQUIRE(*plan.stop <= 1.0028);
   REQUIRE_THROWS(PlanRecordingAtCursor(1, {}, 1000, {{1.0004, 2}}));
   REQUIRE_THROWS(PlanRecordingAtCursor(0.9996, {}, 1000, {{1, 2}}));
   REQUIRE(PlanRecordingAtCursor(3, {}, 44100, {{1, 3}}).start == 3);
}

TEST_CASE("Cursor recording rejects unusable requests before capture")
{
   const auto nan = std::numeric_limits<double>::quiet_NaN();
   const auto inf = std::numeric_limits<double>::infinity();
   REQUIRE_THROWS(PlanRecordingAtCursor(nan, {}, 48000, {}));
   REQUIRE_THROWS(PlanRecordingAtCursor(-1, {}, 48000, {}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, {}, 0, {}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, {}, inf, {}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, 1, 48000, {}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, nan, 48000, {}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, {}, 48000, {{3, 2}}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, {}, 48000, {{nan, 2}}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, {}, 48000, {{1e100, 2e100}}));
}

TEST_CASE("Recording gap boundaries are independent of track ordering and sample rate")
{
   for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
      // Integer sample positions give an independent oracle: exactly seven
      // complete samples fit, regardless of which capture track is visited first.
      std::vector<OccupiedRange> ranges{{10 / rate, 19 / rate},
         {22 / rate, 31 / rate}, {0, 3 / rate}, {7 / rate, 7 / rate}};
      const auto original = ranges;
      for (unsigned turn = 0; turn != ranges.size(); ++turn) {
         for (int repeat = 0; repeat != 2; ++repeat) {
            const auto plan = PlanRecordingAtCursor(3 / rate, {}, rate, ranges);
            REQUIRE(plan.start == Approx(3 / rate));
            REQUIRE(plan.stop == Approx(10 / rate));
            REQUIRE((*plan.stop - plan.start) * rate == Approx(7));
            REQUIRE(PlanRecordingAtCursor(3 / rate, 8 / rate, rate, ranges).stop
               == Approx(8 / rate));
         }
         std::rotate(ranges.begin(), ranges.begin() + 1, ranges.end());
      }
      for (unsigned i = 0; i != ranges.size(); ++i) {
         REQUIRE(ranges[i].start == original[i].start);
         REQUIRE(ranges[i].end == original[i].end);
      }
   }
}

TEST_CASE("Recording refuses a sub-sample gap and accepts its exact one-sample boundary")
{
   for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0}) {
      for (const double start : {0.0, 1.0, 21600.0}) {
         for (int repeat = 0; repeat != 2; ++repeat) {
            REQUIRE_THROWS(PlanRecordingAtCursor(start, start + 0.4 / rate, rate, {}));
            REQUIRE_THROWS(PlanRecordingAtCursor(start, {}, rate,
               {{start + 0.4 / rate, start + 1}}));
            const auto plan = PlanRecordingAtCursor(start, start + 1 / rate, rate, {});
            REQUIRE(plan.start == Approx(start));
            REQUIRE((*plan.stop - plan.start) * rate == Approx(1).margin(1e-5));
         }
      }
   }
}

TEST_CASE("Recording rejects corrupt later tracks even when an earlier track supplies a stop")
{
   const auto nan = std::numeric_limits<double>::quiet_NaN();
   const auto inf = std::numeric_limits<double>::infinity();
   for (const OccupiedRange bad : {OccupiedRange{7, nan}, {inf, inf}, {8, 7}}) {
      for (int repeat = 0; repeat != 2; ++repeat) {
         REQUIRE_THROWS(PlanRecordingAtCursor(3, 4, 48000, {{5, 6}, bad}));
         REQUIRE_THROWS(PlanRecordingAtCursor(3, 4, 48000, {bad, {5, 6}}));
      }
   }
   REQUIRE_THROWS(PlanRecordingAtCursor(0x1p53 / 48000, {}, 48000, {}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, {}, nan, {}));
   REQUIRE_THROWS(PlanRecordingAtCursor(1, inf, 48000, {}));
}
