// Owned navigation policy. An occurrence key identifies a current clip/flag
// pair, including copied flags sharing a persisted marker ID. Never a pane row.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>
namespace ClipMarkers {
struct FlagLocation {
   uint64_t occurrence{};
   double time{};
   bool visible{};
};
inline std::vector<FlagLocation> OrderClipFlags(std::vector<FlagLocation> flags)
{
   std::unordered_set<uint64_t> keys;
   for (const auto &flag : flags)
      if (!flag.occurrence || !keys.insert(flag.occurrence).second || !std::isfinite(flag.time))
         throw std::runtime_error("Clip flag locations require distinct occurrences and finite times.");
   std::sort(flags.begin(), flags.end(), [](const auto &a, const auto &b) {
      return a.time < b.time || (a.time == b.time && a.occurrence < b.occurrence);
   });
   return flags;
}
// Strict time navigation skips tied flags at the cursor and never wraps. The
// navigator list can still select each coincident flag individually.
inline std::optional<uint64_t> AdjacentClipFlag(const std::vector<FlagLocation> &flags,
   double cursor, bool forward)
{
   if (!std::isfinite(cursor)) throw std::runtime_error("Choose a finite flag navigation time.");
   const auto ordered = OrderClipFlags(flags);
   std::optional<uint64_t> result;
   for (const auto &flag : ordered) if (flag.visible && flag.time >= 0) {
      if (forward && flag.time > cursor + 1e-7) return flag.occurrence;
      if (!forward && flag.time < cursor - 1e-7) result = flag.occurrence;
   }
   return result;
}
}
