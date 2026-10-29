// AuTopsy video export through the native audio mixer and bundled FFmpeg.
#pragma once
#include <wx/string.h>
#include <vector>
#include <optional>
#include <utility>
#include "EditVideoTimeline.h"
#include "ChooseVideoExport.h"
class AudacityProject;
class wxWindow;
namespace VideoExport {
void CleanAbandonedWorkspace();
enum class VideoTool { Encoder, Probe, Transcriber };
// Uses the existing leased, destination-volume staging and atomic publication.
void WriteMediaTextFile(const wxString &destination, const std::string &text, bool overwrite);
std::optional<std::pair<int, int>> ReadVideoSourcePictureSize(const wxString &source, wxWindow *parent);
// Shared bounded child-process adapter. The caller owns its output directory;
// cancellation returns false and closing AuTopsy terminates the child process.
bool RunBundledVideoTool(const std::vector<wxString> &arguments, wxWindow *parent,
   const wxString &title, const wxString &status, std::string *output = nullptr,
   VideoTool tool = VideoTool::Encoder, size_t maximumOutputBytes = 16384);
// Recording owns a newly created user media file. Stop closes the container;
// failure retains nonempty captured media and removes an empty owned output.
void RecordBundledVideoInput(const std::vector<wxString> &arguments,
   const wxString &destination, wxWindow *parent);
bool ExportMix(AudacityProject &project, const VideoTimeline::Timeline &timeline,
   double projectVideoDuration, const wxString &destination, bool overwrite, wxWindow *parent,
   const ExportSettings &settings = {}, double projectAudioStart = 0.0);
}
