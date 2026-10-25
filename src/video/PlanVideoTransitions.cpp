#include "PlanVideoTransitions.h"
#include "PlanVideoExport.h"
#include "RenderVideoComposition.h"
#include <iomanip>
#include <locale>
#include <sstream>

namespace VideoEditing {
bool HasVideoOpacity(const std::vector<const VideoTimeline::Timeline*> &layers)
{
   return std::any_of(layers.begin(), layers.end(), [](const auto *timeline) {
      if (!timeline) throw std::runtime_error("A video layer was removed.");
      return std::any_of(timeline->clips.begin(), timeline->clips.end(), [](const auto &clip) { return HasVideoOpacity(clip); });
   });
}
std::vector<VideoBlendSpan> PlanVideoTransitions(const std::vector<const VideoTimeline::Timeline*> &layers,
   double duration, double frameRate)
{
   VideoExport::ProjectTimeFrameBoundary(duration, frameRate);
   std::vector<double> edges{0, duration};
   for (const auto *timeline : layers) {
      if (!timeline) throw std::runtime_error("A video layer was removed.");
      VideoTimeline::RequireWellFormedTimeline(*timeline);
      for (const auto &clip : timeline->clips) {
         const auto cuts = VideoTimeline::VideoContentEdges(clip);
         for (const auto edge : cuts) if (edge > 0 && edge < duration) edges.push_back(edge);
      }
   }
   std::sort(edges.begin(), edges.end());
   edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
   if (edges.size() > VideoTimeline::MaximumVideoRenderSegments)
      throw std::runtime_error("The project has too many picture transition segments.");
   std::vector<VideoBlendSpan> result;
   for (size_t i = 1; i < edges.size(); ++i) {
      const auto first = VideoExport::ProjectTimeFrameBoundary(edges[i - 1], frameRate);
      const auto last = VideoExport::ProjectTimeFrameBoundary(edges[i], frameRate);
      if (last <= first) continue;
      VideoBlendSpan span{first, last - first};
      const auto time = (static_cast<double>(first) + (last - first - 1) / 2.0) / frameRate;
      for (const auto *timeline : layers) {
         const auto *clip = VideoTimeline::ClipAt(*timeline, time);
         if (!clip) continue;
         const auto sample = VideoTimeline::SampleVideoContent(*clip, VideoTimeline::SourceFor(*timeline, *clip),
            clip->ContentTimeAtProjectTime(time));
         if (!sample || sample.opacity <= 0) continue;
         if (span.layers.size() >= 16) throw std::runtime_error("At most sixteen fading video layers can overlap at once.");
         span.layers.push_back({sample.source, sample.sourceTime -
            (time - static_cast<double>(first) / frameRate) * sample.playbackRate,
            sample.playbackRate, sample.crop, sample.opacityRamps});
         if (sample.opacity < 1) span.requiresBlend = true;
         else break;
      }
      std::reverse(span.layers.begin(), span.layers.end());
      result.push_back(std::move(span));
   }
   return result;
}

std::string VideoTransitionFilterGraph(const VideoBlendSpan &span, int width, int height, double rate)
{
   if (width < 2 || height < 2 || width % 2 || height % 2 || width > 16384 || height > 16384 ||
       span.firstFrame < 0 || span.frameCount <= 0 || !std::isfinite(rate) || rate <= 0 || rate > 1000 || span.layers.size() > 16)
      throw std::runtime_error("A picture transition needs usable even dimensions and an exact frame interval.");
   const auto from = static_cast<double>(span.firstFrame) / rate;
   double offset{};
   for (const auto &layer : span.layers) for (const auto &ramp : layer.ramps) {
      if (!std::isfinite(ramp.start) || !std::isfinite(ramp.end) || ramp.end <= ramp.start)
         throw std::runtime_error("A picture fade has invalid endpoints.");
      offset = std::max(offset, -ramp.start);
   }
   std::ostringstream graph; graph.imbue(std::locale::classic()); graph << std::setprecision(17);
   graph << "color=c=black:s=" << width << 'x' << height << ":r=" << rate << ":d=" << span.frameCount / rate
      << ",trim=end_frame=" << span.frameCount << ",setpts=N/(" << rate << "*TB)[base];\n";
   for (size_t i{}; i < span.layers.size(); ++i) {
      const auto &layer = span.layers[i];
      VideoTimeline::RequireUsableVideoSource(layer.source);
      if (!std::isfinite(layer.sourceStart) || layer.sourceStart < -1e-7 || !std::isfinite(layer.playbackRate) || layer.playbackRate <= 0)
         throw std::runtime_error("A picture layer has invalid source timing.");
      graph << '[' << i << ":v:0]setpts=(PTS-" << layer.sourceStart << "/TB)/" << layer.playbackRate
         << ",fps=" << rate << ":start_time=0:eof_action=pass,tpad=stop_mode=clone:stop=-1,trim=end_frame=" << span.frameCount
         << ",setpts=(N/" << rate << '+' << from + offset << ")/TB";
      const auto crop = VideoCropFilterExpression(layer.crop);
      if (!crop.empty()) graph << ',' << crop;
      graph << ",scale=" << width << ':' << height;
      if (!layer.fillsCanvas) graph << ":force_original_aspect_ratio=decrease:force_divisible_by=2,pad="
         << width << ':' << height << ":(ow-iw)/2:(oh-ih)/2:color=black";
      graph << ",setsar=1,format=rgba";
      for (const auto &ramp : layer.ramps)
         graph << ",fade=t=" << (ramp.increasing ? "in" : "out") << ":st=" << ramp.start + offset
            << ":d=" << ramp.end - ramp.start << ":alpha=1";
      graph << ",setpts=PTS-" << from + offset << "/TB[l" << i << "];\n";
      graph << (i ? "[b" + std::to_string(i - 1) + "]" : "[base]") << "[l" << i
         << "]overlay=shortest=1:format=auto[b" << i << "];\n";
   }
   graph << (span.layers.empty() ? "[base]" : "[b" + std::to_string(span.layers.size() - 1) + "]")
      << "trim=end_frame=" << span.frameCount << ",settb=expr=1/(" << rate << "),setpts=N,format=yuv420p[vout]\n";
   return graph.str();
}
}
