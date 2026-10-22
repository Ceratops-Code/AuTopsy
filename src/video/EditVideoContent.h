// Owned immutable picture content. Native tracks own placement and history;
// these values retain media, nested pieces and gaps without rendering them.
#pragma once
#include "../clip-groups/EditClipMarkers.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace VideoEditing {
// Fractions of the oriented picture removed from each edge. Content owns these
// values so retained pieces keep their picture edits through joins and copies.
struct VideoFrameCrop {
   double left{}, top{}, right{}, bottom{};
   bool operator==(const VideoFrameCrop &other) const {
      return left == other.left && top == other.top &&
         right == other.right && bottom == other.bottom;
   }
};
void RequireUsableVideoFrameCrop(const VideoFrameCrop &crop);
// Apply outer crop to an already cropped inner picture.
VideoFrameCrop ComposeVideoFrameCrops(const VideoFrameCrop &inner, const VideoFrameCrop &outer);
}

namespace VideoTimeline {
// Limits apply to one retained content tree, including its owning clip. The
// native reader uses the same bounds; a track may contain many such trees.
inline constexpr size_t MaximumVideoContentDepth = 16;
inline constexpr size_t MaximumVideoContentClips = 10000;
inline constexpr size_t MaximumVideoTrackClips = 10000;
struct MediaSource {
   std::string path;
   double duration{}, frameRate{30.0};
   int width{}, height{};
};
struct VideoClipContent;
struct Clip {
   uint64_t id{};
   double start{}, sourceStart{}, duration{};
   MediaSource media; // Used only for a simple clip; a compound owns its leaves.
   std::string linkGroup;
   std::vector<ClipMarkers::Marker> markers;
   // sourceStart is a window into content, or into media for a simple clip.
   // Snapshots share immutable content; editing a window never edits a sibling.
   std::shared_ptr<const VideoClipContent> content;
   double playbackRate{1}; // Content seconds per second in the containing timeline.
   VideoEditing::VideoFrameCrop crop;
   double End() const { return start + duration; }
   double ContentTimeAtProjectTime(double at) const { return sourceStart + (at - start) * playbackRate; }
   double ProjectTimeAtContentTime(double at) const { return start + (at - sourceStart) / playbackRate; }
};
struct VideoClipContent {
   double duration{}, frameRate{30.0};
   // Child placement is in content seconds. Missing intervals are transparent.
   // Children own resolved sources, so moving the parent to another track is safe.
   std::vector<Clip> parts;
};
struct VideoContentSample {
   const Clip *leaf{};
   MediaSource source;
   double sourceTime{};
   double playbackRate{1}; // Product of the rates along the retained-content path.
   VideoEditing::VideoFrameCrop crop;
   explicit operator bool() const { return leaf != nullptr; }
};
double VideoContentDuration(const Clip &clip, const MediaSource &fallback);
double VideoContentFrameRate(const Clip &clip, const MediaSource &fallback);
void RequireUsableVideoContent(const Clip &clip, const MediaSource &fallback);
VideoContentSample SampleVideoContent(const Clip &clip, const MediaSource &fallback, double contentTime);
// Sorted picture boundaries in the parent's project coordinates, including
// nested source cuts. Used once per export plan, never rebuilt per video frame.
std::vector<double> VideoContentEdges(const Clip &clip);
std::vector<MediaSource> VideoContentSources(const Clip &clip, const MediaSource &fallback);
struct VideoClipRetiming {
   double anchor{}, timeScale{1};
   double ProjectTimeAfterRetiming(double at) const { return anchor + (at - anchor) * timeScale; }
};
// Prepare one affine timing change for the complete linked group. Retained
// content and flags stay in content coordinates; no media is rewritten.
VideoClipRetiming PrepareVideoClipRetiming(const Clip &anchor, double playbackRate);
Clip RetimeVideoClip(const Clip &clip, const MediaSource &fallback, const VideoClipRetiming &change);
std::vector<std::string> VideoContentLinkGroups(const Clip &clip);
struct VideoLinkWindow {
   std::string group;
   double start{}, end{}, sourceOrigin{}, sourceEnd{};
   double playbackRate{1};
   bool wholeContent{}; // A manual link owned by a joined parent, not a source leaf.
};
// Empty windows retain a fully trimmed linked soundtrack piece for untrim.
std::vector<VideoLinkWindow> VideoContentLinkWindows(const Clip &clip);
void RemapVideoContentLinks(Clip &clip, const std::vector<std::pair<std::string, std::string>> &groups);
bool SameVideoContent(const Clip &left, const Clip &right);
// Copy-on-write replacement includes hidden/nested sources and leaves Undo
// snapshots untouched. The caller checks the complete candidate before publishing.
bool ReplaceVideoContentSource(Clip &clip, const MediaSource &fallback,
   const std::string &oldPath, const MediaSource &replacement);
}
