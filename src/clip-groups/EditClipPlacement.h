// Narrow native boundary for timing, locks and gap closing; sample processing stays native.
#pragma once
#include "PlanClipPlacement.h"
#include <cstdint>
class AudacityProject;
class Track;
class WaveTrack;
class WaveClip;
class AuTopsyVideoTrack;
class wxWindow;
struct ClipMoveState;
namespace ClipActions {
bool MovingMediaHasLockedClips(const ClipMoveState &state);
enum class TimingEditScope { Selection, Following, OutsideSelection };
void RequireUnlockedTrackMedia(const Track &track, double from, double to, TimingEditScope scope);
void RequireUnlockedSelectedMedia(AudacityProject &project,
   TimingEditScope scope = TimingEditScope::Selection, bool syncLock = false);
void ShowAudioClipProperties(AudacityProject &project, WaveTrack &track, WaveClip &clip, wxWindow *parent);
void ShowVideoClipProperties(AudacityProject &project, AuTopsyVideoTrack &track, uint64_t id, wxWindow *parent);
bool ApplyAudioClipProperties(AudacityProject &project, WaveTrack &track, WaveClip &clip,
   const ClipTimingProperties &value);
bool ApplyVideoClipProperties(AudacityProject &project, AuTopsyVideoTrack &track, uint64_t id,
   const ClipTimingProperties &value);
void CloseGapBeforeAudioClip(AudacityProject &project, WaveTrack &track, WaveClip &clip);
void CloseGapBeforeVideoClip(AudacityProject &project, AuTopsyVideoTrack &track, uint64_t id);
}
