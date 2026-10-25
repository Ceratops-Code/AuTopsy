// Feature-owned video timing. The model has no Audacity, wxWidgets or mpv types.
#pragma once
#include "EditVideoContent.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace VideoTimeline {
struct Timeline {
   std::string sourcePath;
   double sourceDuration{}, frameRate{30.0};
   uint64_t nextId{1};
   std::vector<Clip> clips;
   int sourceWidth{}, sourceHeight{};
   // Export snapshots also protect source files from hidden video tracks.
   std::vector<std::string> protectedSourcePaths;
   double End() const { return clips.empty() ? 0.0 : clips.back().End(); }
};

inline MediaSource SourceFor(const Timeline &timeline, const Clip &clip)
{
   const Clip *leaf = &clip;
   for (size_t depth{}; leaf->content && depth < 17; ++depth) {
      if (leaf->content->parts.empty()) break;
      leaf = &leaf->content->parts.front();
   }
   return leaf->media.path.empty()
      ? MediaSource{timeline.sourcePath, timeline.sourceDuration, timeline.frameRate,
         timeline.sourceWidth, timeline.sourceHeight} : leaf->media;
}

inline void RequireUsableVideoSource(const MediaSource &source)
{
   if (source.path.empty() || !std::isfinite(source.duration) || source.duration <= 0 ||
       !std::isfinite(source.frameRate) || source.frameRate <= 0 || source.frameRate > 1000 ||
       source.width < 0 || source.height < 0 || source.width > 65536 || source.height > 65536)
      throw std::runtime_error("Video clips require a source file with usable duration and frame rate.");
}

// A malformed saved timeline must never reach a decoder or an export graph.
inline void RequireWellFormedTimeline(const Timeline &timeline,
   size_t maximumClips = MaximumVideoTrackClips)
{
   constexpr double tolerance = 1e-7;
   if (!timeline.sourcePath.empty())
      RequireUsableVideoSource({timeline.sourcePath, timeline.sourceDuration, timeline.frameRate,
         timeline.sourceWidth, timeline.sourceHeight});
   if (timeline.clips.size() > maximumClips || !timeline.nextId)
      throw std::runtime_error("The video timeline has invalid clip information.");
   std::unordered_set<uint64_t> ids;
   double previousEnd{};
   for (const auto &clip : timeline.clips) {
      const auto source = SourceFor(timeline, clip);
      RequireUsableVideoContent(clip, source);
      if (!clip.linkGroup.empty() && (clip.linkGroup.size() != 32 ||
          clip.linkGroup.find_first_not_of("0123456789abcdef") != std::string::npos))
         throw std::runtime_error("A video clip has an invalid saved link.");
      if (!clip.id || clip.id >= timeline.nextId || !ids.insert(clip.id).second ||
          !std::isfinite(clip.start) || !std::isfinite(clip.sourceStart) ||
          !std::isfinite(clip.duration) || clip.start < 0 || clip.sourceStart < 0 ||
          clip.duration <= 0 || clip.End() > 100000000.0 ||
          clip.ContentTimeAtProjectTime(clip.End()) > VideoContentDuration(clip, source) + tolerance ||
          clip.start < previousEnd - tolerance)
         throw std::runtime_error("Video clips must have unique IDs and valid, non-overlapping ranges.");
      previousEnd = clip.End();
   }
}

