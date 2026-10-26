// Desktop boundary for capture. Device/format policy lives in the owned planner;
// the existing media worker and importer own child lifetime and project edits.
#pragma once
#include "PlanVideoRecording.h"
class AudacityProject;
namespace VideoRecording {
bool RecordVideoInput(AudacityProject &project, const RecordingOptions &options);
}
