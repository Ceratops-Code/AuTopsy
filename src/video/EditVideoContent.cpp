#include "EditVideoContent.h"
#include "EditVideoTimeline.h"
#include <set>

namespace VideoTimeline {
namespace {
MediaSource Source(const Clip &clip, const MediaSource &fallback)
{ return clip.media.path.empty() ? fallback : clip.media; }
bool SameSource(const MediaSource &a, const MediaSource &b)
{ return a.path == b.path && a.duration == b.duration && a.frameRate == b.frameRate &&
   a.width == b.width && a.height == b.height; }
void Check(const Clip &clip, const MediaSource &fallback, size_t depth, size_t &count, double parentRate = 1)
{
   if (depth > MaximumVideoContentDepth || ++count > MaximumVideoContentClips)
      throw std::runtime_error("Joined video exceeds the supported nesting or piece limit.");
   const auto duration = VideoContentDuration(clip, fallback);
   if (!std::isfinite(clip.start) || !std::isfinite(clip.sourceStart) ||
       !std::isfinite(clip.duration) || clip.start < 0 || clip.sourceStart < 0 || clip.duration <= 0 ||
       !std::isfinite(clip.playbackRate) || clip.playbackRate < 0.01 || clip.playbackRate > 100 ||
       parentRate * clip.playbackRate < 0.01 || parentRate * clip.playbackRate > 100 ||
       !std::isfinite(duration) || duration <= 0 || duration > 100000000 ||
       clip.ContentTimeAtProjectTime(clip.End()) > duration + 1e-7)
      throw std::runtime_error("A video content window is outside its retained footage.");
   ClipMarkers::RequireUsableMarkers(clip.markers);
   if (!clip.linkGroup.empty() && (clip.linkGroup.size() != 32 ||
       clip.linkGroup.find_first_not_of("0123456789abcdef") != std::string::npos))
      throw std::runtime_error("A video clip has an invalid saved link.");
   if (!clip.content) { RequireUsableVideoSource(Source(clip, fallback)); return; }
   const auto &content = *clip.content;
   if (content.parts.empty() || !std::isfinite(content.frameRate) || content.frameRate <= 0 ||
       content.frameRate > 1000)
      throw std::runtime_error("Joined video requires pieces and a usable editing frame rate.");
   std::set<uint64_t> ids;
   double end{};
   for (const auto &part : content.parts) {
      if (!part.id || !ids.insert(part.id).second || part.start < end - 1e-7 ||
          part.End() > duration + 1e-7 || (!part.content && part.media.path.empty()))
         throw std::runtime_error("Joined video pieces need unique IDs, owned media and ordered ranges.");
      Check(part, {}, depth + 1, count, parentRate * clip.playbackRate); end = part.End();
   }
}
void Edges(const Clip &clip, double origin, double scale, double from, double to, std::vector<double> &out)
{
   const auto begin = std::max(from, origin + clip.start * scale);
   const auto end = std::min(to, origin + clip.End() * scale);
   if (end <= begin + 1e-7) return;
   out.push_back(begin); out.push_back(end);
   if (clip.content)
      for (const auto &part : clip.content->parts)
         Edges(part, origin + clip.start * scale - clip.sourceStart * scale / clip.playbackRate,
            scale / clip.playbackRate, begin, end, out);
}
void Sources(const Clip &clip, const MediaSource &fallback, std::vector<MediaSource> &out)
{
   if (clip.content) for (const auto &part : clip.content->parts) Sources(part, {}, out);
   else {
      const auto source = Source(clip, fallback);
      if (std::none_of(out.begin(), out.end(), [&](const auto &item) { return SameSource(item, source); }))
         out.push_back(source);
   }
}
void Groups(const Clip &clip, std::set<std::string> &out)
{
   if (!clip.linkGroup.empty()) out.insert(clip.linkGroup);
   if (clip.content) for (const auto &part : clip.content->parts) Groups(part, out);
}
void Windows(const Clip &clip, double origin, double scale, double from, double to, std::vector<VideoLinkWindow> &out)
{
   const auto begin = origin + clip.start * scale, end = origin + clip.End() * scale;
   const auto left = std::clamp(from, begin, end), right = std::clamp(to, begin, end);
   std::set<std::string> childGroups;
   if (clip.content) for (const auto &part : clip.content->parts) Groups(part, childGroups);
   // A link assigned after joining belongs to the whole content timeline.
   // Join's ordinary parent alias must not duplicate a child's soundtrack.
   if (!clip.linkGroup.empty() && !childGroups.count(clip.linkGroup)) {
      const auto duration = clip.content ? clip.content->duration : clip.media.duration;
      out.push_back({clip.linkGroup, left, right, begin - clip.sourceStart * scale / clip.playbackRate,
         begin + (duration - clip.sourceStart) * scale / clip.playbackRate,
         clip.playbackRate / scale, clip.content != nullptr});
   }
   if (clip.content) {
      for (const auto &part : clip.content->parts)
         Windows(part, begin - clip.sourceStart * scale / clip.playbackRate,
            scale / clip.playbackRate, left, right, out);
   }
}
}
double VideoContentDuration(const Clip &clip, const MediaSource &fallback)
{ return clip.content ? clip.content->duration : Source(clip, fallback).duration; }
double VideoContentFrameRate(const Clip &clip, const MediaSource &fallback)
{ return clip.content ? clip.content->frameRate : Source(clip, fallback).frameRate; }
void RequireUsableVideoContent(const Clip &clip, const MediaSource &fallback)
{ size_t count{}; Check(clip, fallback, 0, count); }
VideoContentSample SampleVideoContent(const Clip &clip, const MediaSource &fallback, double time)
{
   if (!std::isfinite(time) || time < 0 || time >= VideoContentDuration(clip, fallback)) return {};
   const Clip *current = &clip;
   double playbackRate = clip.playbackRate;
   for (size_t depth{}; depth <= MaximumVideoContentDepth; ++depth) {
      if (!current->content) return {current, Source(*current, current == &clip ? fallback : MediaSource{}), time, playbackRate};
      const auto &parts = current->content->parts;
      const auto found = std::upper_bound(parts.begin(), parts.end(), time,
         [](double t, const Clip &part) { return t < part.start; });
      if (found == parts.begin()) return {};
      current = &*std::prev(found);
      if (time >= current->End()) return {};
      time = current->ContentTimeAtProjectTime(time);
      playbackRate *= current->playbackRate;
   }
   throw std::runtime_error("Joined video exceeds the supported nesting limit.");
}
std::vector<double> VideoContentEdges(const Clip &clip)
{ std::vector<double> out; Edges(clip, 0, 1, clip.start, clip.End(), out); return out; }
std::vector<MediaSource> VideoContentSources(const Clip &clip, const MediaSource &fallback)
{ std::vector<MediaSource> out; Sources(clip, fallback, out); return out; }
std::vector<std::string> VideoContentLinkGroups(const Clip &clip)
{ std::set<std::string> out; Groups(clip, out); return {out.begin(), out.end()}; }
std::vector<VideoLinkWindow> VideoContentLinkWindows(const Clip &clip)
{ std::vector<VideoLinkWindow> out; Windows(clip, 0, 1, clip.start, clip.End(), out); return out; }

VideoClipRetiming PrepareVideoClipRetiming(const Clip &anchor, double playbackRate)
{
   if (!std::isfinite(playbackRate) || playbackRate < 0.01 || playbackRate > 100 ||
       !std::isfinite(anchor.playbackRate) || anchor.playbackRate < 0.01 || anchor.playbackRate > 100 ||
       !std::isfinite(anchor.start) || anchor.start < 0)
      throw std::runtime_error("Video speed must be between 1% and 10000%.");
   return {anchor.start, anchor.playbackRate / playbackRate};
}
Clip RetimeVideoClip(const Clip &clip, const MediaSource &fallback, const VideoClipRetiming &change)
{
   RequireUsableVideoContent(clip, fallback);
   if (!std::isfinite(change.anchor) || change.anchor < 0 ||
       !std::isfinite(change.timeScale) || change.timeScale <= 0)
      throw std::runtime_error("The video speed change has no usable timing map.");
   auto result = clip;
   result.start = change.ProjectTimeAfterRetiming(clip.start);
   result.duration *= change.timeScale;
   result.playbackRate /= change.timeScale;
   RequireUsableVideoContent(result, fallback);
   return result;
}
void RemapVideoContentLinks(Clip &clip, const std::vector<std::pair<std::string, std::string>> &groups)
{
   const auto found = std::find_if(groups.begin(), groups.end(),
      [&](const auto &entry) { return entry.first == clip.linkGroup; });
   if (found != groups.end()) clip.linkGroup = found->second;
   if (clip.content) {
      auto content = std::make_shared<VideoClipContent>(*clip.content);
      for (auto &part : content->parts) RemapVideoContentLinks(part, groups);
      clip.content = std::move(content);
   }
}
bool SameVideoContent(const Clip &a, const Clip &b)
{
   if (a.playbackRate != b.playbackRate || (!a.content && !b.content && !SameSource(a.media, b.media)) ||
       a.markers.size() != b.markers.size()) return false;
   for (size_t i{}; i < a.markers.size(); ++i)
      if (a.markers[i].id != b.markers[i].id || a.markers[i].position != b.markers[i].position ||
          a.markers[i].text != b.markers[i].text) return false;
   if (a.content == b.content) return true;
   if (!a.content || !b.content || a.content->duration != b.content->duration ||
       a.content->frameRate != b.content->frameRate || a.content->parts.size() != b.content->parts.size()) return false;
   for (size_t i{}; i < a.content->parts.size(); ++i) {
      const auto &x = a.content->parts[i], &y = b.content->parts[i];
      if (x.id != y.id || x.start != y.start || x.sourceStart != y.sourceStart ||
          x.duration != y.duration || x.linkGroup != y.linkGroup || !SameVideoContent(x, y)) return false;
   }
   return true;
}
bool ReplaceVideoContentSource(Clip &clip, const MediaSource &fallback,
   const std::string &oldPath, const MediaSource &replacement)
{
   if (!clip.content) {
      if (Source(clip, fallback).path != oldPath) return false;
      clip.media = replacement; return true;
   }
   auto changed = std::make_shared<VideoClipContent>(*clip.content);
   bool found{};
   for (auto &part : changed->parts) found |= ReplaceVideoContentSource(part, {}, oldPath, replacement);
   if (found) clip.content = std::move(changed);
   return found;
}
}
