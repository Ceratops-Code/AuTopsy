// Owned subtitle interchange and clip planning. No GUI, decoder or host types.
#pragma once
#include "EditVideoTimeline.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace VideoCaptions {
struct Cue {
   int64_t startMs{}, endMs{};
   std::string text;
};
enum class FileFormat { Srt, WebVtt };
inline constexpr size_t MaximumCaptionFileBytes = 8 * 1024 * 1024;
std::vector<Cue> ReadCaptionCues(const std::string &utf8, FileFormat format);
std::string WriteCaptionCues(const std::vector<Cue> &cues, FileFormat format);
// Overlapping subtitles become consecutive clips containing all active words.
// Source cues remain unchanged; all bounds are checked before returning a plan.
std::vector<Cue> CombineCaptionOverlaps(const std::vector<Cue> &cues);
std::vector<Cue> PlaceCaptionCues(const std::vector<Cue> &cues, double offset,
   double from, double to);
// Convert elapsed recognition boundaries through the frozen native audio clock.
// Reject an invalid mapping before returning any clips; retain project bounds.
std::vector<Cue> MapRecognitionCues(const std::vector<Cue> &cues, double from, double to,
   const std::function<double(double)> &projectTimeForElapsed);
VideoTimeline::Timeline PlanCaptionClips(const std::vector<Cue> &cues,
   int width, int height, const VideoEditing::VideoTextAppearance &appearance = {});
// Uses rendered content boundaries, preserving joins, cuts, speed and reverse.
std::vector<Cue> ReadTimelineCaptions(const VideoTimeline::Timeline &timeline);
}
