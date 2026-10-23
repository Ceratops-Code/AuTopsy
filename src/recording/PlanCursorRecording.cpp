#include "PlanCursorRecording.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace CursorRecording {
RecordingPlan PlanRecordingAtCursor(double cursor, std::optional<double> selectionEnd,
   double sampleRate, const std::vector<OccupiedRange>& occupied)
{
   // Doubles must still distinguish adjacent samples at the chosen position.
   const auto usableTime = [sampleRate](double value) {
      return std::isfinite(value) && value >= 0 && value * sampleRate < 0x1p53;
   };
   if (!std::isfinite(sampleRate) || sampleRate <= 0 || !usableTime(cursor))
      throw std::invalid_argument("Choose a nonnegative recording position and a usable sample rate.");
   RecordingPlan plan{std::round(cursor * sampleRate) / sampleRate, {}};
   if (selectionEnd) {
      if (!usableTime(*selectionEnd) || *selectionEnd <= cursor)
         throw std::invalid_argument("The recording selection must end after the cursor.");
      plan.stop = *selectionEnd;
   }
   for (const auto& range : occupied) {
      if (!std::isfinite(range.start) || !std::isfinite(range.end) || range.end < range.start)
         throw std::invalid_argument("A recording track has an unusable clip range.");
      if (range.end == range.start) continue;
      if (range.start <= plan.start && plan.start < range.end)
         throw std::runtime_error("The recording cursor is inside an existing clip. Move it into free space or record on a new track.");
      if (range.start > plan.start)
         plan.stop = plan.stop ? std::min(*plan.stop, range.start) : range.start;
   }
   if (plan.stop) {
      if (!usableTime(*plan.stop))
         throw std::invalid_argument("The recording end exceeds the usable sample range.");
      // Stop on the last complete recording sample before the next clip. A tiny
      // rounding allowance only repairs representation of an exact sample edge.
      *plan.stop = std::floor(*plan.stop * sampleRate + 1e-7) / sampleRate;
      if (*plan.stop <= plan.start)
         throw std::runtime_error("There is no complete recording sample before the next clip or selection end.");
   }
   return plan;
}
}
