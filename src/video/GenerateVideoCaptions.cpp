#include "GenerateVideoCaptions.h"
#include "EditCaptionCues.h"
#include "PrepareCaptionModels.h"
#include "PrepareVideoPreview.h"
#include "EditVideoComposition.h"
#include "CreateVideoTrack.h"
#include "ExportVideoMix.h"
#include "RenderVideoComposition.h"
#include "PlanVideoExport.h"
#include "Project.h"
#include "ProjectWindow.h"
#include "ProjectAudioIO.h"
#include "TrackFocus.h"
#include "ViewInfo.h"
#include "WaveTrack.h"
#include "WaveClip.h"
#include "Export.h"
#include "ExportPluginRegistry.h"
#include "Uuid.h"
#include "CommandContext.h"
#include "MenuRegistry.h"
#include "CommonCommandFlags.h"
#include "SettingsVisitor.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include <sndfile.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <filesystem>
#include <fstream>

namespace VideoCaptions {
namespace {
struct SourceRange {
   AuTopsyVideoTrack *parent{};
   WaveTrack *audio{};
   double from{}, to{};
};
void RequireStopped(AudacityProject &project)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before working with captions.");
}
Track *ChooseTrack(AudacityProject &project, int index)
{
   if (index < 0) {
      if (auto *focused = TrackFocus::Get(project).Get()) return focused;
      Track *selected{};
      for (auto *track : TrackList::Get(project).Any<Track>()) if (track->GetSelected()) {
         if (selected) throw std::runtime_error("Focus one audio or video clip for captions.");
         selected = track;
      }
      return selected;
   }
   int i{}; for (auto *track : TrackList::Get(project).Any<Track>()) if (i++ == index) return track;
   throw std::runtime_error("The caption source track no longer exists.");
}
AuTopsyVideoTrack *FindCaptionParent(AudacityProject &project, Track *source, double at)
{
   if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(source)) {
      if (video->Timeline().placement.parentId.empty()) return video;
      for (auto *candidate : VideoTimeline::ProjectVideoTracks(project))
         if (candidate->Timeline().placement.id == video->Timeline().placement.parentId) return candidate;
      throw std::runtime_error("The caption's parent video is missing.");
   }
   AuTopsyVideoTrack *parent{};
   for (auto *candidate : VideoTimeline::ProjectVideoTracks(project)) {
      if (!candidate->Timeline().placement.parentId.empty() || !candidate->VideoVisible() ||
          !VideoTimeline::ClipAt(candidate->Timeline(), at)) continue;
      if (parent) throw std::runtime_error("Several videos cover this time. Focus the video that should receive captions.");
      parent = candidate;
   }
   return parent;
}
SourceRange GetSourceRange(AudacityProject &project, Track *source)
{
   RequireStopped(project);
   if (!source || source->GetOwner().get() != &TrackList::Get(project))
      throw std::runtime_error("Click an audio or video clip before generating captions.");
   const auto &selection = ViewInfo::Get(project).selectedRegion;
   SourceRange result; const auto at = selection.t0();
   if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(source)) {
      const auto *clip = VideoTimeline::ClipAt(video->Timeline(), at);
      if (!clip) throw std::runtime_error("Put the cursor inside the video clip to caption.");
      result.from = clip->start; result.to = clip->End();
   }
   else if (auto *audio = dynamic_cast<WaveTrack*>(source)) {
      const auto clip = audio->GetClipAtTime(at);
      if (!clip) throw std::runtime_error("Put the cursor inside the audio clip to caption.");
      result.audio = audio; result.from = clip->GetPlayStartTime(); result.to = clip->GetPlayEndTime();
   }
   else throw std::runtime_error("Choose an audio or video clip for caption generation.");
   if (!selection.isPoint()) { result.from = std::max(result.from, selection.t0()); result.to = std::min(result.to, selection.t1()); }
   if (result.from < 0 || result.to-result.from < .001 || result.to-result.from > 21600)
      throw std::runtime_error("Select between one millisecond and six hours of an audio or video clip.");
   result.parent = FindCaptionParent(project, source, result.from);
   return result;
}
std::string ReadSubtitle(const wxString &path)
{
   const std::filesystem::path native{path.ToStdWstring()};
   if (!std::filesystem::is_regular_file(native) || std::filesystem::file_size(native) > MaximumCaptionFileBytes)
      throw std::runtime_error("Choose a subtitle file smaller than eight MiB.");
   std::ifstream file(native, std::ios::binary);
   if (!file) throw std::runtime_error("Cannot open the subtitle file for reading.");
   const auto size = std::filesystem::file_size(native);
   if (size > MaximumCaptionFileBytes) throw std::runtime_error("The subtitle file grew beyond its size limit.");
   std::string text(size, '\0'); file.read(text.data(), std::streamsize(size));
   if (file.gcount() != std::streamsize(size) || file.peek() != std::char_traits<char>::eof())
      throw std::runtime_error("The subtitle file changed while it was being read. Try importing the completed file.");
   if (file.bad() || (!text.empty() && wxString::FromUTF8(text.data(), text.size()).empty()))
      throw std::runtime_error("The subtitle file could not be read as UTF-8 text.");
   return text;
}
FileFormat SubtitleFormat(const wxString &path)
{
   const auto extension = wxFileName(path).GetExt().Lower();
   if (extension == "srt") return FileFormat::Srt;
   if (extension == "vtt") return FileFormat::WebVtt;
   throw std::runtime_error("Use an .srt or .vtt subtitle filename.");
}
void AddCaptionBatch(AudacityProject &project, AuTopsyVideoTrack *parent, const std::vector<Cue> &cues)
{
   if (cues.empty()) throw std::runtime_error("No spoken words or caption cues were found in this range.");
   const auto planned = CombineCaptionOverlaps(cues);
   for (auto *existing : VideoTimeline::ProjectVideoTracks(project)) {
      if (!existing->VideoVisible() || existing->Timeline().placement.parentId !=
            (parent ? parent->Timeline().placement.id : std::string{})) continue;
      const auto current = ReadTimelineCaptions(existing->Timeline());
      if (current.size() == planned.size() && std::equal(current.begin(), current.end(), planned.begin(),
            [](const auto &a, const auto &b) { return a.startMs == b.startMs && a.endMs == b.endMs && a.text == b.text; })) return;
   }
   int width = 1280, height = 720;
   VideoEditing::VideoFrameRectangle box{.05, .73, .9, .24};
   if (parent && !parent->Timeline().clips.empty()) {
      const auto *picture = VideoTimeline::ClipAt(parent->Timeline(), cues.front().startMs/1000.0);
      if (!picture) picture = &parent->Timeline().clips.front();
      auto source = VideoTimeline::SourceFor(parent->Timeline(), *picture);
      if (source.width < 2 || source.height < 2) {
         const auto size = VideoExport::ReadVideoSourcePictureSize(wxString::FromUTF8(source.path), &ProjectWindow::Get(project));
         if (!size) throw VideoPreview::PreviewPreparationCancelled{};
         source.width = size->first; source.height = size->second;
      }
      const auto canvas = VideoExport::CalculateVideoExportCanvas(source.width, source.height, picture->crop);
      const auto raster = VideoEditing::CalculateVideoTextRasterSize(box, canvas.first, canvas.second);
      width = raster.width; height = raster.height;
   }
   VideoEditing::VideoTextAppearance style; style.heightFraction = .2;
   auto candidate = PlanCaptionClips(cues, width, height, style);
   candidate.placement.id = audacity::Uuid::Generate().ToHexString();
   if (parent) { candidate.placement.parentId = parent->Timeline().placement.id; candidate.placement.rectangle = box; }
   VideoEditing::AddProjectVideoTextTrack(project, parent, std::move(candidate), _("Captions"));
   VideoTimeline::ShowProjectVideoPreview(project);
}
class AudioProgress final : public ExportProcessorDelegate {
   wxProgressDialog mProgress;
   bool mCancelled{};
public:
   explicit AudioProgress(wxWindow *parent) : mProgress{_("Generate Captions"),
      _("Preparing the edited audio for local speech recognition..."), 1000, parent,
      wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_AUTO_HIDE} {}
   bool IsCancelled() const override { return mCancelled; }
   bool IsStopped() const override { return false; }
   void SetStatusString(const TranslatableString &) override {}
   void OnProgress(double value) override { mCancelled = !mProgress.Update(int(std::clamp(value, 0.0, 1.0)*1000)); }
};
void GenerateCaptions(AudacityProject &project, Track *track, const std::string &model, const std::string &language)
{
   ClipActions::MediaDialogCommandGuard commands;
   const auto source = GetSourceRange(project, track);
   if (TrackList::Get(project).Any<WaveTrack>().empty())
      throw std::runtime_error("The project needs an audio track containing the speech to caption.");
   CaptionWorkspace workspace;
   auto *window = &ProjectWindow::Get(project);
   const auto audio = workspace.File("_speech.wav");
   const auto [plugin, format] = ExportPluginRegistry::Get().FindFormat("WAV");
   if (!plugin) throw std::runtime_error("The WAV export module is unavailable.");
   // Native mixing is the single timing authority: trims, clip speed, project
   // effects and video offsets are already applied before recognition starts.
   struct Selection {
      std::vector<std::pair<Track*, bool>> tracks;
      ~Selection() { for (const auto &item : tracks) item.first->SetSelected(item.second); }
   };
   {
      Selection restore;
      if (source.audio) for (auto *candidate : TrackList::Get(project).Any<Track>()) {
         restore.tracks.push_back({candidate, candidate->GetSelected()}); candidate->SetSelected(candidate == source.audio);
      }
      auto task = ExportTaskBuilder{}.SetPlugin(plugin, format).SetFileName(wxFileName(audio))
         .SetRange(source.from, source.to, source.audio != nullptr).SetSampleRate(16000).SetNumChannels(1)
         .SetParameters({{SF_FORMAT_WAV, int(SF_FORMAT_PCM_16)}}).Build(project);
      AudioProgress progress{window}; auto result = task.get_future(); task(progress);
      if (result.get() != ExportResult::Success) throw VideoPreview::PreviewPreparationCancelled{};
   }
   // Empty audio cannot yield useful captions. Avoid hallucinated words and an
   // unnecessary model download for exact silence, including muted mixes.
   SF_INFO info{};
   const auto nativeAudio = audio.ToStdWstring();
   auto *sound = sf_wchar_open(nativeAudio.c_str(), SFM_READ, &info);
   if (!sound) throw std::runtime_error("Cannot read the prepared caption audio.");
   struct CloseSound { SNDFILE *file; ~CloseSound() { sf_close(file); } } close{sound};
   std::vector<float> samples(16000); double peak{};
   while (const auto count = sf_read_float(sound, samples.data(), samples.size()))
      for (sf_count_t i{}; i < count; ++i) peak = std::max(peak, std::abs(double(samples[size_t(i)])));
   if (peak < 1e-5) throw std::runtime_error("This range contains no audible audio to caption.");
   const auto weights = workspace.PrepareModel(model, window);
   const auto output = workspace.File("_recognized");
   std::string ignored;
   if (!VideoExport::RunBundledVideoTool({"-m", weights, "-f", audio, "-l", wxString::FromUTF8(language),
         "-ng", "-t", "4", "-osrt", "-of", output, "-ml", "80", "-sow"}, window,
         _("Generate Captions"), _("Recognizing speech locally. You can cancel without changing the project."),
         &ignored, VideoExport::VideoTool::Transcriber)) throw VideoPreview::PreviewPreparationCancelled{};
   const auto cues = ReadCaptionCues(ReadSubtitle(output+".srt"), FileFormat::Srt);
   AddCaptionBatch(project, source.parent, PlaceCaptionCues(cues, source.from, source.from, source.to));
}
struct CaptionOptions { std::string model{"base"}, language{"auto"}; };
bool ChooseCaptionOptions(AudacityProject &project, CaptionOptions &options)
{
   wxDialog dialog{&ProjectWindow::Get(project), wxID_ANY, _("Generate Captions")};
   auto *layout = new wxBoxSizer(wxVERTICAL);
   auto *intro = new wxStaticText(&dialog, wxID_ANY,
      _("Speech recognition runs on this computer. The selected model downloads once.\nVideo uses the audible project mix; an audio clip uses its own track.\nSelect a time range first to caption only that portion."));
   layout->Add(intro, 0, wxALL, 12);
   layout->Add(new wxStaticText(&dialog, wxID_ANY, _("Quality")), 0, wxLEFT|wxRIGHT, 12);
   auto *quality = new wxChoice(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize,
      wxArrayString{_("Fast (tiny, 74 MiB)"), _("Balanced (base, 141 MiB)"), _("Accurate (small, 465 MiB)")});
   quality->SetSelection(1); layout->Add(quality, 0, wxEXPAND|wxALL, 12);
   layout->Add(new wxStaticText(&dialog, wxID_ANY, _("Language code (auto, he, en, ru, ar, ...)")), 0, wxLEFT|wxRIGHT, 12);
   auto *language = new wxComboBox(&dialog, wxID_ANY, "auto", wxDefaultPosition, wxDefaultSize,
      wxArrayString{"auto", "he", "en", "ru", "ar", "uk", "es", "fr", "de", "pt", "ja", "zh"});
   layout->Add(language, 0, wxEXPAND|wxALL, 12);
   auto *buttons = dialog.CreateSeparatedButtonSizer(wxOK|wxCANCEL);
   if (auto *ok = dialog.FindWindow(wxID_OK)) ok->SetLabel(_("Generate"));
   layout->Add(buttons, 0, wxEXPAND|wxALL, 12); dialog.SetSizerAndFit(layout);
   if (dialog.ShowModal() != wxID_OK) return false;
   const char *models[] = {"tiny", "base", "small"}; options.model = models[quality->GetSelection()];
   options.language = std::string(language->GetValue().Lower().Trim().ToUTF8());
   if (options.language.empty() || options.language.size() > 16 || options.language.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") != std::string::npos)
      throw std::runtime_error("Use auto or a language code such as he, en or ru.");
   return true;
}
class GenerateCaptionsCommand final : public AudacityCommand {
   int mTrack{-1}; wxString mModel{"base"}, mLanguage{"auto"};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Generate editable captions from a selected audio or video clip using local speech recognition."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mTrack, wxT("Track"), -1, -1, 1000000); v.Define(mModel, wxT("Model"), wxString{"base"});
      v.Define(mLanguage, wxT("Language"), wxString{"auto"}); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      try {
         GetSourceRange(project, ChooseTrack(project, mTrack)); CaptionOptions options;
         if (!ChooseCaptionOptions(project, options)) return false;
         mModel = wxString::FromUTF8(options.model); mLanguage = wxString::FromUTF8(options.language); return true;
      } catch (const std::exception &error) { wxMessageBox(wxString::FromUTF8(error.what()), _("Captions"), wxOK|wxICON_ERROR, &ProjectWindow::Get(project)); return false; }
   }
   bool Apply(const CommandContext &context) override {
      try { GenerateCaptions(context.project, ChooseTrack(context.project, mTrack), std::string(mModel.ToUTF8()), std::string(mLanguage.ToUTF8())); return true; }
      catch (const VideoPreview::PreviewPreparationCancelled &) { return false; }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
class CaptionFileCommand : public AudacityCommand {
   wxString mFilename; int mTrack{-1}; double mAt{}; bool mOverwrite{};
protected:
   virtual bool Importing() const = 0;
public:
   TranslatableString GetDescription() const override { return XO("Import or export timed editable caption clips as SRT or WebVTT."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mFilename, wxT("Filename"), wxString{}); v.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      v.Define(mAt, wxT("At"), 0.0, 0.0, 100000000.0); v.Define(mOverwrite, wxT("Overwrite"), false); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      wxFileDialog dialog{&ProjectWindow::Get(project), Importing() ? _("Import Captions") : _("Export Captions"),
         {}, Importing() ? wxString{} : wxString{"captions.srt"}, _("Subtitle files (*.srt;*.vtt)|*.srt;*.vtt"),
         Importing() ? wxFD_OPEN|wxFD_FILE_MUST_EXIST : wxFD_SAVE|wxFD_OVERWRITE_PROMPT};
      if (dialog.ShowModal() != wxID_OK) return false;
      mFilename = dialog.GetPath(); mOverwrite = !Importing();
      if (Importing()) {
         wxDialog offset{&ProjectWindow::Get(project), wxID_ANY, _("Import Caption Timing")};
         auto *layout = new wxBoxSizer(wxVERTICAL);
         layout->Add(new wxStaticText(&offset, wxID_ANY,
            _("Add this offset to the subtitle times. Use 0 to keep project timecodes.\nFor a moved video clip, enter its start time.")), 0, wxALL, 12);
         auto *seconds = new wxSpinCtrlDouble(&offset, wxID_ANY, "0", wxDefaultPosition, wxDefaultSize,
            wxSP_ARROW_KEYS, 0, 100000000, 0, .001); seconds->SetDigits(3);
         layout->Add(seconds, 0, wxEXPAND|wxALL, 12);
         layout->Add(offset.CreateSeparatedButtonSizer(wxOK|wxCANCEL), 0, wxEXPAND|wxALL, 12);
         offset.SetSizerAndFit(layout);
         if (offset.ShowModal() != wxID_OK) return false;
         mAt = seconds->GetValue();
      }
      return true;
   }
   bool Apply(const CommandContext &context) override {
      try {
         ClipActions::MediaDialogCommandGuard commands; auto &project = context.project; RequireStopped(project);
         auto *track = ChooseTrack(project, mTrack);
         if (Importing()) {
            const auto cues = ReadCaptionCues(ReadSubtitle(mFilename), SubtitleFormat(mFilename));
            AddCaptionBatch(project, FindCaptionParent(project, track, mAt), PlaceCaptionCues(cues, mAt, 0, 100000000));
         }
         else {
            auto *video = dynamic_cast<AuTopsyVideoTrack*>(track);
            if (!video) throw std::runtime_error("Focus the caption track to export its subtitles.");
            const auto cues = ReadTimelineCaptions(video->Timeline());
            if (cues.empty()) throw std::runtime_error("This track has no visible caption clips to export.");
            VideoExport::WriteMediaTextFile(mFilename, WriteCaptionCues(cues, SubtitleFormat(mFilename)), mOverwrite);
         }
         return true;
      } catch (const VideoPreview::PreviewPreparationCancelled &) { return false; }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
class ImportCaptionsCommand final : public CaptionFileCommand {
   bool Importing() const override { return true; }
public: static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
class ExportCaptionsCommand final : public CaptionFileCommand {
   bool Importing() const override { return false; }
public: static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol GenerateCaptionsCommand::Symbol{XO("GenerateCaptions")};
const ComponentInterfaceSymbol ImportCaptionsCommand::Symbol{XO("ImportCaptions")};
const ComponentInterfaceSymbol ExportCaptionsCommand::Symbol{XO("ExportCaptions")};
BuiltinCommandsModule::Registration<GenerateCaptionsCommand> sGenerateCaptions;
BuiltinCommandsModule::Registration<ImportCaptionsCommand> sImportCaptions;
BuiltinCommandsModule::Registration<ExportCaptionsCommand> sExportCaptions;
using namespace MenuRegistry;
AttachedItem sCaptionMenu{Menu("Captions", XXO("&Captions"),
   Command("GenerateCaptions", XXO("&Generate Captions..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()|TracksExistFlag()),
   Command("ImportCaptions", XXO("&Import SRT / VTT..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
   Command("ExportCaptions", XXO("&Export SRT / VTT..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()|TracksExistFlag())), wxT("Tracks")};
}
void ShowGenerateCaptions(AudacityProject &project, Track *source)
{
   ClipActions::MediaDialogCommandGuard commands;
   try {
      if (!source) source = ChooseTrack(project, -1);
      GetSourceRange(project, source); CaptionOptions options;
      if (ChooseCaptionOptions(project, options)) GenerateCaptions(project, source, options.model, options.language);
   } catch (const VideoPreview::PreviewPreparationCancelled &) {}
   catch (const std::exception &error) { wxMessageBox(wxString::FromUTF8(error.what()), _("Captions"), wxOK|wxICON_ERROR, &ProjectWindow::Get(project)); }
}
}
