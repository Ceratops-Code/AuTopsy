// AuTopsy video export through the native audio mixer and bundled FFmpeg.
#pragma once
#include <wx/string.h>
#include <vector>
#include "EditVideoTimeline.h"
class AudacityProject;
class wxWindow;
namespace VideoExport {
void CleanAbandonedWorkspace();
// Shared bounded child-process adapter. The caller owns its output directory;
// cancellation returns false and closing AuTopsy terminates the child process.
bool RunBundledVideoTool(const std::vector<wxString> &arguments, wxWindow *parent,
   const wxString &title, const wxString &status);
bool ExportMix(AudacityProject &project, const VideoTimeline::Timeline &timeline,
   const wxString &destination, bool overwrite, wxWindow *parent);
}
