// Native commands publish owned picture envelopes in one undoable edit.
#pragma once
#include <cstdint>
class AudacityProject;
class AuTopsyVideoTrack;
class wxWindow;
namespace VideoEditing {
bool ChangeVideoClipFades(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, double fadeIn, double fadeOut);
void ShowVideoClipFades(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent, bool crossfade = false);
}