inline Timeline CreateVideoTimeline(std::string path, double duration, double frameRate, int width = 0, int height = 0)
{
   Timeline timeline{std::move(path), duration, frameRate, 2, {{1, 0, 0, duration}}};
   timeline.sourceWidth = width; timeline.sourceHeight = height;
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
   if (const auto *clip = ClipAt(timeline, projectTime)) {
      const auto picture = SampleVideoContent(*clip, SourceFor(timeline, *clip),
         clip->ContentTimeAtProjectTime(projectTime));
      if (picture) return {true, picture.sourceTime, clip->id};
   }
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
   const auto rate = VideoContentFrameRate(*clip, SourceFor(timeline, *clip));
   const auto offset = (std::round(clip->ContentTimeAtProjectTime(time) * rate) / rate - clip->sourceStart) / clip->playbackRate;
   if (offset <= 1e-7 || offset >= clip->duration - 1e-7) return false;
   Timeline changed = timeline;
   const auto index = static_cast<size_t>(clip - timeline.clips.data());
   auto &left = changed.clips[index];
   auto right = left;
   right.id = changed.nextId++; right.start += offset;
   right.sourceStart += offset * left.playbackRate; right.duration -= offset;
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
   Timeline changed = timeline;
   const auto found = std::find_if(changed.clips.begin(), changed.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == changed.clips.end()) throw std::runtime_error("Unknown video clip ID.");
   const auto rate = VideoContentFrameRate(*found, SourceFor(timeline, *found)) * found->playbackRate;
   start = std::round(start * rate) / rate;
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

inline bool ChangeVideoClipMute(Timeline &timeline, uint64_t id, bool muted)
{
   RequireWellFormedTimeline(timeline);
   auto changed = timeline;
   const auto found = std::find_if(changed.clips.begin(), changed.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == changed.clips.end()) throw std::runtime_error("Unknown video clip ID.");
   if (found->muted == muted) return false;
   found->muted = muted;
   timeline = std::move(changed);
   return true;
}

inline bool ChangeVideoClipCrop(Timeline &timeline, uint64_t id, VideoEditing::VideoFrameCrop crop)
{
   RequireWellFormedTimeline(timeline);
   VideoEditing::RequireUsableVideoFrameCrop(crop);
   auto changed = timeline;
   const auto found = std::find_if(changed.clips.begin(), changed.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == changed.clips.end()) throw std::runtime_error("Unknown video clip ID.");
   if (found->crop == crop) return false;
   found->crop = crop;
   RequireWellFormedTimeline(changed);
   timeline = std::move(changed);
   return true;
}

inline bool ChangeVideoClipSpeed(Timeline &timeline, uint64_t id, double playbackRate)
{
   RequireWellFormedTimeline(timeline);
   auto changed = timeline;
   const auto found = std::find_if(changed.clips.begin(), changed.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == changed.clips.end()) throw std::runtime_error("Unknown video clip ID.");
   const auto change = PrepareVideoClipRetiming(*found, playbackRate);
   if (std::abs(change.timeScale - 1) < 1e-12) return false;
   *found = RetimeVideoClip(*found, SourceFor(changed, *found), change);
   RequireWellFormedTimeline(changed);
   timeline = std::move(changed);
   return true;
}

inline bool KeepsOriginalPicture(const Timeline &timeline)
{
   if (timeline.clips.empty()) return false;
   double end{};
   for (const auto &clip : timeline.clips) {
      const auto source = SourceFor(timeline, clip);
      if (clip.muted || clip.content || !(clip.crop == VideoEditing::VideoFrameCrop{}) ||
          clip.playbackRate != 1 || source.path != timeline.sourcePath ||
          std::abs(source.duration - timeline.sourceDuration) > 1e-7 ||
          std::abs(clip.start - end) > 1e-7 ||
          std::abs(clip.sourceStart - clip.start) > 1e-7) return false;
      end = clip.End();
   }
   return std::abs(end - timeline.sourceDuration) < 1e-7;
}

// All editing builds a candidate first. Overlaps, out-of-source trims and
// interrupted UI gestures cannot publish a partially changed track.
inline Timeline CopyVideoRange(const Timeline &timeline, double from, double to)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to < from)
      throw std::runtime_error("Choose an ordered, non-negative video range.");
   Timeline result;
   result.nextId = timeline.nextId;
   for (const auto &clip : timeline.clips) {
      const auto begin = std::max(from, clip.start), end = std::min(to, clip.End());
      if (end <= begin + 1e-7) continue;
      auto copy = clip;
      copy.sourceStart = clip.ContentTimeAtProjectTime(begin); copy.start = begin - from; copy.duration = end - begin;
      copy.media = SourceFor(timeline, clip);
      result.clips.push_back(std::move(copy));
   }
   RequireWellFormedTimeline(result);
   return result;
}

