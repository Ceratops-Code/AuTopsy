#include "RelinkVideoMedia.h"
#include "CreateVideoTrack.h"
#include "ExportVideoMix.h"
#include "PrepareVideoPreview.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectHistory.h"
#include "ProjectWindow.h"
#include "Viewport.h"
#include "CommandContext.h"
#include "MenuRegistry.h"
#include "CommonCommandFlags.h"
#include "SettingsVisitor.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include <rapidjson/document.h>
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <set>
namespace {
std::string Utf8(const wxString &text) {
   const auto bytes = text.ToUTF8(); return {bytes.data(), bytes.length()};
}
VideoTimeline::MediaSource InspectReplacement(const wxString &path, wxWindow *parent)
{
   std::string output;
   if (!VideoExport::RunBundledVideoTool({"-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height,avg_frame_rate,duration:stream_side_data=rotation:stream_tags=rotate:format=duration",
         "-of", "json", path}, parent, _("Relink Video Media"), _("Inspecting replacement video..."), &output, VideoExport::VideoTool::Probe))
      throw std::runtime_error("Relinking was cancelled.");
   rapidjson::Document data;
   if (data.Parse(output.c_str()).HasParseError() || !data.IsObject() ||
       !data.HasMember("streams") || !data["streams"].IsArray() || data["streams"].Empty())
      throw std::runtime_error("The replacement does not contain a readable video stream.");
   const auto &stream = data["streams"][0];
   VideoTimeline::MediaSource result; result.path = Utf8(path);
   if (stream.HasMember("width") && stream["width"].IsInt()) result.width = stream["width"].GetInt();
   if (stream.HasMember("height") && stream["height"].IsInt()) result.height = stream["height"].GetInt();
   if (stream.HasMember("avg_frame_rate") && stream["avg_frame_rate"].IsString()) {
      const std::string rate{stream["avg_frame_rate"].GetString()};
      const auto slash = rate.find('/');
      result.frameRate = std::stod(rate.substr(0, slash));
      if (slash != std::string::npos) result.frameRate /= std::stod(rate.substr(slash + 1));
   }
   const auto duration = [&](const auto &object) {
      return object.IsObject() && object.HasMember("duration") && object["duration"].IsString()
         ? std::stod(object["duration"].GetString()) : 0.0;
   };
   if (stream.HasMember("duration") && stream["duration"].IsString() &&
       std::string(stream["duration"].GetString()) != "N/A")
      result.duration = duration(stream);
   if (result.duration <= 0 && data.HasMember("format")) result.duration = duration(data["format"]);
   double rotation{};
   if (stream.HasMember("side_data_list") && stream["side_data_list"].IsArray())
      for (const auto &data : stream["side_data_list"].GetArray())
         if (data.HasMember("rotation") && data["rotation"].IsNumber()) rotation = data["rotation"].GetDouble();
   if (stream.HasMember("tags") && stream["tags"].IsObject() &&
       stream["tags"].HasMember("rotate") && stream["tags"]["rotate"].IsString())
      rotation = std::stod(stream["tags"]["rotate"].GetString());
   if (std::abs(std::remainder(rotation, 180.0)) > 45) std::swap(result.width, result.height);
   VideoTimeline::RequireUsableVideoSource(result);
   return result;
}
class RelinkVideoMediaCommand final : public AudacityCommand {
   wxString mOldPath, mFileName;
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
   { return XO("Locate moved video files while preserving edits and imported audio."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mOldPath, wxT("OldPath"), wxString{});
      visitor.Define(mFileName, wxT("Filename"), wxString{}); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      std::set<std::string> sources;
      for (const auto *track : VideoTimeline::ProjectVideoTracks(project))
         for (const auto &clip : track->Timeline().clips)
            sources.insert(VideoTimeline::SourceFor(track->Timeline(), clip).path);
      if (sources.empty()) return false;
      wxArrayString paths, labels;
      for (const auto &source : sources) {
         const auto path = wxString::FromUTF8(source); paths.Add(path);
         labels.Add((wxFileExists(path) ? wxString{} : _("Missing: ")) + path);
      }
      wxSingleChoiceDialog choice{&ProjectWindow::Get(project), _("Choose the video source to locate."),
         _("Relink Video Media"), labels};
      if (choice.ShowModal() != wxID_OK) return false;
      mOldPath = paths[choice.GetSelection()];
      wxFileDialog file{&ProjectWindow::Get(project), _("Locate replacement video"), {}, {},
         _("Video files|*.mp4;*.mkv;*.mov;*.avi;*.webm|All files|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST};
      if (file.ShowModal() != wxID_OK) return false;
      mFileName = file.GetPath(); return true;
   }
   bool Apply(const CommandContext &context) override {
      auto &project = context.project;
      try {
         if (ProjectAudioIO::Get(project).IsAudioActive())
            throw std::runtime_error("Stop playback or recording before relinking media.");
         if (mOldPath.empty() || mFileName.empty())
            throw std::runtime_error("Specify OldPath and the replacement Filename.");
         const auto tracks = VideoTimeline::ProjectVideoTracks(project);
         bool pending{};
         for (const auto *track : tracks) for (const auto &clip : track->Timeline().clips)
            pending |= VideoTimeline::SourceFor(track->Timeline(), clip).path == Utf8(mOldPath);
         if (!pending) return true;
         if (!wxFileExists(mFileName)) throw std::runtime_error("The replacement video is unavailable.");
         const auto source = InspectReplacement(mFileName, &ProjectWindow::Get(project));
         struct Change { AuTopsyVideoTrack *track; VideoTimeline::Timeline old, replacement; };
         std::vector<Change> changes;
         for (auto *track : tracks) {
            auto candidate = track->Timeline();
            if (VideoTimeline::ReplaceVideoSource(candidate, Utf8(mOldPath), source))
               changes.push_back({track, track->Timeline(), std::move(candidate)});
         }
         // Prepare the replacement before publishing project references. A
         // relocated large source needs a proxy under its new cache identity.
         const bool lightweight = static_cast<double>(source.width) * source.height > 1280.0 * 720.0 ||
            source.frameRate > 60;
         const auto prepared = VideoPreview::PrepareVideoMedia(
            mFileName, source.duration, lightweight, &ProjectWindow::Get(project));
         if (!prepared) throw std::runtime_error("Relinking was cancelled.");
         try {
            for (auto &change : changes) change.track->SetTimeline(std::move(change.replacement));
            ProjectHistory::Get(project).PushState(XO("Relinked video media"), XO("Relink Video"));
         }
         catch (...) {
            for (auto &change : changes) change.track->SetTimeline(std::move(change.old));
            throw;
         }
         VideoTimeline::ReloadProjectVideoPreview(project);
         Viewport::Get(project).Redraw(); return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol RelinkVideoMediaCommand::Symbol{XO("RelinkVideoMedia")};
BuiltinCommandsModule::Registration<RelinkVideoMediaCommand> sRelink;
using namespace MenuRegistry;
AttachedItem sRelinkMenu{
   Section("RelinkVideo", Command("RelinkVideoMedia", XXO("Relink Video Media..."),
      CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())),
   wxT("File/Import-Export")
};
}
