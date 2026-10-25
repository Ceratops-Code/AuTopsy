// Clip-menu boundary: native effects/export remain responsible for audio work.
#pragma once
#include <cstdint>
class AudacityProject;
class AuTopsyVideoTrack;
class CommandContext;
class wxWindow;
namespace ClipActions {
void ShowAudioClipVolume(const CommandContext& context);
void ShowAudioClipExport(const CommandContext& context);
void ShowAudioClipProperties(const CommandContext& context);
void CloseAudioClipGap(const CommandContext& context);
void ShowVideoClipExport(AudacityProject& project, const AuTopsyVideoTrack& track,
   uint64_t clipId, wxWindow* parent);
}
