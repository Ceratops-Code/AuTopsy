#include "EditVideoContent.h"
#include "EditVideoTimeline.h"
#include "RenderVideoComposition.h"
#include <set>
#include <iomanip>
#include <sstream>
#include <locale>

namespace VideoEditing {
void RequireUsableVideoFrameCrop(const VideoFrameCrop &crop)
{
   for (const auto value : {crop.left, crop.top, crop.right, crop.bottom})
      if (!std::isfinite(value) || value < 0 || value >= 1)
         throw std::runtime_error("Picture crop margins must be finite fractions from zero to less than one.");
   if (crop.left + crop.right >= 1 || crop.top + crop.bottom >= 1)
      throw std::runtime_error("Picture crop must retain a nonempty rectangle.");
}
VideoFrameCrop ComposeVideoFrameCrops(const VideoFrameCrop &inner, const VideoFrameCrop &outer)
{
   RequireUsableVideoFrameCrop(inner); RequireUsableVideoFrameCrop(outer);
   const auto width = 1 - inner.left - inner.right;
   const auto height = 1 - inner.top - inner.bottom;
   VideoFrameCrop result{inner.left + outer.left * width, inner.top + outer.top * height,
      inner.right + outer.right * width, inner.bottom + outer.bottom * height};
   RequireUsableVideoFrameCrop(result);
   return result;
}
double VideoOpacityAt(const VideoOpacityEnvelope &value, double time)
{
   double opacity = 1;
   if (value.inDuration > 0) opacity *= std::clamp((time - value.inStart) / value.inDuration, 0.0, 1.0);
   if (value.outDuration > 0) opacity *= std::clamp((value.outEnd - time) / value.outDuration, 0.0, 1.0);
   return opacity;
}
bool HasVideoOpacity(const VideoTimeline::Clip &clip)
{
   if (clip.opacity.inDuration > 0 || clip.opacity.outDuration > 0 || !clip.opacityModifiers.empty()) return true;
   return clip.content && std::any_of(clip.content->parts.begin(), clip.content->parts.end(),
      [](const auto &part) { return HasVideoOpacity(part); });
}
void RequireUsableVideoOpacityRamps(const std::vector<VideoOpacityRamp> &ramps) {
   if (ramps.size() > MaximumVideoOpacityRamps) throw std::runtime_error("Too many retained picture envelopes.");
   for (const auto &ramp : ramps)
      if (!std::isfinite(ramp.start) || !std::isfinite(ramp.end) || ramp.end <= ramp.start ||
          std::abs(ramp.start) > 1e20 || std::abs(ramp.end) > 1e20)
         throw std::runtime_error("A retained picture envelope has invalid endpoints.");
}
std::string EncodeVideoOpacityRamps(const std::vector<VideoOpacityRamp> &ramps) {
   RequireUsableVideoOpacityRamps(ramps);
   std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17) << ramps.size();
   for (const auto &ramp : ramps) out << ' ' << ramp.start << ' ' << ramp.end << ' ' << int(ramp.increasing);
   return out.str();
}
std::vector<VideoOpacityRamp> DecodeVideoOpacityRamps(const std::string &text) {
   if (text.size() > 16384) throw std::runtime_error("The saved picture envelope exceeds its size bound.");
   std::istringstream input(text); input.imbue(std::locale::classic()); size_t count{};
   if (!(input >> count) || count > MaximumVideoOpacityRamps) throw std::runtime_error("The saved picture envelope is invalid.");
   std::vector<VideoOpacityRamp> result;
   for (size_t i{}; i < count; ++i) {
      VideoOpacityRamp ramp; int increasing{};
      if (!(input >> ramp.start >> ramp.end >> increasing) || increasing < 0 || increasing > 1)
         throw std::runtime_error("The saved picture envelope is incomplete.");
      ramp.increasing = increasing != 0; result.push_back(ramp);
   }
   input >> std::ws;
   if (!input.eof()) throw std::runtime_error("The saved picture envelope has unexpected data.");
   RequireUsableVideoOpacityRamps(result); return result;
}
}

