// AuTopsy video export through the native audio mixer and bundled FFmpeg.
#pragma once
#include <wx/string.h>
#include <vector>
#include "EditVideoTimeline.h"
#include "ChooseVideoExport.h"
class AudacityProject;
class wxWindow;
namespace VideoExport {
void CleanAbandonedWorkspace();
enum class VideoTool { Encoder, Probe };
// Shared bounded child-process adapter. The caller owns its output directory;
// cancellation returns false and closing AuTopsy terminates the child process.
bool RunBundledVideoTool(const std::vector<wxString> &arguments, wxWindow *parent,
   const wxString &title, const wxString &status, std::string *output = nullptr,
   VideoTool tool = VideoTool::Encoder);
bool ExportMix(AudacityProject &project, const VideoTimeline::Timeline &timeline,
   double projectVideoDuration, const wxString &destination, bool overwrite, wxWindow *parent,
   const ExportSettings &settings = {});
}
