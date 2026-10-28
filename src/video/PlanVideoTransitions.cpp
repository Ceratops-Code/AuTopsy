#include "PlanVideoTransitions.h"
#include "PlanVideoExport.h"
#include "RenderVideoComposition.h"
#include "EvaluateVideoTimeline.h"
#include <iomanip>
#include <locale>
#include <sstream>

namespace VideoEditing {
VideoGeneratorWindow PlanVideoGeneratorWindow(const VideoTimeline::VideoClipContent &content, double from, double to)
{
   const auto rate = content.frameRate;
   const auto total = VideoExport::ProjectTimeFrameBoundary(content.duration, rate);
   if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to <= from ||
       from >= content.duration || to > content.duration + 1e-7)
      throw std::runtime_error("The requested generated picture window is outside retained content.");
   const auto first = std::min(total-1, static_cast<std::int64_t>(std::floor(from * rate + 1e-8)));
   const auto last = std::min(total, std::max(first + 1, VideoExport::ProjectTimeFrameBoundary(to, rate)));
   VideoGeneratorWindow result{double(first) / rate, double(last-first) / rate, rate, 0, 0, last-first};
   using Operation = VideoTimeline::VideoClipContent::Operation;
   if (content.operation == Operation::Reverse || content.operation == Operation::Freeze) {
      if (content.parts.size() != 1) throw std::runtime_error("A picture effect needs one retained input.");
      const auto inputEnd = content.parts.front().End();
      if (content.operation == Operation::Reverse) {
         result.inputStart = double(total-last) / rate; result.inputEnd = std::min(inputEnd, double(total-first) / rate);
      }
      else {
         result.inputStart = std::floor(content.heldTime * rate + 1e-8) / rate;
         result.inputEnd = std::min(inputEnd, result.inputStart + 1 / rate);
      }
      if (result.inputEnd <= result.inputStart) throw std::runtime_error("The picture effect has no input frame in this window.");
   }
   return result;
}
bool HasVideoOpacity(const std::vector<const VideoTimeline::Timeline*> &layers)
{
   return std::any_of(layers.begin(), layers.end(), [](const auto *timeline) {
      if (!timeline) throw std::runtime_error("A video layer was removed.");
      return std::any_of(timeline->clips.begin(), timeline->clips.end(), [](const auto &clip) { return HasVideoOpacity(clip); });
   });
}
bool RequiresVideoComposition(const std::vector<const VideoTimeline::Timeline*> &layers)
{
   return HasVideoOpacity(layers) || std::any_of(layers.begin(), layers.end(), [](const auto *layer) {
      return !layer->placement.parentId.empty() || !(layer->placement.rectangle == VideoFrameRectangle{}) ||
         std::any_of(layer->clips.begin(), layer->clips.end(), VideoTimeline::HasGeneratedVideoContent);
   });
}
std::vector<VideoBlendSpan> PlanVideoTransitions(const std::vector<const VideoTimeline::Timeline*> &layers,
   double duration, double frameRate)
{
   VideoExport::ProjectTimeFrameBoundary(duration, frameRate);
   std::vector<double> edges{0, duration};
   VideoProjectSnapshot project;
   VideoCompositionSettings composition{2, 2, duration};
   for (size_t i{}; i < layers.size(); ++i) {
      if (!layers[i]) throw std::runtime_error("A video layer was removed.");
      const auto &placement = layers[i]->placement;
      const auto id = placement.id.empty() ? "anonymous-" + std::to_string(i) : placement.id;
      project.tracks.push_back({id, 0, *layers[i], placement.parentId});
      composition.layers.push_back({id, placement.rectangle});
   }
   RequireUsableVideoProject(project);
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
      const auto picture = EvaluateVideoTimeline(project, composition, time);
      size_t firstVisible{};
      for (size_t j{}; j < picture.layers.size(); ++j)
         if (picture.layers[j].opacity >= 1 && picture.layers[j].rectangle == VideoFrameRectangle{} &&
             !(picture.layers[j].generated && VideoTimeline::VideoContentUsesAlpha(*picture.layers[j].generated)))
            firstVisible = j; // Opaque full-canvas roots conceal lower families.
      for (size_t j = firstVisible; j < picture.layers.size(); ++j) {
         const auto &sample = picture.layers[j];
         if (sample.opacity <= 0) continue;
         if (span.layers.size() >= 16) throw std::runtime_error("At most sixteen visible video layers can overlap at once.");
         VideoBlendLayer layer{sample.source, sample.sourceTime -
            (time - static_cast<double>(first) / frameRate) * sample.playbackRate,
            sample.playbackRate, sample.crop, sample.opacityRamps};
         layer.rectangle = sample.rectangle; layer.generated = sample.generated;
          if (sample.generated) layer.transparent = VideoTimeline::VideoContentUsesAlpha(*sample.generated);
         span.layers.push_back(std::move(layer));
         if (sample.opacity < 1 || !(sample.rectangle == VideoFrameRectangle{})) span.requiresBlend = true;
      }
      span.requiresBlend = span.requiresBlend || span.layers.size() > 1;
      result.push_back(std::move(span));
   }
   return result;
}

std::string VideoTransitionFilterGraph(const VideoBlendSpan &span, int width, int height, double rate,
   bool preserveAlpha)
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
   graph << "color=c=" << (preserveAlpha ? "black@0" : "black") << ":s=" << width << 'x' << height << ":r=" << rate << ":d=" << span.frameCount / rate
      << (preserveAlpha ? ",format=rgba" : "")
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
      const auto rectangle = CalculateVideoPlacementRectangle(layer.rectangle, width, height);
      graph << ",scale=" << rectangle.width << ':' << rectangle.height;
      if (!layer.fillsCanvas) graph << ":force_original_aspect_ratio=decrease:force_divisible_by=2";
      graph << ",setsar=1,format=rgba";
      if (!layer.fillsCanvas) graph << ",pad=" << rectangle.width << ':' << rectangle.height
         << ":(ow-iw)/2:(oh-ih)/2:color=" <<
            (layer.rectangle == VideoFrameRectangle{} && !layer.transparent ? "black" : "black@0");
      for (const auto &ramp : layer.ramps)
         graph << ",fade=t=" << (ramp.increasing ? "in" : "out") << ":st=" << ramp.start + offset
            << ":d=" << ramp.end - ramp.start << ":alpha=1";
      graph << ",setpts=PTS-" << from + offset << "/TB[l" << i << "];\n";
      graph << (i ? "[b" + std::to_string(i - 1) + "]" : "[base]") << "[l" << i
         << "]overlay=x=" << rectangle.x << ":y=" << rectangle.y << ":shortest=1:format=auto[b" << i << "];\n";
   }
   graph << (span.layers.empty() ? "[base]" : "[b" + std::to_string(span.layers.size() - 1) + "]")
      << "trim=end_frame=" << span.frameCount << ",settb=expr=1/(" << rate << "),setpts=N,format="
      << (preserveAlpha ? "argb" : "yuv420p") << "[vout]\n";
   return graph.str();
}
}
