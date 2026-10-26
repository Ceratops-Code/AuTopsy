#include "RecordVideoInput.h"
#include "../video/ExportVideoMix.h"
#include "../video/CreateVideoTrack.h"
#include "commands/AudacityCommand.h"
#include "commands/LoadCommands.h"
#include "AudioIO.h"
#include "CommandContext.h"
#include "CommonCommandFlags.h"
#include "TempDirectory.h"
#include "MenuRegistry.h"
#include "ProjectWindow.h"
#include "SettingsVisitor.h"
#include "ViewInfo.h"
#include "commands/CommandDispatch.h"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/msw/wrapwin.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace VideoRecording {
namespace {
namespace fs = std::filesystem;
std::string Utf8(const wxString &value) {
   const auto bytes = value.ToUTF8(); return {bytes.data(), bytes.length()};
}
struct DisplaySource { std::wstring id; ScreenRegion region; bool primary; };
std::vector<DisplaySource> DisplaySources() {
   // GDI capture consumes physical pixels. Ask under a per-monitor DPI context
   // rather than feeding scaled wx coordinates to FFmpeg on mixed-DPI desktops.
   const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
   std::vector<DisplaySource> result;
   const auto callback = [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
      MONITORINFOEXW info{}; info.cbSize = sizeof(info);
      if (!GetMonitorInfoW(monitor, &info)) return FALSE;
      auto &list = *reinterpret_cast<std::vector<DisplaySource>*>(data);
      list.push_back({info.szDevice, {info.rcMonitor.left, info.rcMonitor.top,
         info.rcMonitor.right-info.rcMonitor.left, info.rcMonitor.bottom-info.rcMonitor.top},
         bool(info.dwFlags & MONITORINFOF_PRIMARY)});
      return TRUE;
   };
   const auto succeeded = EnumDisplayMonitors(nullptr, nullptr, callback, reinterpret_cast<LPARAM>(&result));
   if (previous) SetThreadDpiAwarenessContext(previous);
   if (!succeeded) throw std::runtime_error("Windows could not list recording displays.");
   return result;
}

bool IsPrivateGeneratedInput(const RecordingOptions &options) {
   wxString enabled;
   if (!wxGetEnv("AUTOPSY_TEST_VIDEO_RECORDING_INPUT", &enabled) || enabled != "generated") return false;
   const auto audio = fs::canonical(fs::path(TempDirectory::TempDir().ToStdWstring()));
   std::ifstream marker{audio.parent_path() / "_owner", std::ios::binary};
   std::string signature{std::istreambuf_iterator<char>{marker}, {}};
   if (audio.filename() != "_audio_temp" || signature != "AuTopsy behavior tests 1") return false;
   const auto relative = fs::weakly_canonical(options.destination).lexically_relative(audio.parent_path());
   return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

// All choices are frozen before capture; the selected display must still exist
// under the same identity when Apply runs. No disconnected-display fallback.
class RecordVideoCommand final : public AudacityCommand {
   wxString mFilename, mSource{"Webcam"}, mCamera, mAudio, mDisplay, mQuality{"720p"};
   double mDuration{}, mFrameRate{30};
   bool mPointer{true}, mGeneratedAudio{true};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
   { return XO("Record a camera or screen into new linked video and audio clips at the cursor."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mFilename, wxT("Filename"), wxString{});
      v.Define(mSource, wxT("Source"), wxString{"Webcam"});
      v.Define(mCamera, wxT("Camera"), wxString{}); v.Define(mAudio, wxT("Audio"), wxString{});
      v.Define(mDisplay, wxT("Display"), wxString{}); v.Define(mQuality, wxT("Quality"), wxString{"720p"});
      v.Define(mDuration, wxT("Duration"), 0.0, std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
      v.Define(mFrameRate, wxT("FrameRate"), 30.0, std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
      v.Define(mPointer, wxT("Pointer"), true); v.Define(mGeneratedAudio, wxT("GeneratedAudio"), true);
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      auto &parent = ProjectWindow::Get(project);
      try {
         std::string listing;
         if (!VideoExport::RunBundledVideoTool({"-hide_banner", "-sources", "dshow"}, &parent,
             _("Record Video"), _("Reading cameras and audio inputs..."), &listing,
             VideoExport::VideoTool::Encoder, 512*1024)) return false;
         const auto devices = ReadCaptureDeviceList(listing);
         const auto displays = DisplaySources();
         std::vector<std::string> cameras, microphones;
         wxDialog dialog{&parent, wxID_ANY, _("Record Video"), wxDefaultPosition, wxDefaultSize,
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER};
         auto *outer = new wxBoxSizer(wxVERTICAL);
         auto *grid = new wxFlexGridSizer(2, 8, 12); grid->AddGrowableCol(1);
         const auto choice = [&](const wxString &label) {
            grid->Add(new wxStaticText{&dialog, wxID_ANY, label}, 0, wxALIGN_CENTER_VERTICAL);
            auto *control = new wxChoice{&dialog, wxID_ANY}; grid->Add(control, 1, wxEXPAND); return control;
         };
         auto *source = choice(_("Source"));
         source->Append(_("Webcam")); source->Append(_("Screen")); source->Append(_("AuTopsy window"));
         source->SetSelection(0);
         auto *camera = choice(_("Camera")); auto *display = choice(_("Display"));
         auto *audio = choice(_("Audio input")); audio->Append(_("No audio")); audio->SetSelection(0);
         for (const auto &device : devices) {
            if (device.kind == DeviceKind::Video) { cameras.push_back(device.identifier); camera->Append(wxString::FromUTF8(device.label)); }
            if (device.kind == DeviceKind::Audio) { microphones.push_back(device.identifier); audio->Append(wxString::FromUTF8(device.label)); }
         }
         if (!cameras.empty()) camera->SetSelection(0);
         for (const auto &item : displays) {
            const auto label = wxString{item.id} + wxString::Format(" (%d x %d)", item.region.width, item.region.height);
            display->Append(label + (item.primary ? _(" â€” main display") : wxString{}));
         }
         if (!displays.empty()) display->SetSelection(0);
         auto *quality = choice(_("Maximum picture size"));
         quality->Append("720p"); quality->Append("1080p"); quality->Append(_("Original")); quality->SetSelection(0);
         auto *fps = choice(_("Frames per second"));
         for (const auto *value : {"24", "25", "30", "60"}) fps->Append(value);
         fps->SetSelection(2);
         grid->Add(new wxStaticText{&dialog, wxID_ANY, _("Duration in seconds (0 = until stopped)")}, 0, wxALIGN_CENTER_VERTICAL);
         auto *duration = new wxSpinCtrl{&dialog, wxID_ANY}; duration->SetRange(0, 86400); grid->Add(duration, 1, wxEXPAND);
         auto *pointer = new wxCheckBox{&dialog, wxID_ANY, _("Include mouse pointer in screen recording")}; pointer->SetValue(true);
         const auto enable = [&] { camera->Enable(source->GetSelection() == 0); display->Enable(source->GetSelection() == 1); pointer->Enable(source->GetSelection() != 0); };
         source->Bind(wxEVT_CHOICE, [&](wxCommandEvent &) { enable(); }); enable();
         outer->Add(grid, 1, wxEXPAND | wxALL, 14); outer->Add(pointer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
         auto *note = new wxStaticText{&dialog, wxID_ANY,
            _("The recording is saved to a new media file, then added at the cursor.\nChoose a microphone or a listed loopback input to capture audio.\nStop and Add finishes recording. Existing files will not be overwritten.")};
         outer->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
         outer->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
         dialog.FindWindow(wxID_OK)->SetLabel(_("Choose File and Record..."));
         dialog.SetSizerAndFit(outer); dialog.SetMinSize(dialog.GetSize()); dialog.CentreOnParent();
         if (dialog.ShowModal() != wxID_OK) return false;
         const int selected = source->GetSelection();
         mSource = selected == 0 ? "Webcam" : selected == 1 ? "Screen" : "AuTopsyWindow";
         if (selected == 0) {
            if (camera->GetSelection() == wxNOT_FOUND) throw std::runtime_error("No camera is available. Choose screen recording or connect a camera.");
            mCamera = wxString::FromUTF8(cameras.at(camera->GetSelection()));
         }
         if (selected == 1) {
            if (display->GetSelection() == wxNOT_FOUND) throw std::runtime_error("No recording display is available.");
            mDisplay = displays.at(display->GetSelection()).id;
         }
         mAudio = audio->GetSelection() > 0 ? wxString::FromUTF8(microphones.at(audio->GetSelection()-1)) : wxString{};
         mQuality = quality->GetSelection() == 0 ? "720p" : quality->GetSelection() == 1 ? "1080p" : "Original";
         fps->GetStringSelection().ToDouble(&mFrameRate); mDuration = duration->GetValue(); mPointer = pointer->GetValue();
         wxFileDialog file{&parent, _("Save Recording To a New File"), wxStandardPaths::Get().GetDocumentsDir(),
            "AuTopsy Recording " + wxDateTime::Now().Format("%Y-%m-%d %H-%M-%S") + ".mkv",
            _("Matroska video (*.mkv)|*.mkv|MP4 video (*.mp4)|*.mp4"), wxFD_SAVE};
         if (file.ShowModal() != wxID_OK) return false;
         mFilename = file.GetPath(); return true;
      }
      catch (const std::exception &error) { wxMessageBox(wxString::FromUTF8(error.what()), _("Record Video"), wxOK | wxICON_ERROR, &parent); return false; }
   }
   bool Apply(const CommandContext &context) override {
      try {
         RecordingOptions options;
         options.destination = fs::path(mFilename.ToStdWstring());
         options.projectStart = ViewInfo::Get(context.project).selectedRegion.t0();
         options.camera = Utf8(mCamera); options.audio = Utf8(mAudio);
         options.duration = mDuration; options.frameRate = mFrameRate; options.includePointer = mPointer;
         options.generatedAudio = mGeneratedAudio;
         if (mQuality == "1080p") { options.maximumWidth = 1920; options.maximumHeight = 1080; }
         else if (mQuality == "Original") options.maximumWidth = options.maximumHeight = 0;
         else if (mQuality != "720p") throw std::invalid_argument("Choose 720p, 1080p or Original recording quality.");
         if (mSource == "Webcam") options.source = SourceKind::Webcam;
         else if (mSource == "Screen") {
            options.source = SourceKind::Screen; bool found{};
            for (const auto &display : DisplaySources()) if (display.id == mDisplay.ToStdWstring()) { options.region = display.region; found = true; break; }
            if (!found) throw std::runtime_error("The selected recording display is unavailable. Choose a display again.");
         }
         else if (mSource == "AuTopsyWindow") {
            options.source = SourceKind::Window;
            options.window = reinterpret_cast<uintptr_t>(ProjectWindow::Get(context.project).GetHandle());
         }
         else if (mSource == "GeneratedTest") options.source = SourceKind::GeneratedTest;
         else throw std::invalid_argument("Choose Webcam, Screen or AuTopsyWindow recording.");
         return RecordVideoInput(context.project, options);
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol RecordVideoCommand::Symbol{XO("RecordVideo")};
BuiltinCommandsModule::Registration<RecordVideoCommand> sRecordVideoCommand;
MenuRegistry::AttachedItem sRecordVideoMenu{
   MenuRegistry::Command("RecordVideo", XXO("Record &Video..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
   wxT("Transport/Basic/Record")
};
}

bool RecordVideoInput(AudacityProject &project, const RecordingOptions &options) {
   if (AudioIO::Get()->IsBusy()) throw std::runtime_error("Stop playback or recording before starting video recording.");
   const auto args = PrepareVideoRecordingArguments(options,
      options.source == SourceKind::GeneratedTest && IsPrivateGeneratedInput(options));
   std::vector<wxString> native; for (const auto &argument : args) native.push_back(wxString::FromUTF8(argument));
   VideoExport::RecordBundledVideoInput(native, wxString{options.destination.wstring()}, &ProjectWindow::Get(project));
   // A failed import keeps the completed user recording. No temporary recovery
   // project, hidden media directory or second Undo transaction is produced.
   return VideoTimeline::ImportRecordedVideoAtCursor(project, wxString{options.destination.wstring()}, options.projectStart);
}
}