inline void EraseVideoRange(Timeline &timeline, double from, double to, bool closeGap)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to < from)
      throw std::runtime_error("Choose an ordered, non-negative video range.");
   if (to - from <= 1e-7) return;
   Timeline changed = timeline; changed.clips.clear();
   const auto addPiece = [&](const Clip &original, double begin, double end, bool newId) {
      if (end <= begin + 1e-7) return;
      auto piece = original;
      piece.id = newId ? changed.nextId++ : original.id;
      piece.start = begin - (closeGap && begin >= to ? to - from : 0);
      piece.sourceStart = original.ContentTimeAtProjectTime(begin); piece.duration = end - begin;
      changed.clips.push_back(std::move(piece));
   };
   for (const auto &clip : timeline.clips) {
      if (clip.End() <= from || clip.start >= to)
         addPiece(clip, clip.start, clip.End(), false);
      else {
         const bool left = clip.start < from;
         addPiece(clip, clip.start, std::min(from, clip.End()), false);
         addPiece(clip, std::max(to, clip.start), clip.End(), left);
      }
   }
   RequireWellFormedTimeline(changed);
   timeline = std::move(changed);
}

inline void InsertVideoGap(Timeline &timeline, double at, double duration)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(at) || !std::isfinite(duration) || at < 0 || duration < 0)
      throw std::runtime_error("Choose a non-negative insertion time and duration.");
   if (duration <= 1e-7) return;
   auto changed = timeline;
   // Insertion is at the exact selection boundary, including subframe audio
   // selections. Picture sampling still follows each source's frame clock.
   const auto *hit = ClipAt(changed, at);
   if (hit && at > hit->start + 1e-7) {
      const auto index = size_t(hit - changed.clips.data());
      const auto original = changed.clips[index];
      auto right = original;
      right.id = changed.nextId++; right.start = at;
      right.sourceStart = original.ContentTimeAtProjectTime(at); right.duration = original.End() - at;
      changed.clips[index].duration = at - original.start;
      changed.clips.insert(changed.clips.begin() + index + 1, right);
   }
   for (auto &clip : changed.clips) if (clip.start >= at - 1e-7) clip.start += duration;
   RequireWellFormedTimeline(changed); timeline = std::move(changed);
}

inline void PasteVideoRange(Timeline &timeline, double at, const Timeline &copied, double duration)
{
   RequireWellFormedTimeline(timeline); RequireWellFormedTimeline(copied);
   if (!std::isfinite(duration) || duration < copied.End() - 1e-7)
      throw std::runtime_error("The copied video range does not fit its declared duration.");
   auto changed = timeline;
   InsertVideoGap(changed, at, duration);
   for (const auto &clip : copied.clips) {
      auto copy = clip; copy.id = changed.nextId++; copy.start += at;
      copy.media = SourceFor(copied, clip); changed.clips.push_back(std::move(copy));
   }
   std::sort(changed.clips.begin(), changed.clips.end(),
      [](const auto &a, const auto &b) { return a.start < b.start; });
   RequireWellFormedTimeline(changed); timeline = std::move(changed);
}

inline bool TrimVideoClip(Timeline &timeline, uint64_t id, double start, double end)
{
   RequireWellFormedTimeline(timeline);
   auto changed = timeline;
   const auto found = std::find_if(changed.clips.begin(), changed.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == changed.clips.end()) throw std::runtime_error("Unknown video clip ID.");
   const auto rate = VideoContentFrameRate(*found, SourceFor(changed, *found));
   if (!std::isfinite(start) || !std::isfinite(end))
      throw std::runtime_error("Video trim times must be finite.");
   const auto sourceStart = std::round(found->ContentTimeAtProjectTime(start) * rate) / rate;
   const auto sourceEnd = std::round(found->ContentTimeAtProjectTime(end) * rate) / rate;
   start = found->ProjectTimeAtContentTime(sourceStart);
   end = found->ProjectTimeAtContentTime(sourceEnd);
   if (sourceEnd - sourceStart < 0.5 / rate)
      throw std::runtime_error("Keep at least one video frame.");
   if (std::abs(start - found->start) < 1e-7 && std::abs(end - found->End()) < 1e-7) return false;
   found->start = start; found->sourceStart = sourceStart; found->duration = end - start;
   RequireWellFormedTimeline(changed); timeline = std::move(changed);
   return true;
}

