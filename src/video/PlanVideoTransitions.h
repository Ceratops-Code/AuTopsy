// Host-free opacity composition. Picture and audio keep the same project clock.
#pragma once
#include "EditVideoTimeline.h"
#include <cstdint>

namespace VideoEditing {
struct VideoGeneratorWindow {
   double contentStart{}, duration{}, frameRate{}, inputStart{}, inputEnd{};
   std::int64_t frameCount{};
};
// A visible window, snapped outwards to the generator's own picture clock.
// Hidden source handles stay as metadata and never cause an eager full render.
VideoGeneratorWindow PlanVideoGeneratorWindow(const VideoTimeline::VideoClipContent &content,
   double from, double to);
struct VideoBlendLayer {
   VideoTimeline::MediaSource source;
   double sourceStart{}, playbackRate{1};
   VideoFrameCrop crop;
   std::vector<VideoOpacityRamp> ramps;
   bool fillsCanvas{};
   VideoFrameRectangle rectangle;
   std::shared_ptr<const VideoTimeline::VideoClipContent> generated;
};
struct VideoBlendSpan {
   std::int64_t firstFrame{}, frameCount{};
   std::vector<VideoBlendLayer> layers; // Bottom to top. Empty means black.
   bool requiresBlend{};
};
bool HasVideoOpacity(const VideoTimeline::Clip &clip);
bool HasVideoOpacity(const std::vector<const VideoTimeline::Timeline*> &layers);
bool RequiresVideoComposition(const std::vector<const VideoTimeline::Timeline*> &layers);
std::vector<VideoBlendSpan> PlanVideoTransitions(const std::vector<const VideoTimeline::Timeline*> &layers,
   double duration, double frameRate);
// All inputs use FFmpeg -copyts -start_at_zero. The graph consumes original
// source timestamps and publishes an exact, zero-based integer frame sequence.
std::string VideoTransitionFilterGraph(const VideoBlendSpan &span, int width, int height, double frameRate);
}