namespace VideoTimeline {
namespace {
MediaSource Source(const Clip &clip, const MediaSource &fallback)
{ return clip.media.path.empty() ? fallback : clip.media; }
bool SameSource(const MediaSource &a, const MediaSource &b)
{ return a.path == b.path && a.duration == b.duration && a.frameRate == b.frameRate &&
   a.width == b.width && a.height == b.height; }
void Check(const Clip &clip, const MediaSource &fallback, size_t depth, size_t &count, double parentRate = 1, VideoEditing::VideoFrameCrop parentCrop = {})
{
   if (depth > MaximumVideoContentDepth || ++count > MaximumVideoContentClips)
      throw std::runtime_error("Joined video exceeds the supported nesting or piece limit.");
   const auto crop = VideoEditing::ComposeVideoFrameCrops(clip.crop, parentCrop);
   const auto duration = VideoContentDuration(clip, fallback);
   if (!std::isfinite(clip.start) || !std::isfinite(clip.sourceStart) ||
       !std::isfinite(clip.duration) || clip.start < 0 || clip.sourceStart < 0 || clip.duration <= 0 ||
       !std::isfinite(clip.playbackRate) || clip.playbackRate < 0.01 || clip.playbackRate > 100 ||
       parentRate * clip.playbackRate < 0.01 || parentRate * clip.playbackRate > 100 ||
       !std::isfinite(duration) || duration <= 0 || duration > 100000000 ||
       clip.ContentTimeAtProjectTime(clip.End()) > duration + 1e-7)
      throw std::runtime_error("A video content window is outside its retained footage.");
   ClipMarkers::RequireUsableMarkers(clip.markers);
   VideoEditing::RequireUsableVideoOpacityRamps(clip.opacityModifiers);
   const auto &opacity = clip.opacity;
   for (const auto value : {opacity.inStart, opacity.inDuration, opacity.outEnd, opacity.outDuration})
      if (!std::isfinite(value) || value < 0) throw std::runtime_error("Picture fades require finite non-negative timing.");
   if (opacity.inStart + opacity.inDuration > duration + 1e-7 || opacity.outEnd > duration + 1e-7 ||
       opacity.outDuration > opacity.outEnd + 1e-7)
      throw std::runtime_error("Picture fades must stay inside retained content.");
   if (!clip.linkGroup.empty() && (clip.linkGroup.size() != 32 ||
       clip.linkGroup.find_first_not_of("0123456789abcdef") != std::string::npos))
      throw std::runtime_error("A video clip has an invalid saved link.");
   if (!clip.content) { RequireUsableVideoSource(Source(clip, fallback)); return; }
   const auto &content = *clip.content;
   using Operation = VideoClipContent::Operation;
   if (!std::isfinite(content.frameRate) || content.frameRate <= 0 || content.frameRate > 1000 ||
       int(content.operation) < 0 || int(content.operation) > int(Operation::Text) ||
       !std::isfinite(content.audioContentOffset) || std::abs(content.audioContentOffset) > 100000000)
      throw std::runtime_error("Video content needs a supported operation and editing frame rate.");
   if (content.operation == Operation::Image || content.operation == Operation::Title || content.operation == Operation::Text) {
      if (!content.parts.empty() || content.width < 2 || content.height < 2 ||
          content.width > 8192 || content.height > 8192 ||
          (content.operation == Operation::Image && (content.imagePath.empty() || content.imagePath.size() > 32768)) ||
          (content.operation != Operation::Image && (content.title.empty() || content.title.size() > 16384)) ||
          (content.operation == Operation::Text && (content.title.find_first_not_of(" \t\r\n") == std::string::npos ||
             content.title.find('\0') != std::string::npos)))
         throw std::runtime_error("A still picture or title requires usable dimensions and bounded content.");
      if (content.operation == Operation::Text) VideoEditing::RequireUsableVideoTextAppearance(content.textAppearance);
      return;
   }
   if (content.parts.empty()) throw std::runtime_error("Video content requires retained source pieces.");
   if (content.operation != Operation::Sequence &&
       (content.parts.size() != 1 || content.parts.front().start != 0 ||
        (content.operation == Operation::Reverse && std::abs(content.parts.front().End() - content.duration) > 1e-7) ||
        (content.operation == Operation::Freeze && (!std::isfinite(content.heldTime) || content.heldTime < 0 ||
           content.heldTime >= content.parts.front().End()))))
      throw std::runtime_error("A picture generator has an invalid retained source window.");
   std::set<uint64_t> ids;
   double end{};
   for (const auto &part : content.parts) {
      if (!part.id || !ids.insert(part.id).second || part.start < end - 1e-7 ||
          (content.operation == Operation::Sequence && part.End() > duration + 1e-7) || (!part.content && part.media.path.empty()))
         throw std::runtime_error("Joined video pieces need unique IDs, owned media and ordered ranges.");
      Check(part, {}, depth + 1, count, parentRate * clip.playbackRate, crop); end = part.End();
   }
}
void Edges(const Clip &clip, double origin, double scale, double from, double to, std::vector<double> &out)
{
   const auto begin = std::max(from, origin + clip.start * scale);
   const auto end = std::min(to, origin + clip.End() * scale);
   if (end <= begin + 1e-7) return;
   out.push_back(begin); out.push_back(end);
   const auto edge = [&](double contentTime) {
      const auto at = origin + clip.ProjectTimeAtContentTime(contentTime) * scale;
      if (at > begin && at < end) out.push_back(at);
   };
   if (clip.opacity.inDuration > 0) { edge(clip.opacity.inStart); edge(clip.opacity.inStart + clip.opacity.inDuration); }
   if (clip.opacity.outDuration > 0) { edge(clip.opacity.outEnd - clip.opacity.outDuration); edge(clip.opacity.outEnd); }
   for (const auto &ramp : clip.opacityModifiers) { edge(ramp.start); edge(ramp.end); }
   if (clip.content && clip.content->operation == VideoClipContent::Operation::Sequence)
      for (const auto &part : clip.content->parts)
         Edges(part, origin + clip.start * scale - clip.sourceStart * scale / clip.playbackRate,
            scale / clip.playbackRate, begin, end, out);
}
void Sources(const Clip &clip, const MediaSource &fallback, std::vector<MediaSource> &out)
{
   if (clip.content) {
      if (clip.content->operation == VideoClipContent::Operation::Image) {
         MediaSource image{clip.content->imagePath, clip.content->duration, clip.content->frameRate,
            clip.content->width, clip.content->height};
         if (std::none_of(out.begin(), out.end(), [&](const auto &item) { return SameSource(item, image); }))
            out.push_back(std::move(image));
      }
      for (const auto &part : clip.content->parts) Sources(part, {}, out);
   }
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
      const auto offset = clip.content->operation == VideoClipContent::Operation::Sequence ? 0.0 : clip.content->audioContentOffset;
      for (const auto &part : clip.content->parts)
         Windows(part, begin - (clip.sourceStart + offset) * scale / clip.playbackRate,
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
   auto crop = clip.crop;
   double origin = clip.start - clip.sourceStart / clip.playbackRate, scale = 1 / clip.playbackRate;
   double opacity = 1;
   std::vector<VideoEditing::VideoOpacityRamp> ramps;
   for (size_t depth{}; depth <= MaximumVideoContentDepth; ++depth) {
      if (current->muted) return {};
      opacity *= VideoEditing::VideoOpacityAt(current->opacity, time);
      for (const auto &ramp : current->opacityModifiers) {
         const auto phase = std::clamp((time - ramp.start) / (ramp.end - ramp.start), 0.0, 1.0);
         opacity *= ramp.increasing ? phase : 1 - phase;
         ramps.push_back({origin + ramp.start * scale, origin + ramp.end * scale, ramp.increasing});
      }
      if (current->opacity.inDuration > 0) ramps.push_back({origin + current->opacity.inStart * scale,
         origin + (current->opacity.inStart + current->opacity.inDuration) * scale, true});
      if (current->opacity.outDuration > 0) ramps.push_back({origin + (current->opacity.outEnd - current->opacity.outDuration) * scale,
         origin + current->opacity.outEnd * scale, false});
      if (!current->content) return {current, Source(*current, current == &clip ? fallback : MediaSource{}),
         time, playbackRate, crop, opacity, std::move(ramps)};
      if (current->content->operation != VideoClipContent::Operation::Sequence) {
         MediaSource source;
         const auto sources = VideoContentSources(*current, {});
         if (!sources.empty()) source = sources.front();
         source.duration = current->content->duration; source.frameRate = current->content->frameRate;
         source.width = current->content->width; source.height = current->content->height;
         return {current, std::move(source), time, playbackRate, crop, opacity, std::move(ramps), current->content};
      }
      const auto &parts = current->content->parts;
      const auto found = std::upper_bound(parts.begin(), parts.end(), time,
         [](double t, const Clip &part) { return t < part.start; });
      if (found == parts.begin()) return {};
      current = &*std::prev(found);
      if (time >= current->End()) return {};
      origin += (current->start - current->sourceStart / current->playbackRate) * scale;
      scale /= current->playbackRate;
      time = current->ContentTimeAtProjectTime(time);
      playbackRate *= current->playbackRate;
      crop = VideoEditing::ComposeVideoFrameCrops(current->crop, crop);
   }
   throw std::runtime_error("Joined video exceeds the supported nesting limit.");
}
VideoContentSample SampleVideoThumbnail(const Clip &clip, const MediaSource &fallback, double time)
{
   auto sample = SampleVideoContent(clip, fallback, time);
   for (size_t depth{}; sample && sample.generated && depth <= MaximumVideoContentDepth; ++depth) {
      const auto &content = *sample.generated;
      using Operation = VideoClipContent::Operation;
      if (content.operation == Operation::Image || content.operation == Operation::Title || content.operation == Operation::Text) return sample;
      if (content.parts.size() != 1) throw std::runtime_error("A picture thumbnail needs one retained input.");
      auto inputTime = std::floor(content.heldTime * content.frameRate + 1e-8) / content.frameRate;
      if (content.operation == Operation::Reverse) {
         const auto last = std::max(0.0, std::ceil(content.duration * content.frameRate - 1e-8) - 1);
         const auto frame = std::clamp(std::floor(sample.sourceTime * content.frameRate + 1e-8), 0.0, last);
         inputTime = (last - frame) / content.frameRate;
      }
      else if (content.operation != Operation::Freeze)
         throw std::runtime_error("The picture thumbnail operation is unsupported.");
      const auto &part = content.parts.front();
      auto input = SampleVideoContent(part, {}, part.ContentTimeAtProjectTime(inputTime));
      input.crop = VideoEditing::ComposeVideoFrameCrops(input.crop, sample.crop);
      sample = std::move(input);
   }
   if (sample.generated) throw std::runtime_error("The picture thumbnail exceeds the nesting limit.");
   return sample;
}
std::vector<double> VideoContentEdges(const Clip &clip)
{ std::vector<double> out; Edges(clip, 0, 1, clip.start, clip.End(), out); return out; }
std::vector<MediaSource> VideoContentSources(const Clip &clip, const MediaSource &fallback)
{ std::vector<MediaSource> out; Sources(clip, fallback, out); return out; }
std::vector<std::string> VideoContentLinkGroups(const Clip &clip)
{ std::set<std::string> out; Groups(clip, out); return {out.begin(), out.end()}; }
std::vector<VideoLinkWindow> VideoContentLinkWindows(const Clip &clip)
{
   RequireUsableVideoContent(clip, {});
   std::vector<VideoLinkWindow> out; Windows(clip, 0, 1, clip.start, clip.End(), out); return out;
}

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
       !std::isfinite(change.timeScale) || change.timeScale <= 0 || !std::isfinite(change.shift))
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
   if (a.muted != b.muted || a.timingLocked != b.timingLocked || !(a.opacity == b.opacity) || a.opacityModifiers != b.opacityModifiers || !(a.crop == b.crop) || a.playbackRate != b.playbackRate || (!a.content && !b.content && !SameSource(a.media, b.media)) ||
       a.markers.size() != b.markers.size()) return false;
   for (size_t i{}; i < a.markers.size(); ++i)
      if (a.markers[i].id != b.markers[i].id || a.markers[i].position != b.markers[i].position ||
          a.markers[i].text != b.markers[i].text) return false;
   if (a.content == b.content) return true;
   if (!a.content || !b.content || a.content->duration != b.content->duration ||
       a.content->frameRate != b.content->frameRate || a.content->parts.size() != b.content->parts.size() ||
       a.content->operation != b.content->operation || a.content->heldTime != b.content->heldTime ||
       a.content->audioContentOffset != b.content->audioContentOffset ||
       a.content->imagePath != b.content->imagePath || a.content->title != b.content->title ||
       a.content->width != b.content->width || a.content->height != b.content->height ||
       !(a.content->textAppearance == b.content->textAppearance)) return false;
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
   if (changed->operation == VideoClipContent::Operation::Image && changed->imagePath == oldPath) {
      changed->imagePath = replacement.path; changed->width = replacement.width; changed->height = replacement.height; found = true;
   }
   for (auto &part : changed->parts) found |= ReplaceVideoContentSource(part, {}, oldPath, replacement);
   if (found) clip.content = std::move(changed);
   return found;
}
}

