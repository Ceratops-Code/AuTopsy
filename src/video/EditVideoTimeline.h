// Feature-owned video timing. The model has no Audacity, wxWidgets or mpv types.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

class AudacityProject;

namespace VideoTimeline {
struct Clip {
   uint64_t id{};
   double start{}, sourceStart{}, duration{};
   double End() const { return start + duration; }
};

struct Timeline {
   std::string sourcePath;
   double sourceDuration{}, frameRate{30.0};
   uint64_t nextId{1};
   std::vector<Clip> clips;
   double End() const { return clips.empty() ? 0.0 : clips.back().End(); }
};

// A malformed saved timeline must never reach a decoder or an export graph.
inline void RequireWellFormedTimeline(const Timeline &timeline)
{
   constexpr double tolerance = 1e-7;
   if (timeline.sourcePath.empty()) {
      if (!timeline.clips.empty() || timeline.sourceDuration != 0)
         throw std::runtime_error("Video clips require a source file.");
      return;
   }
   if (!std::isfinite(timeline.sourceDuration) || timeline.sourceDuration <= 0 ||
       !std::isfinite(timeline.frameRate) || timeline.frameRate <= 0 ||
       timeline.frameRate > 1000 || timeline.clips.size() > 10000 || !timeline.nextId)
      throw std::runtime_error("The video timeline has invalid source information.");
   std::unordered_set<uint64_t> ids;
   double previousEnd{};
   for (const auto &clip : timeline.clips) {
      if (!clip.id || clip.id >= timeline.nextId || !ids.insert(clip.id).second ||
          !std::isfinite(clip.start) || !std::isfinite(clip.sourceStart) ||
          !std::isfinite(clip.duration) || clip.start < 0 || clip.sourceStart < 0 ||
          clip.duration <= 0 || clip.End() > 100000000.0 ||
          clip.sourceStart + clip.duration > timeline.sourceDuration + tolerance ||
          clip.start < previousEnd - tolerance)
         throw std::runtime_error("Video clips must have unique IDs and valid, non-overlapping ranges.");
      previousEnd = clip.End();
   }
}

inline Timeline CreateVideoReference(std::string path, double duration, double frameRate)
{
   Timeline timeline{std::move(path), duration, frameRate, 2, {{1, 0, 0, duration}}};
   RequireWellFormedTimeline(timeline);
   return timeline;
}

inline const Clip *ClipAt(const Timeline &timeline, double time)
{
   const auto found = std::find_if(timeline.clips.begin(), timeline.clips.end(),
      [time](const auto &clip) { return clip.start <= time && time < clip.End(); });
   return found == timeline.clips.end() ? nullptr : &*found;
}

struct PicturePosition { bool visible{}; double sourceTime{}; uint64_t clipId{}; };
inline PicturePosition PictureAt(const Timeline &timeline, double projectTime)
{
   if (const auto *clip = ClipAt(timeline, projectTime))
      return {true, clip->sourceStart + projectTime - clip->start, clip->id};
   return {};
}

// Clip IDs survive reordering. A split is snapped to the source frame boundary;
// repeating the same split is a no-op, and failure leaves the input unchanged.
inline bool SplitClipAt(Timeline &timeline, double time)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(time) || time < 0)
      throw std::runtime_error("Choose a non-negative split time.");
   const auto *clip = ClipAt(timeline, time);
   if (!clip) return false;
   const auto offset = std::round((clip->sourceStart + time - clip->start) *
      timeline.frameRate) / timeline.frameRate - clip->sourceStart;
   if (offset <= 1e-7 || offset >= clip->duration - 1e-7) return false;
   Timeline changed = timeline;
   const auto index = static_cast<size_t>(clip - timeline.clips.data());
   auto &left = changed.clips[index];
   const Clip right{changed.nextId++, left.start + offset,
      left.sourceStart + offset, left.duration - offset};
   left.duration = offset;
   changed.clips.insert(changed.clips.begin() + index + 1, right);
   RequireWellFormedTimeline(changed);
   timeline = std::move(changed);
   return true;
}

inline bool MoveClipTo(Timeline &timeline, uint64_t id, double start)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(start) || start < 0)
      throw std::runtime_error("Choose a non-negative clip start time.");
   start = std::round(start * timeline.frameRate) / timeline.frameRate;
   Timeline changed = timeline;
   const auto found = std::find_if(changed.clips.begin(), changed.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == changed.clips.end()) throw std::runtime_error("Unknown video clip ID.");
   if (std::abs(found->start - start) < 1e-7) return false;
   found->start = start;
   std::sort(changed.clips.begin(), changed.clips.end(),
      [](const auto &left, const auto &right) { return left.start < right.start; });
   RequireWellFormedTimeline(changed);
   timeline = std::move(changed);
   return true;
}

inline bool RemoveClip(Timeline &timeline, uint64_t id)
{
   RequireWellFormedTimeline(timeline);
   const auto found = std::find_if(timeline.clips.begin(), timeline.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == timeline.clips.end()) return false;
   timeline.clips.erase(found);
   return true;
}

inline bool KeepsOriginalPicture(const Timeline &timeline)
{
   if (timeline.clips.empty()) return false;
   double end{};
   for (const auto &clip : timeline.clips) {
      if (std::abs(clip.start - end) > 1e-7 ||
          std::abs(clip.sourceStart - clip.start) > 1e-7) return false;
      end = clip.End();
   }
   return std::abs(end - timeline.sourceDuration) < 1e-7;
}

// Native project adapters are implemented in the .cpp, separate from the model.
const Timeline &GetProjectTimeline(const AudacityProject &project);
void AdoptProjectVideo(AudacityProject &project, Timeline timeline, bool importedAudio);
void RemoveProjectVideo(AudacityProject &project);
bool SplitProjectVideo(AudacityProject &project, double at);
bool MoveProjectVideoClip(AudacityProject &project, uint64_t id, double start);
bool RemoveProjectVideoClip(AudacityProject &project, uint64_t id);
void ShowProjectVideoClips(AudacityProject &project);
void ShowProjectVideoPreview(AudacityProject &project, bool raise = false);
}
