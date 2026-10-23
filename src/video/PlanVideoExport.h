// Host-free export geometry and one frame clock shared by every rendered span.
#pragma once
#include "EditVideoTimeline.h"
#include <cstdint>
#include <utility>
#include <vector>

namespace VideoExport {
struct VideoExportFrameSpan {
   std::uint64_t clipId{}; // Zero identifies generated gap frames.
   std::int64_t firstFrame{}, frameCount{};
   double sourceStart{}, sourceEnd{}, playbackRate{1};
   VideoEditing::VideoFrameCrop crop;
};
// Every boundary uses the same project clock. Sub-frame spans can disappear,
// but rounding cannot accumulate at edits or move later picture against audio.
std::vector<VideoExportFrameSpan> PlanVideoExportFrames(
   const VideoTimeline::Timeline &timeline, double duration, double frameRate);
// Automatic dimensions follow the first rendered picture's crop. The encoder
// needs even dimensions; rounding changes at most one pixel on each axis.
std::pair<int, int> CalculateVideoExportCanvas(int orientedWidth, int orientedHeight,
   const VideoEditing::VideoFrameCrop &crop);
}
