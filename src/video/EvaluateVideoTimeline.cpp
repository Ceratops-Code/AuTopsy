#include "EvaluateVideoTimeline.h"
#include <functional>
#include <unordered_map>

namespace VideoEditing {
namespace {
void RequireRectangle(const VideoFrameRectangle &rect)
{
   for (const auto value : {rect.x, rect.y, rect.width, rect.height})
      if (!std::isfinite(value)) throw std::runtime_error("Video placement must be finite.");
   if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
       rect.x + rect.width > 1 || rect.y + rect.height > 1)
      throw std::runtime_error("The embedded video rectangle must fit inside its parent.");
}
VideoFrameRectangle PlaceWithin(const VideoFrameRectangle &parent, const VideoFrameRectangle &child)
{
   return {parent.x + child.x * parent.width, parent.y + child.y * parent.height,
      parent.width * child.width, parent.height * child.height};
}
}

VideoFramePlan EvaluateVideoTimeline(const VideoProjectSnapshot &project,
   const VideoCompositionSettings &settings, double time)
{
   RequireUsableVideoProject(project);
   if (settings.canvasWidth <= 0 || settings.canvasHeight <= 0 ||
       settings.canvasWidth > 65536 || settings.canvasHeight > 65536 ||
       !std::isfinite(settings.duration) || settings.duration < 0 ||
       settings.duration > 100000000 || !std::isfinite(time) || time < 0)
      throw std::runtime_error("Composition requires a finite canvas, duration and non-negative time.");
   std::unordered_map<std::string, size_t> indices;
   for (size_t i = 0; i < project.tracks.size(); ++i) indices.emplace(project.tracks[i].id, i);
   std::vector<VideoLayerPresentation> styles(project.tracks.size());
   std::unordered_set<std::string> seen;
   for (const auto &style : settings.layers) {
      const auto index = indices.find(style.trackId);
      if (index == indices.end() || !seen.insert(style.trackId).second)
         throw std::runtime_error("Composition properties require unique, existing track identities.");
      RequireRectangle(style.rectangle);
      styles[index->second] = style;
   }
   VideoFramePlan result{settings.canvasWidth, settings.canvasHeight, settings.duration};
   std::vector<std::vector<size_t>> children(project.tracks.size());
   std::vector<size_t> roots;
   std::unordered_set<size_t> soloGroups;
   for (size_t i = 0; i < project.tracks.size(); ++i) {
      const auto &parent = project.tracks[i].parentTrackId;
      if (parent.empty()) roots.push_back(i);
      else children[indices.at(parent)].push_back(i);
      auto root = i;
      while (!project.tracks[root].parentTrackId.empty())
         root = indices.at(project.tracks[root].parentTrackId);
      if (styles[i].solo) soloGroups.insert(root);
   }
   // A bounded, explicit traversal avoids recursion on user-supplied nesting.
   struct Pending { size_t index; VideoFrameRectangle parent; };
   std::vector<Pending> pending;
   for (const auto root : roots)
      if (VideoTimeline::IsVideoLayerVisible(
            {styles[root].muted, soloGroups.count(root) != 0}, !soloGroups.empty()))
         pending.push_back({root, {}});
   if (time >= settings.duration) return result;
   while (!pending.empty()) {
      const auto item = pending.back(); pending.pop_back();
      const auto &track = project.tracks[item.index];
      const auto &style = styles[item.index];
      const auto *clip = VideoTimeline::ClipAt(track.timeline, time);
      if (style.muted || !clip) continue;
      const auto sample = VideoTimeline::SampleVideoContent(*clip, VideoTimeline::SourceFor(track.timeline, *clip),
         clip->ContentTimeAtProjectTime(time));
      if (!sample) continue;
      const auto rectangle = PlaceWithin(item.parent, style.rectangle);
      result.layers.push_back({track.id, clip->id, sample.source,
         sample.sourceTime,
         sample.crop, rectangle, sample.playbackRate, sample.opacity, sample.opacityRamps});
      // Lower roots are processed first. Children go above their own parent,
      // irrespective of where an upstream pane happens to display their rows.
      for (const auto child : children[item.index]) pending.push_back({child, rectangle});
   }
   return result;
}
}
