#include "PlanVideoExport.h"
#include "RenderVideoComposition.h"
#include <cmath>
#include <stdexcept>

namespace VideoExport {
std::pair<int, int> CalculateVideoExportCanvas(int width, int height,
   const VideoEditing::VideoFrameCrop &crop)
{
   const auto rectangle = VideoEditing::CalculateVideoCropRectangle(crop, width, height, 0);
   return {std::max(2, rectangle.width & ~1), std::max(2, rectangle.height & ~1)};
}

std::vector<VideoExportFrameSpan> PlanVideoExportFrames(
   const VideoTimeline::Timeline &timeline, double duration, double frameRate)
{
   VideoTimeline::RequireWellFormedTimeline(timeline, VideoTimeline::MaximumVideoRenderSegments);
   if (!std::isfinite(duration) || duration <= 0 || duration + 1e-7 < timeline.End() ||
       !std::isfinite(frameRate) || frameRate <= 0 || frameRate > 1000)
      throw std::runtime_error("Video export requires a usable duration and frame rate.");
   const auto boundary = [frameRate](double seconds) {
      const auto exact = static_cast<long double>(seconds) * frameRate;
      // Grid tolerance must not erase the only frame of a very slow source.
      const auto frames = std::ceil(exact - std::min(1e-7L, exact / 2));
      // Beyond this range double project times cannot distinguish frame edges.
      if (!std::isfinite(frames) || frames < 0 || frames > 4503599627370495.0L)
         throw std::runtime_error("The video export frame count is outside supported bounds.");
      return static_cast<std::int64_t>(frames);
   };
   std::vector<VideoExportFrameSpan> result;
   std::int64_t cursor{};
   for (const auto &clip : timeline.clips) {
      const auto first = std::max(cursor, boundary(clip.start));
      const auto last = std::max(first, boundary(clip.End()));
      if (first > cursor) result.push_back({0, cursor, first - cursor});
      if (last > first) {
         // Preserve the global phase when a source cut is between output frames.
         const auto sourceStart = clip.sourceStart +
            (static_cast<double>(first) / frameRate - clip.start) * clip.playbackRate;
         result.push_back({clip.id, first, last - first, sourceStart,
            clip.sourceStart + clip.duration * clip.playbackRate, clip.playbackRate, clip.crop});
      }
      cursor = last;
   }
   const auto end = boundary(duration);
   if (end > cursor) result.push_back({0, cursor, end - cursor});
   return result;
}
}
