// Pure composition planning, with no decoder, GUI, filesystem or native types.
// This foundation does not yet enable crop/PiP in the native renderer or format.
#pragma once
#include "PlanVideoEdits.h"

namespace VideoEditing {
struct VideoFrameCrop { double left{}, top{}, right{}, bottom{}; };
struct VideoFrameRectangle { double x{}, y{}, width{1}, height{1}; };
struct VideoClipPresentation {
   uint64_t clipId{};
   VideoFrameCrop crop;
   bool muted{};
};
struct VideoLayerPresentation {
   std::string trackId;
   VideoFrameRectangle rectangle; // Fractions of parent rectangle, or canvas for roots.
   bool muted{}, solo{};
   std::vector<VideoClipPresentation> clips;
};
struct VideoCompositionSettings {
   int canvasWidth{}, canvasHeight{};
   double duration{}; // Explicit; hiding a picture never shortens output/audio.
   std::vector<VideoLayerPresentation> layers; // Missing presentation uses defaults.
};
struct VideoFrameSample {
   std::string trackId;
   uint64_t clipId{};
   VideoTimeline::MediaSource source;
   double sourceTime{};
   VideoFrameCrop crop; // Fractions of oriented ORIGINAL source, before placement.
   VideoFrameRectangle rectangle; // Resolved canvas fractions; never proxy pixels.
   double playbackRate{1};
};
struct VideoFramePlan {
   int canvasWidth{}, canvasHeight{};
   double duration{};
   std::vector<VideoFrameSample> layers; // Bottom to top, including embedded layers.
};

// Bound groups solo together. A muted/gapped parent suppresses descendants;
// child gaps reveal the parent. Empty layers mean black, not a missing timeline.
// Owned results may outlive input snapshots; adapters consume this geometry for
// both preview and export, retaining audio's independent native clock and mix.
VideoFramePlan EvaluateVideoTimeline(const VideoProjectSnapshot &project,
   const VideoCompositionSettings &settings, double projectTime);
}
