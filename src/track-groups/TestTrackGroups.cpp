#include "EditTrackGroups.h"
#include "RenderGroupWaveform.h"
#include <catch2/catch.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>
using namespace TrackGroups;
namespace { std::string Id(char value) { return std::string(32, value); } }

TEST_CASE("Track folders gather nonadjacent tracks without moving unrelated content")
{
   const auto a = Id('a'), b = Id('b'), c = Id('c'), group = Id('d');
   const std::vector<GroupRow> original{{a}, {b}, {c}};
   for (int repeat{}; repeat < 3; ++repeat) {
      const auto plan = PlanTrackGroup(original, {c, a}, group);
      REQUIRE(plan.members == std::vector<std::string>{a, c});
      REQUIRE(plan.order == std::vector<std::string>{group, a, c, b});
      REQUIRE(original[0].group.empty());
   }
   REQUIRE_THROWS(PlanTrackGroup(original, {a}, group));
   REQUIRE_THROWS(PlanTrackGroup(original, {a, a}, group));
   REQUIRE_THROWS(PlanTrackGroup(original, {a, Id('e')}, group));
   REQUIRE_THROWS(PlanTrackGroup(original, {a, b}, a));
}
TEST_CASE("Collapsing folders hides only real members and leaves orphan audio accessible")
{
   const auto a = Id('a'), b = Id('b'), group = Id('c');
   std::vector<GroupRow> rows{{group, {}, true, true}, {a, group}, {b, Id('d')}};
   REQUIRE(IsCollapsedGroupMember(rows, a));
   REQUIRE_FALSE(IsCollapsedGroupMember(rows, b));
   REQUIRE_FALSE(IsCollapsedGroupMember(rows, group));
   rows[0].collapsed = false;
   REQUIRE_FALSE(IsCollapsedGroupMember(rows, a));
   rows.erase(rows.begin());
   REQUIRE_FALSE(IsCollapsedGroupMember(rows, a));
   REQUIRE(ArrangeGroupRows(rows) == std::vector<std::string>{a, b});
   REQUIRE_THROWS(CheckGroupRows({{a}, {a}}));
   REQUIRE_THROWS(CheckGroupRows({{group, a, true, true}}));
}
TEST_CASE("Folder movement keeps whole blocks and cannot insert inside another folder")
{
   const auto g = Id('a'), h = Id('b'), x = Id('c'), y = Id('d'), z = Id('e');
   std::vector<GroupRow> rows{{g, {}, true}, {x, g}, {h, {}, true}, {y, h}, {z}};
   REQUIRE(MoveGroupRows(rows, g, y) == std::vector<std::string>{g, x, h, y, z});
   REQUIRE(MoveGroupRows(rows, h, x) == std::vector<std::string>{h, y, g, x, z});
   REQUIRE(MoveGroupRows(rows, g, {}) == std::vector<std::string>{h, y, z, g, x});
   REQUIRE(MoveGroupRows(rows, g, x) == ArrangeGroupRows(rows));
   REQUIRE_THROWS(MoveGroupRows(rows, Id('f'), {}));
   REQUIRE_THROWS(MoveGroupRows(rows, g, Id('f')));
   rows = {{x, g}, {g, {}, true}, {z}, {y, g}};
   const auto arranged = ArrangeGroupRows(rows);
   REQUIRE(arranged == std::vector<std::string>{g, x, y, z});
}
TEST_CASE("Summed waveform retains transients and phase cancellation across chunk boundaries")
{
   for (int repeat{}; repeat < 3; ++repeat) {
      std::vector<float> first(9001), second(9001);
      first[0] = 0.5f; first[4095] = -0.75f; first[4096] = 0.9f; first.back() = 0.7f;
      for (bool cancel : {false, true}) {
         size_t offset{};
         for (size_t i{}; i < first.size(); ++i) second[i] = cancel ? -first[i] : first[i];
         auto read = [&](float *buffer, size_t wanted) {
            const auto count = std::min<size_t>({wanted, 701, first.size() - offset});
            for (size_t i{}; i < count; ++i) buffer[i] = first[offset + i] + second[offset + i];
            offset += count; return count;
         };
         const auto result = RenderSummedWaveform(read, first.size(), 9);
         REQUIRE(result); REQUIRE(result->size() == 9);
         REQUIRE((*result)[0].maximum == (cancel ? 0 : 1.0f));
         REQUIRE((*result)[4].minimum == (cancel ? 0 : -1.5f));
         REQUIRE((*result)[4].maximum == (cancel ? 0 : 1.8f));
         REQUIRE(result->back().maximum == (cancel ? 0 : 1.4f));
      }
   }
}
TEST_CASE("Waveform cancellation and failed reads publish no partial summary")
{
   for (int boundary : {0, 1, 2}) {
      int calls{};
      const auto result = RenderSummedWaveform([](float *buffer, size_t count) {
         std::fill_n(buffer, count, 0.5f); return count;
      }, 8192, 4, [&] { return calls++ == boundary; });
      REQUIRE_FALSE(result);
   }
   REQUIRE_THROWS(RenderSummedWaveform([](float *, size_t) { return size_t{}; }, 10, 2));
   REQUIRE_THROWS(RenderSummedWaveform([](float *buffer, size_t count) {
      std::fill_n(buffer, count, std::numeric_limits<float>::quiet_NaN()); return count;
   }, 10, 2));
   const auto silence = RenderSummedWaveform([](float *, size_t) { return size_t{}; }, 0, 2);
   REQUIRE(silence); REQUIRE((*silence)[0].maximum == 0);
   const auto dc = RenderSummedWaveform([](float *buffer, size_t count) {
      std::fill_n(buffer, count, 0.5f); return count;
   }, 10, 2);
   REQUIRE(dc); REQUIRE((*dc)[0].minimum == 0.5f); REQUIRE((*dc)[0].maximum == 0.5f);
}
