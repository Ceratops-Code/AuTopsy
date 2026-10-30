#include "JoinVideoClips.h"
#include <iomanip>
#include <sstream>
#include <set>

namespace VideoTimeline {
namespace {
bool Selected(const Clip &clip, double from, double to)
{
   return from == to ? clip.start <= from + 1e-7 && clip.End() >= from - 1e-7
      : clip.start < to - 1e-7 && clip.End() > from + 1e-7;
}
std::vector<size_t> Selection(const Timeline &timeline, double from, double to)
{
   RequireWellFormedTimeline(timeline);
   if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to < from)
      throw std::runtime_error("Choose an ordered, non-negative video range.");
   std::vector<size_t> result;
   for (size_t i{}; i < timeline.clips.size(); ++i)
      if (Selected(timeline.clips[i], from, to)) result.push_back(i);
   return result;
}
void AddMarker(std::vector<ClipMarkers::Marker> &markers, ClipMarkers::Marker marker)
{
   const auto same = std::find_if(markers.begin(), markers.end(), [&](const auto &m) {
      return m.id == marker.id && std::abs(m.position - marker.position) < 1e-7 && m.text == marker.text;
   });
   if (same != markers.end()) return;
   // Repeated source occurrences can carry the same original flag identity.
   // Give the additional occurrence a deterministic unused local identity.
   uint64_t next{};
   while (std::any_of(markers.begin(), markers.end(), [&](const auto &m) { return m.id == marker.id; })) {
      std::ostringstream id; id << std::hex << std::setfill('0') << std::setw(32) << ++next;
      marker.id = id.str();
   }
   markers.push_back(std::move(marker));
   ClipMarkers::RequireUsableMarkers(markers);
}
}
bool HealVideoClips(Timeline &timeline, double from, double to)
{
   const auto selected = Selection(timeline, from, to);
   if (selected.size() < 2) return false;
   auto result = timeline.clips[selected.front()];
   const auto source = SourceFor(timeline, result);
   bool differentLinks{};
   for (size_t i = 1; i < selected.size(); ++i) {
      auto part = timeline.clips[selected[i]];
      const auto other = SourceFor(timeline, part);
      if (std::abs(result.End() - part.start) > 1e-7 ||
          std::abs(result.ContentTimeAtProjectTime(result.End()) - part.sourceStart) > 1e-7 ||
          source.path != other.path || source.duration != other.duration || source.frameRate != other.frameRate ||
          source.width != other.width || source.height != other.height)
         throw std::runtime_error("Heal Split needs touching pieces of continuous footage with compatible links. Use Join otherwise.");
      auto a = result, b = part; a.markers.clear(); b.markers.clear();
      a.media = source; b.media = other;
       a.appearance = {}; b.appearance = {};
      if (!SameVideoContent(a, b))
         throw std::runtime_error("These clips do not share the same retained content. Use Join instead.");
      differentLinks |= result.linkGroup != part.linkGroup;
      for (const auto &marker : part.markers) {
         const auto collision = std::find_if(result.markers.begin(), result.markers.end(),
            [&](const auto &m) { return m.id == marker.id; });
         if (collision != result.markers.end() &&
             (collision->position != marker.position || collision->text != marker.text))
            throw std::runtime_error("Flags at this split have different edits. Use Join to retain both.");
         if (collision == result.markers.end()) result.markers.push_back(marker);
      }
      result.duration += part.duration;
   }
   // Native linked splits deliberately have different groups on each side.
   // Retain that relationship as content instead of merging soundtrack samples.
   if (differentLinks) return JoinVideoClips(timeline, from, to);
   auto changed = timeline;
   changed.clips.erase(changed.clips.begin() + selected.front(), changed.clips.begin() + selected.back() + 1);
   changed.clips.insert(changed.clips.begin() + selected.front(), std::move(result));
   RequireWellFormedTimeline(changed); timeline = std::move(changed); return true;
}
bool JoinVideoClips(Timeline &timeline, double from, double to)
{
   const auto selected = Selection(timeline, from, to);
   if (selected.size() < 2) return false;
   const auto &first = timeline.clips[selected.front()], &last = timeline.clips[selected.back()];
   auto content = std::make_shared<VideoClipContent>();
   content->frameRate = VideoContentFrameRate(first, SourceFor(timeline, first));
   const auto handle = first.sourceStart / first.playbackRate;
   content->duration = handle + last.End() - first.start +
      (VideoContentDuration(last, SourceFor(timeline, last)) - last.sourceStart) / last.playbackRate - last.duration;
   Clip joined{first.id, first.start, handle, last.End() - first.start};
   joined.linkGroup = first.linkGroup;
   joined.appearance = first.appearance;
   for (const auto index : selected) {
      auto part = timeline.clips[index]; part.media = SourceFor(timeline, part);
      part.start = handle + part.start - first.start;
      if (index == selected.front()) {
         part.start -= part.sourceStart / part.playbackRate;
         part.duration += part.sourceStart / part.playbackRate; part.sourceStart = 0;
      }
      if (index == selected.back()) part.duration = (VideoContentDuration(part, part.media) - part.sourceStart) / part.playbackRate;
      for (auto marker : part.markers) {
         marker.position = part.ProjectTimeAtContentTime(marker.position);
         if (marker.position >= part.start - 1e-7 && marker.position <= part.End() + 1e-7)
            AddMarker(joined.markers, std::move(marker));
      }
      content->parts.push_back(std::move(part));
   }
   joined.content = std::move(content);
   auto changed = timeline;
   changed.clips.erase(changed.clips.begin() + selected.front(), changed.clips.begin() + selected.back() + 1);
   changed.clips.insert(changed.clips.begin() + selected.front(), std::move(joined));
   RequireWellFormedTimeline(changed); timeline = std::move(changed); return true;
}
static bool SeparateOneLevel(Timeline &timeline, double from, double to)
{
   const auto selected = Selection(timeline, from, to);
   auto changed = timeline; changed.clips.clear(); bool any{};
   std::set<uint64_t> used;
   for (size_t i{}; i < timeline.clips.size(); ++i)
      if (!timeline.clips[i].content || timeline.clips[i].content->operation != VideoClipContent::Operation::Sequence ||
          std::find(selected.begin(), selected.end(), i) == selected.end())
         used.insert(timeline.clips[i].id);
   for (size_t i{}; i < timeline.clips.size(); ++i) {
      const auto &clip = timeline.clips[i];
      // Generated pictures are editable leaves, even though they own content.
      // Only joined sequences can be expanded; subsequent passes must stop at
      // retained text/still/reverse/freeze nodes without stripping their effect.
      if (!clip.content || clip.content->operation != VideoClipContent::Operation::Sequence ||
          std::find(selected.begin(), selected.end(), i) == selected.end()) {
         changed.clips.push_back(clip); continue;
      }
      any = true;
      // A slipped compound can have different picture and soundtrack cuts.
      // Flattening it would assign sound to the wrong source piece. Retain the
      // compound until the user explicitly unlinks the independently edited sound.
      if (std::abs(clip.audioSourceOffset) > 1e-7 && !VideoContentLinkGroups(clip).empty())
         throw std::runtime_error("Unlink the soundtrack before separating a slipped joined clip; its picture and audio cuts differ.");
      const auto windowEnd = clip.ContentTimeAtProjectTime(clip.End());
      if (std::none_of(clip.content->parts.begin(), clip.content->parts.end(), [&](const auto &part) {
            return part.start < windowEnd - 1e-7 && part.End() > clip.sourceStart + 1e-7; }))
         throw std::runtime_error("This window contains only a gap. Untrim it to visible footage before separating.");
      for (const auto &marker : clip.markers)
         if (marker.position >= clip.sourceStart && marker.position < windowEnd &&
             std::none_of(clip.content->parts.begin(), clip.content->parts.end(), [&](const auto &part) {
                return marker.position >= part.start && marker.position < part.End(); }))
            throw std::runtime_error("Move or remove flags inside a joined gap before separating it.");
      for (const auto &original : clip.content->parts) {
         const auto begin = std::max(original.start, clip.sourceStart);
         const auto end = std::min(original.End(), windowEnd);
         if (end <= begin + 1e-7) continue;
         auto part = original;
         if (used.count(part.id)) part.id = changed.nextId++;
         used.insert(part.id); changed.nextId = std::max(changed.nextId, part.id + 1);
         // Keep flags hidden by this child's original cuts. Visible/outer-handle
         // flags come from the parent, including edits made while it was joined.
         part.markers.erase(std::remove_if(part.markers.begin(), part.markers.end(), [&](const auto &marker) {
            const auto at = original.ProjectTimeAtContentTime(marker.position);
            return at >= original.start - 1e-7 && at <= original.End() + 1e-7;
         }), part.markers.end());
         for (auto marker : clip.markers)
            if (marker.position >= original.start - 1e-7 && marker.position <= original.End() + 1e-7) {
               marker.position = original.ContentTimeAtProjectTime(marker.position); AddMarker(part.markers, std::move(marker));
            }
         part.start = clip.ProjectTimeAtContentTime(begin);
         part.sourceStart = original.ContentTimeAtProjectTime(begin);
         part.duration = (end - begin) / clip.playbackRate;
         part.playbackRate *= clip.playbackRate;
         part.crop = VideoEditing::ComposeVideoFrameCrops(part.crop, clip.crop);
         part.muted = part.muted || clip.muted;
         part.timingLocked = part.timingLocked || clip.timingLocked;
         auto inherited = clip.opacityModifiers;
         if (clip.opacity.inDuration > 0) inherited.push_back({clip.opacity.inStart,
            clip.opacity.inStart + clip.opacity.inDuration, true});
         if (clip.opacity.outDuration > 0) inherited.push_back({clip.opacity.outEnd - clip.opacity.outDuration,
            clip.opacity.outEnd, false});
         for (const auto &ramp : inherited) part.opacityModifiers.push_back({
            original.ContentTimeAtProjectTime(ramp.start), original.ContentTimeAtProjectTime(ramp.end), ramp.increasing});
         VideoEditing::RequireUsableVideoOpacityRamps(part.opacityModifiers);
         changed.clips.push_back(std::move(part));
      }
   }
   if (!any) return false;
   RequireWellFormedTimeline(changed); timeline = std::move(changed); return true;
}
bool SeparateVideoClips(Timeline &timeline, double from, double to)
{
   // Separate reaches editable source pieces, including nested joins. A fresh
   // repeat sees that desired state and has no additional effect. All levels
   // are built privately so an inner flag/identity error cannot publish half
   // of a separation.
   auto changed = timeline; bool any{};
   for (size_t depth{}; depth <= 16; ++depth) {
      if (!SeparateOneLevel(changed, from, to)) {
         if (any) timeline = std::move(changed);
         return any;
      }
      any = true;
   }
   throw std::runtime_error("Joined video exceeds the supported nesting limit.");
}
}
