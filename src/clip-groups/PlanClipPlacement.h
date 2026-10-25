// Owned timing policy. Native adapters supply intervals and publish one history edit.
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace ClipActions {
struct ClipTimingProperties {
   double start{}, end{}, speed{1};
   bool locked{};
};
struct ClipPlacementInterval { double start{}, end{}; };

inline void RequireClipTiming(const ClipTimingProperties &value)
{
   if (!std::isfinite(value.start) || !std::isfinite(value.end) || !std::isfinite(value.speed) ||
       value.start < 0 || value.end <= value.start || value.end > 100000000 || value.speed < .01 || value.speed > 100)
      throw std::runtime_error("Enter a non-negative start, an end after the start, and speed between 1% and 10000%.");
}

inline double FindGapBeforeClip(double start, const std::vector<ClipPlacementInterval> &others)
{
   if (!std::isfinite(start) || start < 0) throw std::runtime_error("Choose a usable clip position.");
   double previousEnd{};
   for (const auto &other : others) {
      if (!std::isfinite(other.start) || !std::isfinite(other.end) || other.start < 0 || other.end <= other.start)
         throw std::runtime_error("A neighbouring clip has invalid timing.");
      if (other.end <= start + 1e-7) previousEnd = std::max(previousEnd, std::min(start, other.end));
      else if (other.start < start - 1e-7) throw std::runtime_error("The clip overlaps another clip.");
   }
   return previousEnd;
}

inline void RequireFreeClipPlacement(const ClipTimingProperties &value,
   const std::vector<ClipPlacementInterval> &others)
{
   RequireClipTiming(value);
   for (const auto &other : others)
      if (other.start < value.end - 1e-7 && other.end > value.start + 1e-7)
         throw std::runtime_error("There is not enough free space. Move or trim the neighbouring clip first.");
}
inline void RequireConsistentClipRetiming(double oldDuration, double oldSpeed, const ClipTimingProperties &value)
{
   RequireClipTiming(value);
   const auto expected = oldDuration * oldSpeed / value.speed;
   if (!std::isfinite(expected) || std::abs(expected - (value.end - value.start)) > 1e-6)
      throw std::runtime_error("End, duration and speed must describe the same retained clip. Use edge handles for trimming.");
}
}
