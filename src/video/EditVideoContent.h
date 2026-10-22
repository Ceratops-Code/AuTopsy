// Owned immutable picture content. Native tracks own placement and history;
// these values retain media, nested pieces and gaps without rendering them.
#pragma once
#include "../clip-groups/EditClipMarkers.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace VideoTimeline {
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
   double End() const { return start + duration; }
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
std::vector<std::string> VideoContentLinkGroups(const Clip &clip);
struct VideoLinkWindow {
   std::string group;
   double start{}, end{}, sourceOrigin{}, sourceEnd{};
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
