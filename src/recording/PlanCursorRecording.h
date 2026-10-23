// Recording policy uses project seconds and visible clip ranges, without host types.
#pragma once
#include <optional>
#include <vector>

namespace CursorRecording {
struct OccupiedRange { double start{}, end{}; };
struct RecordingPlan {
   double start{};
   std::optional<double> stop;
};
// Throws before capture starts if the cursor is occupied or no complete sample
// fits. Flatten ranges from every capture track: the earliest boundary wins.
RecordingPlan PlanRecordingAtCursor(double cursor, std::optional<double> selectionEnd,
   double sampleRate, const std::vector<OccupiedRange>& occupied);
}