namespace VideoEditing {
void RequireUsableVideoPlacement(const VideoLayerPlacement &placement)
{
   const auto &r = placement.rectangle;
   for (const auto value : {r.x, r.y, r.width, r.height})
      if (!std::isfinite(value)) throw std::runtime_error("Video placement must be finite.");
   if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0 ||
       r.x + r.width > 1 + 1e-12 || r.y + r.height > 1 + 1e-12 ||
       placement.id.size() > 128 || placement.parentId.size() > 128 ||
       (!placement.parentId.empty() && (placement.id.empty() || placement.id == placement.parentId)))
      throw std::runtime_error("An embedded video needs a distinct parent and a rectangle inside its canvas.");
}
}

namespace VideoTimeline {
bool HasGeneratedVideoContent(const Clip &clip)
{
   return clip.content && (clip.content->operation != VideoClipContent::Operation::Sequence ||
      std::any_of(clip.content->parts.begin(), clip.content->parts.end(), HasGeneratedVideoContent));
}
namespace {
std::shared_ptr<VideoClipContent> RetainGeneratorInput(const Clip &clip, const MediaSource &fallback)
{
   auto result = std::make_shared<VideoClipContent>();
   result->frameRate = std::min(1000.0, VideoContentFrameRate(clip, fallback) * clip.playbackRate);
   auto part = clip; part.start = 0; part.sourceStart = 0; part.muted = false; part.crop = {};
   part.duration = VideoContentDuration(clip, fallback) / clip.playbackRate;
   part.media = Source(clip, fallback);
   result->duration = part.duration;
   const auto sources = VideoContentSources(clip, fallback);
   if (!sources.empty()) { result->width = sources.front().width; result->height = sources.front().height; }
   if (clip.content && clip.content->operation != VideoClipContent::Operation::Sequence) {
      result->width = clip.content->width; result->height = clip.content->height;
   }
   if (result->width < 2 || result->height < 2) { result->width = 1280; result->height = 720; }
   auto uncropped = clip; uncropped.crop = {}; uncropped.muted = false;
   auto picture = SampleVideoContent(uncropped, fallback, clip.sourceStart);
   if (picture && picture.source.width > 0 && picture.source.height > 0) {
      const auto rectangle = VideoEditing::CalculateVideoCropRectangle(picture.crop,
         picture.source.width, picture.source.height, 0);
      result->width = std::max(2, rectangle.width & ~1); result->height = std::max(2, rectangle.height & ~1);
   }
   result->parts.push_back(std::move(part)); return result;
}
Clip GeneratorClip(const Clip &original, const std::shared_ptr<VideoClipContent> &content)
{
   Clip result{original.id, original.start, 0, original.duration};
   result.content = content; result.linkGroup = original.linkGroup;
   result.timingLocked = original.timingLocked; result.muted = original.muted; result.crop = original.crop;
   return result;
}
}
bool ReverseVideoClip(Timeline &timeline, uint64_t id)
{
   RequireWellFormedTimeline(timeline);
   auto candidate = timeline;
   const auto found = std::find_if(candidate.clips.begin(), candidate.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == candidate.clips.end()) throw std::runtime_error("The video clip was removed.");
   if (found->timingLocked) throw std::runtime_error("Unlock this clip before reversing it.");
   if (found->content && found->content->operation == VideoClipContent::Operation::Reverse) return false;
   const auto original = *found;
   auto content = RetainGeneratorInput(original, SourceFor(timeline, original));
   content->operation = VideoClipContent::Operation::Reverse;
   content->parts.front().opacity = {}; content->parts.front().opacityModifiers.clear();
   auto result = GeneratorClip(original, content);
   const auto rate = original.playbackRate;
   result.opacity = {content->duration - original.opacity.outEnd / rate, original.opacity.outDuration / rate,
      content->duration - original.opacity.inStart / rate, original.opacity.inDuration / rate};
   for (const auto &ramp : original.opacityModifiers)
      result.opacityModifiers.push_back({content->duration - ramp.end / rate,
         content->duration - ramp.start / rate, !ramp.increasing});
   result.sourceStart = std::max(0.0, content->duration - original.sourceStart / original.playbackRate - original.duration);
   content->audioContentOffset = original.sourceStart / original.playbackRate - result.sourceStart;
   for (auto marker : original.markers) {
      marker.position = content->duration - marker.position / original.playbackRate;
      result.markers.push_back(std::move(marker));
   }
   *found = std::move(result); RequireWellFormedTimeline(candidate); timeline = std::move(candidate); return true;
}
bool FreezeVideoClip(Timeline &timeline, uint64_t id, double at, double seconds)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600 || !std::isfinite(at))
      throw std::runtime_error("Choose a freeze length greater than zero and at most one hour.");
   auto candidate = timeline;
   const auto found = std::find_if(candidate.clips.begin(), candidate.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == candidate.clips.end()) throw std::runtime_error("The video clip was removed.");
   if (found->timingLocked) throw std::runtime_error("Unlock this clip before freezing it.");
   if (at < found->start || at >= found->End()) throw std::runtime_error("Place the cursor inside the picture to freeze.");
   const auto original = *found;
   if (original.content && original.content->operation == VideoClipContent::Operation::Freeze) {
      if (original.duration == seconds) return false;
      auto content = std::make_shared<VideoClipContent>(*original.content);
      content->duration = std::max(content->duration, original.sourceStart + seconds * original.playbackRate);
      found->content = std::move(content); found->duration = seconds;
      RequireWellFormedTimeline(candidate); timeline = std::move(candidate); return true;
   }
   auto content = RetainGeneratorInput(original, SourceFor(timeline, original));
   content->heldTime = original.sourceStart / original.playbackRate + at - original.start;
   content->operation = VideoClipContent::Operation::Freeze;
   content->audioContentOffset = original.sourceStart / original.playbackRate;
   content->duration = seconds;
   auto result = GeneratorClip(original, content); result.duration = seconds;
   for (auto marker : original.markers) {
      marker.position = (marker.position - original.sourceStart) / original.playbackRate;
      if (marker.position >= 0 && marker.position <= seconds) result.markers.push_back(std::move(marker));
   }
   *found = std::move(result); RequireWellFormedTimeline(candidate); timeline = std::move(candidate); return true;
}
Clip MakeStillVideoClip(uint64_t id, double start, double seconds,
   const std::string &path, int width, int height)
{
   if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600)
      throw std::runtime_error("Choose a picture length greater than zero and at most one hour.");
   auto content = std::make_shared<VideoClipContent>();
   content->duration = seconds; content->width = width; content->height = height;
   content->operation = VideoClipContent::Operation::Image; content->imagePath = path;
   Clip result{id, start, 0, seconds}; result.content = std::move(content);
   RequireUsableVideoContent(result, {}); return result;
}
Clip MakeTitleVideoClip(uint64_t id, double start, double seconds,
   const std::string &text, int width, int height)
{
   auto result = MakeStillVideoClip(id, start, seconds, "title", width, height);
   auto content = std::make_shared<VideoClipContent>(*result.content);
   content->operation = VideoClipContent::Operation::Title; content->imagePath.clear(); content->title = text;
   result.content = std::move(content); RequireUsableVideoContent(result, {}); return result;
}
Clip MakeTextVideoClip(uint64_t id, double start, double seconds,
   const std::string &text, int width, int height, const VideoEditing::VideoTextAppearance &appearance)
{
   auto result = MakeTitleVideoClip(id, start, seconds, text, width, height);
   auto content = std::make_shared<VideoClipContent>(*result.content);
   content->operation = VideoClipContent::Operation::Text; content->textAppearance = appearance;
   result.content = std::move(content); RequireUsableVideoContent(result, {}); return result;
}
bool VideoContentUsesAlpha(const Clip &clip)
{
   return clip.content && VideoContentUsesAlpha(*clip.content);
}
bool VideoContentUsesAlpha(const VideoClipContent &content)
{
   return content.operation == VideoClipContent::Operation::Text ||
      std::any_of(content.parts.begin(), content.parts.end(),
         [](const Clip &part) { return VideoContentUsesAlpha(part); });
}
bool IsStaticVideoContent(const VideoClipContent &content)
{
   using Operation = VideoClipContent::Operation;
   return content.operation == Operation::Image || content.operation == Operation::Title ||
      content.operation == Operation::Text || content.operation == Operation::Freeze;
}
const VideoClipContent *FindEditableVideoTextContent(const Clip &clip)
{
   const auto *node = clip.content.get();
   for (size_t depth{}; node && depth <= MaximumVideoContentDepth; ++depth) {
      if (node->operation == VideoClipContent::Operation::Text) return node;
      if ((node->operation != VideoClipContent::Operation::Reverse &&
           node->operation != VideoClipContent::Operation::Freeze) || node->parts.size() != 1) return nullptr;
      node = node->parts.front().content.get();
   }
   return nullptr;
}
bool ChangeVideoTextContent(Clip &clip, const std::string &text,
   const VideoEditing::VideoTextAppearance &appearance)
{
   const auto *current = FindEditableVideoTextContent(clip);
   if (!current) throw std::runtime_error("Separate joined text clips to edit their individual wording.");
   RequireUsableVideoContent(clip, {});
   MakeTextVideoClip(1, 0, std::min(3600.0, current->duration), text,
      current->width, current->height, appearance);
   if (current->title == text && current->textAppearance == appearance) return false;
   const auto replace = [&](const auto &self, Clip &target) -> void {
      auto content = std::make_shared<VideoClipContent>(*target.content);
      if (content->operation == VideoClipContent::Operation::Text) {
         content->title = text; content->textAppearance = appearance;
      }
      else self(self, content->parts.front());
      target.content = std::move(content);
   };
   auto changed = clip; replace(replace, changed); RequireUsableVideoContent(changed, {});
   clip = std::move(changed); return true;
}
bool ResizeVideoTextContent(Clip &clip, int width, int height)
{
   if (width < 2 || height < 2 || width > 8192 || height > 8192)
      throw std::runtime_error("A text overlay needs usable picture dimensions.");
   RequireUsableVideoContent(clip, clip.media);
   const auto resize = [&](const auto &self, Clip &target) -> bool {
      if (!target.content || !VideoContentUsesAlpha(target)) return false;
      auto content = std::make_shared<VideoClipContent>(*target.content);
      bool changed = content->width != width || content->height != height;
      content->width = width; content->height = height;
      for (auto &part : content->parts) changed = self(self, part) || changed;
      if (changed) target.content = std::move(content);
      return changed;
   };
   auto candidate = clip;
   if (!resize(resize, candidate)) return false;
   RequireUsableVideoContent(candidate, candidate.media);
   clip = std::move(candidate); return true;
}
bool RepeatVideoClip(Timeline &timeline, uint64_t id, int copies)
{
   RequireWellFormedTimeline(timeline);
   if (copies < 1 || copies > 100) throw std::runtime_error("Choose between one and one hundred extra picture copies.");
   const auto found = std::find_if(timeline.clips.begin(), timeline.clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == timeline.clips.end()) throw std::runtime_error("The video clip was removed.");
   if (found->timingLocked) throw std::runtime_error("Unlock this clip before repeating it.");
   auto picture = *found; picture.media = SourceFor(timeline, picture);
   std::vector<std::pair<std::string, std::string>> detached;
   for (const auto &group : VideoContentLinkGroups(picture)) detached.emplace_back(group, std::string{});
   RemapVideoContentLinks(picture, detached);
   auto candidate = timeline; bool changed{};
   for (int i = 1; i <= copies; ++i) {
      auto copy = picture; copy.start = picture.start + picture.duration * i;
      const auto existing = std::find_if(candidate.clips.begin(), candidate.clips.end(), [&](const auto &clip) {
         return std::abs(clip.start - copy.start) < 1e-7 && std::abs(clip.duration - copy.duration) < 1e-7 &&
            std::abs(clip.sourceStart - copy.sourceStart) < 1e-7 && SameVideoContent(clip, copy);
      });
      if (existing != candidate.clips.end()) continue;
      copy.id = candidate.nextId++; candidate.clips.push_back(std::move(copy)); changed = true;
   }
   std::sort(candidate.clips.begin(), candidate.clips.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
   RequireWellFormedTimeline(candidate);
   if (changed) timeline = std::move(candidate); return changed;
}
}

namespace VideoEditing {
void RequireUsableVideoTextAppearance(const VideoTextAppearance &appearance)
{
   if (!std::isfinite(appearance.heightFraction) || appearance.heightFraction < .01 ||
       appearance.heightFraction > .9 || appearance.rgb > 0xffffff || appearance.fontFace.size() > 256 ||
       std::any_of(appearance.fontFace.begin(), appearance.fontFace.end(),
          [](unsigned char c) { return c < 32; }))
      throw std::runtime_error("Choose a usable font, text colour and text size between 1% and 90% of the box height.");
}
}