// Pointer gestures stop at available source frames and neighboring clips.
// The strict range-edit command still reports invalid requested bounds.
inline bool DragVideoClipEdge(Timeline &timeline, uint64_t id, bool left, double at)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(at)) throw std::runtime_error("Choose a finite video edge time.");
   const auto found = std::find_if(timeline.clips.begin(), timeline.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == timeline.clips.end()) throw std::runtime_error("The video clip was removed.");
   const auto source = SourceFor(timeline, *found);
   const auto rate = VideoContentFrameRate(*found, source) * found->playbackRate;
   const auto origin = found->ProjectTimeAtContentTime(0);
   auto minimum = left ? std::max(0.0, origin) : found->start + 1 / rate;
   auto maximum = left ? found->End() - 1 / rate : found->ProjectTimeAtContentTime(VideoContentDuration(*found, source));
   for (const auto &other : timeline.clips) if (other.id != id) {
      if (left && other.End() <= found->start + 1e-7) minimum = std::max(minimum, other.End());
      if (!left && other.start >= found->End() - 1e-7) maximum = std::min(maximum, other.start);
   }
   // Round constraints inward; rounding the pointer alone can cross a
   // non-frame-aligned neighbor by half a frame.
   minimum = origin + std::ceil((minimum - origin) * rate - 1e-7) / rate;
   maximum = origin + std::floor((maximum - origin) * rate + 1e-7) / rate;
   if (minimum > maximum + 1e-7) return false;
   at = std::clamp(at, minimum, maximum);
   return TrimVideoClip(timeline, id, left ? at : found->start, left ? found->End() : at);
}

struct VideoTrimEdge { uint64_t id{}; bool left{}; };

// Adjacent clips share one visible seam. Dragging it into either clip trims
// that side rather than trying to extend its neighbor into occupied space.
// Once a gap exists, the chosen edge keeps its ordinary trim/untrim behavior.
inline VideoTrimEdge ChooseVideoTrimEdge(const Timeline &timeline, uint64_t id, bool left, double motion)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(motion)) throw std::runtime_error("Choose a finite video edge motion.");
   const auto found = std::find_if(timeline.clips.begin(), timeline.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == timeline.clips.end()) throw std::runtime_error("The video clip was removed.");
   if ((left && motion < 0) || (!left && motion > 0))
      for (const auto &other : timeline.clips) if (other.id != id &&
            std::abs(left ? other.End() - found->start : other.start - found->End()) < 1e-7)
         return {other.id, !left};
   return {id, left};
}

// A borrowed evaluation result is valid only while its input timelines are
// unchanged. It is used immediately by preview/export adapters, never persisted
// or retained across native edits. Ordering and gap selection live here so the
// two rendering paths cannot acquire different picture precedence rules.
// The owning track checks ranges when publishing its state; this query borrows
// those values without a per-frame rebuild of the timeline.
struct VideoLayerVisibility { bool muted{}, solo{}; };
inline bool IsVideoLayerVisible(const VideoLayerVisibility &visibility, bool anySolo)
{ return !visibility.muted && (!anySolo || visibility.solo); }
struct VideoTimelineLayer {
   const Timeline *timeline{};
   VideoLayerVisibility visibility;
};
// Select once at the native boundary. Empty/gapped solo tracks still suppress
// other picture layers; mute overrides solo. Audio mixing is outside this model.
inline std::vector<const Timeline*> VisibleVideoLayers(const std::vector<VideoTimelineLayer> &layers)
{
   const bool anySolo = std::any_of(layers.begin(), layers.end(),
      [](const auto &layer) { return layer.visibility.solo; });
   std::vector<const Timeline*> result;
   for (const auto &layer : layers) {
      if (!layer.timeline) throw std::runtime_error("A video layer requires a timeline.");
      if (IsVideoLayerVisible(layer.visibility, anySolo)) result.push_back(layer.timeline);
   }
   return result;
}

struct EvaluatedVideoPicture {
   const Timeline *timeline{};
   const Clip *clip{};
   double sourceTime{};
   MediaSource source;
   double playbackRate{1};
   VideoEditing::VideoFrameCrop crop;
};

inline EvaluatedVideoPicture EvaluateVideoLayers(
   const std::vector<const Timeline*> &layers, double projectTime)
{
   for (const auto *layer : layers)
      if (!layer) throw std::runtime_error("A video layer requires a timeline.");
   for (const auto *layer : layers)
      if (const auto *clip = ClipAt(*layer, projectTime)) {
         const auto sample = SampleVideoContent(*clip, SourceFor(*layer, *clip),
            clip->ContentTimeAtProjectTime(projectTime));
         if (sample) return {layer, clip, sample.sourceTime, sample.source, sample.playbackRate, sample.crop};
      }
   return {};
}

