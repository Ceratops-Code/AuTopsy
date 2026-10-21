// Owned media replacement keeps edits independent of filesystem locations.
#pragma once
#include "EditVideoTimeline.h"
class AudacityProject;
namespace VideoTimeline {
inline bool ReplaceVideoSource(Timeline &timeline, const std::string &oldPath,
   const MediaSource &replacement)
{
   RequireWellFormedTimeline(timeline);
   RequireUsableVideoSource(replacement);
   if (oldPath.empty()) throw std::runtime_error("Choose the source to relink.");
   auto changed = timeline;
   bool found{};
   for (auto &clip : changed.clips)
      if (SourceFor(timeline, clip).path == oldPath) {
         clip.media = replacement; found = true;
      }
   if (!found) return false;
   if (changed.sourcePath == oldPath) {
      changed.sourcePath = replacement.path; changed.sourceDuration = replacement.duration;
      changed.frameRate = replacement.frameRate; changed.sourceWidth = replacement.width;
      changed.sourceHeight = replacement.height;
   }
   for (auto &path : changed.protectedSourcePaths) if (path == oldPath) path = replacement.path;
   RequireWellFormedTimeline(changed);
   timeline = std::move(changed);
   return true;
}
void ReloadProjectVideoPreview(AudacityProject &project);
}
