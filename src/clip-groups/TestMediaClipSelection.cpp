#include "SelectMediaClips.h"
#include "PlanRippleDeletion.h"
#include <catch2/catch.hpp>
#include <limits>
using ClipGroups::TemporaryClipSelection;

TEST_CASE("Ripple deletion closes selected and linked track scope before editing", "[ripple-deletion]")
{
   using namespace ClipGroups;
   const std::vector<RippleTrackInput<std::string>> tracks{
      {"picture", true, "embedded", {{0, 8, false, {"sound"}}, {10, 12, false, {"later"}}}},
      {"sound", false, {}, {{0, 8, false, {"sound"}}}},
      {"overlay", false, "embedded", {{10, 12, false, {"caption-sound"}}}},
      {"later", false, {}, {{10, 12, false, {"later"}}}},
      {"caption-sound", false, {}, {{10, 12, false, {"caption-sound"}}}},
      {"unrelated", false, {}, {{0, 12, false, {}}}},
   };
   for (int repeat{}; repeat < 2; ++repeat) {
      auto result = PlanRippleDeletion(tracks, 2, 4, false);
      REQUIRE(result == std::vector<std::string>{"picture", "sound", "overlay", "later", "caption-sound"});
      REQUIRE(PlanRippleDeletion(tracks, 2, 4, true).size() == 6);
      auto reordered = tracks; std::reverse(reordered.begin(), reordered.end());
      auto other = PlanRippleDeletion(reordered, 2, 4, false);
      std::sort(result.begin(), result.end()); std::sort(other.begin(), other.end());
      REQUIRE(result == other);
   }
}
TEST_CASE("Ripple locks in an expanded partner reject the entire plan", "[ripple-deletion]")
{
   using namespace ClipGroups;
   auto tracks = std::vector<RippleTrackInput<int>>{
      {1, true, {}, {{4, 10, false, {"link"}}}},
      {2, false, {}, {{4, 10, true, {"link"}}}},
      {3, false, {}, {{0, 1, true, {"old"}}}},
   };
   for (int repeat{}; repeat < 2; ++repeat) {
      REQUIRE_THROWS(PlanRippleDeletion(tracks, 2, 3, false));
      REQUIRE(tracks[1].clips[0].locked);
      auto unlocked = tracks; unlocked[1].clips[0].locked = false;
      REQUIRE(PlanRippleDeletion(unlocked, 2, 3, true) == std::vector<int>{1, 2, 3});
      unlocked[0].selected = false;
      REQUIRE(PlanRippleDeletion(unlocked, 2, 3, false).empty());
   }
}
TEST_CASE("Unchanged earlier links do not widen ripple deletion", "[ripple-deletion]")
{
   using namespace ClipGroups;
   const std::vector<RippleTrackInput<int>> tracks{
      {10, true, {}, {{0, 1, false, {"old"}}, {5, 8, false, {}}}},
      {20, false, {}, {{0, 1, false, {"old"}}, {5, 8, true, {}}}},
   };
   REQUIRE(PlanRippleDeletion(tracks, 2, 4, false) == std::vector<int>{10});
   REQUIRE_THROWS(PlanRippleDeletion(tracks, 2, 4, true));
   for (const auto range : std::vector<std::pair<double, double>>{{0, 0}, {3, 2}, {-1, 2},
         {0, std::numeric_limits<double>::infinity()}})
      REQUIRE_THROWS(PlanRippleDeletion(tracks, range.first, range.second, false));
   auto duplicate = tracks; duplicate[1].id = 10;
   REQUIRE_THROWS(PlanRippleDeletion(duplicate, 2, 4, false));
}

TEST_CASE("Temporary selection adds and removes individual clips without grouping", "[clip-selection]")
{
   TemporaryClipSelection selection;
   selection.Click(10, false);
   REQUIRE_FALSE(selection.IsExplicit());
   REQUIRE(selection.MovingKeys().empty());
   selection.Click(20, true);
   REQUIRE(selection.MovingKeys() == std::vector<std::uint64_t>{20, 10});
   selection.Click(10, true);
   REQUIRE(selection.MovingKeys() == std::vector<std::uint64_t>{20});
   selection.Click(20, true);
   REQUIRE(selection.IsExplicit());
   REQUIRE(selection.MovingKeys().empty());
}

TEST_CASE("Dragging any selected anchor retains the exact set and replaces unrelated selection", "[clip-selection]")
{
   TemporaryClipSelection selection;
   selection.Click(1, false); selection.Click(2, true); selection.Click(3, true);
   selection.Click(1, false);
   REQUIRE(selection.MovingKeys() == std::vector<std::uint64_t>{1, 2, 3});
   selection.Click(2, false);
   REQUIRE(selection.MovingKeys() == std::vector<std::uint64_t>{2, 1, 3});
   selection.Click(4, false);
   REQUIRE(selection.MovingKeys() == std::vector<std::uint64_t>{4});
}

TEST_CASE("Expired clip identities cannot rejoin a temporary move selection", "[clip-selection]")
{
   TemporaryClipSelection selection;
   selection.Click(1, true); selection.Click(2, true);
   selection.Retain({1});
   REQUIRE(selection.MovingKeys() == std::vector<std::uint64_t>{1});
   selection.Retain({1, 2});
   REQUIRE(selection.MovingKeys() == std::vector<std::uint64_t>{1});
   selection.Click(0, true);
   REQUIRE(selection.MovingKeys() == std::vector<std::uint64_t>{1});
   selection.Clear(); selection.Clear();
   REQUIRE_FALSE(selection.IsExplicit());
   REQUIRE(selection.Keys().empty());
}