// Upper tracks take visual precedence. Flatten only at preview/export boundaries;
// native tracks and their clips remain independent, editable project objects.
inline constexpr size_t MaximumVideoRenderSegments = 100000;
inline Timeline ComposeVideoTracks(const std::vector<const Timeline*> &layers)
{
   std::vector<double> edges;
   for (const auto *layer : layers) {
      if (!layer) throw std::runtime_error("A video layer requires a timeline.");
      RequireWellFormedTimeline(*layer);
      for (const auto &clip : layer->clips) {
         const auto boundaries = VideoContentEdges(clip);
         if (boundaries.size() > 2 * MaximumVideoRenderSegments - edges.size())
            throw std::runtime_error("The video composition exceeds its frame-boundary budget.");
         edges.insert(edges.end(), boundaries.begin(), boundaries.end());
      }
   }
   std::sort(edges.begin(), edges.end());
   edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
   Timeline result;
   for (size_t i = 1; i < edges.size(); ++i) {
      if (edges[i] - edges[i-1] <= 1e-7) continue;
      const auto picture = EvaluateVideoLayers(layers, (edges[i]+edges[i-1])/2);
      if (!picture.clip) continue;
       if (result.clips.size() >= MaximumVideoRenderSegments)
          throw std::runtime_error("The video composition exceeds its segment budget.");
      Clip part{result.nextId++, edges[i-1],
          std::max(0.0, picture.sourceTime - (edges[i] - edges[i-1]) * picture.playbackRate / 2), edges[i] - edges[i-1]};
      part.media = picture.source;
      part.playbackRate = picture.playbackRate;
      part.crop = picture.crop;
      if (result.sourcePath.empty()) {
         result.sourcePath = part.media.path; result.sourceDuration = part.media.duration;
         result.frameRate = part.media.frameRate; result.sourceWidth = part.media.width; result.sourceHeight = part.media.height;
      }
      result.clips.push_back(std::move(part));
   }
   // Flattened render segments are not editable native clips. Several valid
   // joined roots may produce more segments than one track permits as roots.
   RequireWellFormedTimeline(result, MaximumVideoRenderSegments);
   return result;
}

struct VideoClipExportPlan {
   Timeline picture;
   double projectStart{}, duration{};
};
// Export picture begins at zero, while its soundtrack is mixed from the
// original project-time window. Retained trims/compounds are flattened once.
inline VideoClipExportPlan PlanVideoClipExport(const Timeline& timeline, uint64_t id)
{
   RequireWellFormedTimeline(timeline);
   const auto found = std::find_if(timeline.clips.begin(), timeline.clips.end(),
      [id](const auto& clip) { return clip.id == id; });
   if (found == timeline.clips.end()) throw std::runtime_error("The video clip was removed.");
   auto oneClip = timeline;
   oneClip.clips = {*found};
   const auto range = CopyVideoRange(oneClip, found->start, found->End());
   VideoClipExportPlan result{ComposeVideoTracks({&range}), found->start, found->duration};
   // Retain the envelope tree until the shared picture renderer has evaluated
   // it; flattening an opaque cut list would discard fades on joined content.
   if (VideoEditing::HasVideoOpacity(*found) && !found->muted) result.picture = range;
   // A hidden occurrence still defines its export canvas and source identity.
   // Keep no render segments: black frames use the explicit clip duration and
   // independently mixed project audio, without reopening the muted source.
   if (result.picture.sourcePath.empty()) {
      const auto source = SourceFor(timeline, *found);
      result.picture.sourcePath = source.path;
      result.picture.sourceDuration = source.duration;
      result.picture.frameRate = source.frameRate;
      result.picture.sourceWidth = source.width;
      result.picture.sourceHeight = source.height;
   }
   result.picture.protectedSourcePaths = timeline.protectedSourcePaths;
   for (const auto& clip : timeline.clips)
      for (const auto& source : VideoContentSources(clip, SourceFor(timeline, clip)))
         result.picture.protectedSourcePaths.push_back(source.path);
   return result;
}

}
