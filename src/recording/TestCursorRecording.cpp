#include "PlanCursorRecording.h"
#include <catch2/catch.hpp>
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
}
