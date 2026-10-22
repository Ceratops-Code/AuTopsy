#include "EvaluateVideoTimeline.h"
#include <functional>
#include <unordered_map>

namespace VideoEditing {
namespace {
void RequireCrop(const VideoFrameCrop &crop)
{
   for (const auto value : {crop.left, crop.top, crop.right, crop.bottom})
      if (!std::isfinite(value) || value < 0 || value >= 1)
         throw std::runtime_error("Video crop margins must be finite fractions from zero to less than one.");
   if (crop.left + crop.right >= 1 || crop.top + crop.bottom >= 1)
      throw std::runtime_error("Video crop must retain a nonempty source rectangle.");
}
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
      std::unordered_set<uint64_t> clips;
      for (const auto &appearance : style.clips) {
         const auto &timeline = project.tracks[index->second].timeline;
         if (!clips.insert(appearance.clipId).second ||
             std::none_of(timeline.clips.begin(), timeline.clips.end(),
                [&](const auto &clip) { return clip.id == appearance.clipId; }))
            throw std::runtime_error("Clip picture properties require unique, existing clip identities.");
         RequireCrop(appearance.crop);
      }
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
      if (soloGroups.empty() || soloGroups.count(root)) pending.push_back({root, {}});
   if (time >= settings.duration) return result;
   while (!pending.empty()) {
      const auto item = pending.back(); pending.pop_back();
      const auto &track = project.tracks[item.index];
      const auto &style = styles[item.index];
      const auto *clip = VideoTimeline::ClipAt(track.timeline, time);
      if (style.muted || !clip) continue;
      const auto appearance = std::find_if(style.clips.begin(), style.clips.end(),
         [&](const auto &value) { return value.clipId == clip->id; });
      if (appearance != style.clips.end() && appearance->muted) continue;
      const auto sample = VideoTimeline::SampleVideoContent(*clip, VideoTimeline::SourceFor(track.timeline, *clip),
         clip->sourceStart + time - clip->start);
      if (!sample) continue;
      const auto rectangle = PlaceWithin(item.parent, style.rectangle);
      result.layers.push_back({track.id, clip->id, sample.source,
         sample.sourceTime,
         appearance == style.clips.end() ? VideoFrameCrop{} : appearance->crop, rectangle});
      // Lower roots are processed first. Children go above their own parent,
      // irrespective of where an upstream pane happens to display their rows.
      for (const auto child : children[item.index]) pending.push_back({child, rectangle});
   }
   return result;
}
}
