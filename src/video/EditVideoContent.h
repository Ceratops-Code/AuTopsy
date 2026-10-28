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
// Text size is a fraction of the caption box, independent of preview quality
// and export resolution. Fonts are saved by family, never by machine paths.
struct VideoTextAppearance {
   std::string fontFace;
   double heightFraction{.4};
   uint32_t rgb{0xffffff};
   bool bold{}, italic{}, outline{true};
   bool operator==(const VideoTextAppearance &other) const {
      return fontFace == other.fontFace && heightFraction == other.heightFraction &&
         rgb == other.rgb && bold == other.bold && italic == other.italic && outline == other.outline;
   }
};
void RequireUsableVideoTextAppearance(const VideoTextAppearance &appearance);
// Canvas fractions retain framing across proxies, export sizes and relinking.
struct VideoFrameRectangle {
   double x{}, y{}, width{1}, height{1};
   bool operator==(const VideoFrameRectangle &other) const {
      return x == other.x && y == other.y && width == other.width && height == other.height;
   }
};
struct VideoLayerPlacement {
   std::string id, parentId; // Opaque owned identities; never pane positions.
   VideoFrameRectangle rectangle;
};
void RequireUsableVideoPlacement(const VideoLayerPlacement &placement);
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
// Anchors are in retained content seconds, so splitting or trimming a clip
// preserves its original opacity curve rather than starting new fades at cuts.
struct VideoOpacityEnvelope {
   double inStart{}, inDuration{}, outEnd{}, outDuration{};
   bool operator==(const VideoOpacityEnvelope &other) const {
      return inStart == other.inStart && inDuration == other.inDuration &&
         outEnd == other.outEnd && outDuration == other.outDuration;
   }
};
struct VideoOpacityRamp {
   double start{}, end{}; bool increasing{};
   bool operator==(const VideoOpacityRamp &other) const {
      return start == other.start && end == other.end && increasing == other.increasing;
   }
};
inline constexpr size_t MaximumVideoOpacityRamps = 128;
void RequireUsableVideoOpacityRamps(const std::vector<VideoOpacityRamp> &ramps);
std::string EncodeVideoOpacityRamps(const std::vector<VideoOpacityRamp> &ramps);
std::vector<VideoOpacityRamp> DecodeVideoOpacityRamps(const std::string &text);
double VideoOpacityAt(const VideoOpacityEnvelope &envelope, double contentTime);
}

namespace VideoTimeline {
struct Timeline;
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
   bool muted{}; // Picture only; retained media and separately editable sound remain intact.
   bool timingLocked{}; // Optional fork state; a locked member protects linked timing edits too.
   VideoEditing::VideoOpacityEnvelope opacity;
   // Parent envelopes projected into this content clock by Separate. Products
   // cannot be replaced with a single linear ramp without changing the picture.
   std::vector<VideoEditing::VideoOpacityRamp> opacityModifiers;
   double End() const { return start + duration; }
   double ContentTimeAtProjectTime(double at) const { return sourceStart + (at - start) * playbackRate; }
   double ProjectTimeAtContentTime(double at) const { return start + (at - sourceStart) / playbackRate; }
};
struct VideoClipContent {
   double duration{}, frameRate{30.0};
   // Child placement is in content seconds. Missing intervals are transparent.
   // Children own resolved sources, so moving the parent to another track is safe.
   std::vector<Clip> parts;
   // Generators retain their source tree. Cache files are derived and can be
   // discarded without losing the edit; sequence remains the legacy default.
   enum class Operation { Sequence, Reverse, Freeze, Image, Title, Text };
   Operation operation{Operation::Sequence};
   double heldTime{};
   // Picture-only generators keep soundtrack links on their original forward
   // clock, independently of reversed or held pictures.
   double audioContentOffset{};
   std::string imagePath, title;
   VideoEditing::VideoTextAppearance textAppearance;
   int width{1280}, height{720};
};
struct VideoContentSample {
   const Clip *leaf{};
   MediaSource source;
   double sourceTime{};
   double playbackRate{1}; // Product of the rates along the retained-content path.
   VideoEditing::VideoFrameCrop crop;
   double opacity{1};
   std::vector<VideoEditing::VideoOpacityRamp> opacityRamps; // In root project coordinates.
   std::shared_ptr<const VideoClipContent> generated;
   explicit operator bool() const { return leaf != nullptr; }
};
double VideoContentDuration(const Clip &clip, const MediaSource &fallback);
double VideoContentFrameRate(const Clip &clip, const MediaSource &fallback);
void RequireUsableVideoContent(const Clip &clip, const MediaSource &fallback);
VideoContentSample SampleVideoContent(const Clip &clip, const MediaSource &fallback, double contentTime);
// Resolve a thumbnail's picture clock to original media without rendering.
// Image/title results stay opaque for the native bitmap/text adapter.
VideoContentSample SampleVideoThumbnail(const Clip &clip, const MediaSource &fallback, double contentTime);
// Sorted picture boundaries in the parent's project coordinates, including
// nested source cuts. Used once per export plan, never rebuilt per video frame.
std::vector<double> VideoContentEdges(const Clip &clip);
std::vector<MediaSource> VideoContentSources(const Clip &clip, const MediaSource &fallback);
struct VideoClipRetiming {
   double anchor{}, timeScale{1}, shift{};
   double ProjectTimeAfterRetiming(double at) const { return anchor + (at - anchor) * timeScale + shift; }
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
// Inputs own their leaf sources. Empty windows retain a fully trimmed linked
// soundtrack piece for untrim; a missing source cannot produce a timing map.
std::vector<VideoLinkWindow> VideoContentLinkWindows(const Clip &clip);
void RemapVideoContentLinks(Clip &clip, const std::vector<std::pair<std::string, std::string>> &groups);
bool SameVideoContent(const Clip &left, const Clip &right);
// Copy-on-write replacement includes hidden/nested sources and leaves Undo
// snapshots untouched. The caller checks the complete candidate before publishing.
bool ReplaceVideoContentSource(Clip &clip, const MediaSource &fallback,
   const std::string &oldPath, const MediaSource &replacement);
bool HasGeneratedVideoContent(const Clip &clip);
// Transparency propagates through retained joins/reverse/freeze. Losing it in
// an intermediate would put a black rectangle over the parent picture.
bool VideoContentUsesAlpha(const Clip &clip);
bool VideoContentUsesAlpha(const VideoClipContent &content);
bool IsStaticVideoContent(const VideoClipContent &content);
const VideoClipContent *FindEditableVideoTextContent(const Clip &clip);
bool ChangeVideoTextContent(Clip &clip, const std::string &text,
   const VideoEditing::VideoTextAppearance &appearance);
// Full retained windows, including handles, remain available after reverse.
// These operations affect picture only; the native soundtrack stays editable.
bool ReverseVideoClip(Timeline &timeline, uint64_t id);
bool FreezeVideoClip(Timeline &timeline, uint64_t id, double at, double seconds);
bool RepeatVideoClip(Timeline &timeline, uint64_t id, int copies);
Clip MakeStillVideoClip(uint64_t id, double start, double seconds,
   const std::string &path, int width, int height);
Clip MakeTitleVideoClip(uint64_t id, double start, double seconds,
   const std::string &text, int width, int height);
Clip MakeTextVideoClip(uint64_t id, double start, double seconds,
   const std::string &text, int width, int height,
   const VideoEditing::VideoTextAppearance &appearance);
}
namespace VideoEditing {
bool HasVideoOpacity(const VideoTimeline::Clip &clip);
}
