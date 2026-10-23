#include "SelectMediaClips.h"
#include <catch2/catch.hpp>
using ClipGroups::TemporaryClipSelection;

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
