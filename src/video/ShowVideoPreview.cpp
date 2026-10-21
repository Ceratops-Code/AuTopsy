/**********************************************************************

  Audacity: A Digital Audio Editor

  ShowVideoPreview.cpp

  A project-owned video preview. libmpv owns decoding and its
  native child window; Audacity's GUI timer follows the audio playback clock.
   No video work runs in an audio callback. The existing importer adds the
   soundtrack as ordinary editable audio; clip timing lives in EditVideoTimeline.

**********************************************************************/

#include "SyncVideoPlayback.h"
#include "ExportVideoMix.h"
#include "EditVideoTimeline.h"
#include "PrepareVideoPreview.h"
#include "ShowVideoTrack.h"

#include "AudioIO.h"
#include "CommandContext.h"
#include "MenuRegistry.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "Track.h"
#include "TrackFocus.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "../CommonCommandFlags.h"
#include "../ProjectAudioManager.h"
#include "../ProjectFileManager.h"
#include "../ProjectWindow.h"
#include "../commands/AudacityCommand.h"
#include "../commands/CommandDispatch.h"
#include "../commands/LoadCommands.h"
#include "SettingsVisitor.h"

#include <mpv/client.h>
#include <wx/filedlg.h>
#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/stopwatch.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <wx/msw/wrapwin.h>

#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "Export.h"
#include <wx/filefn.h>
#include <algorithm>
#include <cmath>
#include <vector>

#include <wx/log.h>
#include <cstring>
#include <deque>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {

std::string Utf8(const wxString &text)
{
   const auto bytes = text.ToUTF8();
   return { bytes.data(), bytes.length() };
}




struct UnloadLibrary {
   void operator()(void *handle) const
   {
      if (handle)
         FreeLibrary(static_cast<HMODULE>(handle));
   }
};

//! Load only the build's own player DLL, with safe dependency search rules.
class MpvApi final {
   std::unique_ptr<void, UnloadLibrary> mLibrary;

   template<typename Function>
   Function Symbol(const char *name)
   {
      auto address = GetProcAddress(static_cast<HMODULE>(mLibrary.get()), name);
      if (!address)
         throw std::runtime_error{ std::string{ "libmpv is missing " } + name };
      return reinterpret_cast<Function>(address);
   }

public:
   decltype(&mpv_client_api_version) version{};
   decltype(&mpv_create) create{};
   decltype(&mpv_initialize) initialize{};
   decltype(&mpv_terminate_destroy) destroy{};
   decltype(&mpv_set_option_string) option{};
   decltype(&mpv_set_option) typedOption{};
   decltype(&mpv_get_property) get{};
   decltype(&mpv_set_property) set{};
   decltype(&mpv_command) command{};
   decltype(&mpv_wait_event) wait{};
   decltype(&mpv_error_string) error{};
   decltype(&mpv_free) free{};

   MpvApi()
   {
      wxFileName library{ wxStandardPaths::Get().GetExecutablePath() };
      library.SetFullName("libmpv-2.dll");
      mLibrary.reset(LoadLibraryExW(library.GetFullPath().wc_str(), nullptr,
         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
      if (!mLibrary)
         throw std::runtime_error{ "Cannot load " + Utf8(library.GetFullPath()) +
            " (Windows error " + std::to_string(GetLastError()) + ")." };

      version = Symbol<decltype(version)>("mpv_client_api_version");
      create = Symbol<decltype(create)>("mpv_create");
      initialize = Symbol<decltype(initialize)>("mpv_initialize");
      destroy = Symbol<decltype(destroy)>("mpv_terminate_destroy");
      option = Symbol<decltype(option)>("mpv_set_option_string");
      typedOption = Symbol<decltype(typedOption)>("mpv_set_option");
      get = Symbol<decltype(get)>("mpv_get_property");
      set = Symbol<decltype(set)>("mpv_set_property");
      command = Symbol<decltype(command)>("mpv_command");
      wait = Symbol<decltype(wait)>("mpv_wait_event");
      error = Symbol<decltype(error)>("mpv_error_string");
      free = Symbol<decltype(free)>("mpv_free");
      if ((version() >> 16) != (MPV_CLIENT_API_VERSION >> 16))
         throw std::runtime_error{ "The libmpv DLL and development headers "
            "have incompatible client API versions." };
   }

   void Check(int result, const char *operation) const
   {
      if (result < 0)
         throw std::runtime_error{ std::string{ operation } + ": " + error(result) };
   }
};

class MpvPlayer final {
   // Member order ensures the player is destroyed before its DLL is unloaded.
   MpvApi mApi;
   std::unique_ptr<mpv_handle, decltype(&mpv_terminate_destroy)>
      mHandle{ nullptr, nullptr };
   bool mLoaded{};

   std::optional<double> Number(const char *name)
   {
      double value{};
      const int result = mApi.get(mHandle.get(), name, MPV_FORMAT_DOUBLE, &value);
      if (result == MPV_ERROR_PROPERTY_UNAVAILABLE)
         return {};
      mApi.Check(result, name);
      return value;
   }

   bool Flag(const char *name)
   {
      int value{};
      mApi.Check(mApi.get(mHandle.get(), name, MPV_FORMAT_FLAG, &value), name);
      return value != 0;
   }

   void ReadEvents()
   {
      // A bounded drain prevents a busy decoder from starving the GUI loop.
      for (int i = 0; i != 256; ++i) {
         const auto *event = mApi.wait(mHandle.get(), 0.0);
         if (event->event_id == MPV_EVENT_NONE)
            return;
         if (event->event_id == MPV_EVENT_FILE_LOADED) {
            int64_t videoId{};
            const int result = mApi.get(mHandle.get(), "vid",
               MPV_FORMAT_INT64, &videoId);
            if (result < 0 || videoId <= 0)
               throw std::runtime_error{ "The selected file has no playable video stream." };
            mLoaded = true;
         }
         else if (event->event_id == MPV_EVENT_END_FILE) {
            const auto *end = static_cast<const mpv_event_end_file *>(event->data);
            if (end && end->reason == MPV_END_FILE_REASON_ERROR)
               mApi.Check(end->error, "Loading video");
         }
         else if (event->event_id == MPV_EVENT_SHUTDOWN)
            throw std::runtime_error{ "The video player shut down unexpectedly." };
      }
   }

public:
   MpvPlayer(wxWindow &surface, const wxString &path)
   {
      mHandle.get_deleter() = mApi.destroy;
      mHandle.reset(mApi.create());
      if (!mHandle)
         throw std::runtime_error{ "Cannot create the video player." };

      const std::pair<const char *, const char *> options[] = {
         { "config", "no" }, { "load-scripts", "no" }, { "ytdl", "no" },
         { "audio", "no" }, { "sub", "no" }, { "terminal", "no" },
         { "input-default-bindings", "no" }, { "input-vo-keyboard", "no" },
         { "input-cursor", "no" }, { "idle", "yes" },
         { "osc", "no" }, { "osd-level", "0" },
         { "gpu-shader-cache", "no" }, { "cache-on-disk", "no" },
         { "save-position-on-quit", "no" },
         { "keep-open", "yes" }, { "pause", "yes" }, { "hr-seek", "yes" }
         , { "hwdec", "auto" }, { "profile", "fast" }, { "gpu-context", "d3d11" }
      };
      for (const auto &entry : options)
         mApi.Check(mApi.option(mHandle.get(), entry.first, entry.second), entry.first);

      // mpv's Win32 embedding contract requires an unsigned 32-bit HWND value.
      const auto window = static_cast<uint32_t>(
         reinterpret_cast<uintptr_t>(surface.GetHandle()));
      int64_t windowId = window;
      mApi.Check(mApi.typedOption(mHandle.get(), "wid", MPV_FORMAT_INT64, &windowId),
         "Setting video window");
      mApi.Check(mApi.initialize(mHandle.get()), "Initializing video player");

      const auto filename = Utf8(path);
      const char *arguments[]{ "loadfile", filename.c_str(), "replace", nullptr };
      mApi.Check(mApi.command(mHandle.get(), arguments), "Opening video");
   }

   VideoPreview::PlayerSnapshot ReadState()
   {
      ReadEvents();
      VideoPreview::PlayerSnapshot result;
      if (!mLoaded)
         return result;
      const auto position = Number("time-pos");
      const auto duration = Number("duration");
      if (!position || !duration)
         return result;
      result.ready = true;
      result.paused = Flag("pause");
      result.seeking = Flag("seeking");
      result.position = *position;
      result.duration = *duration;
      result.frameRate = Number("container-fps").value_or(30.0);
      result.speed = Number("speed").value_or(1.0);
      return result;
   }

   bool HasAudio()
   {
      // Audio output is disabled in mpv, so query the stream list rather than
      // the selected audio ID. Silent videos are still valid references.
      int64_t count{};
      mApi.Check(mApi.get(mHandle.get(), "track-list/count", MPV_FORMAT_INT64,
         &count), "Reading video streams");
      for (int64_t i = 0; i < count; ++i) {
         const auto property = "track-list/" + std::to_string(i) + "/type";
         char *type{};
         mApi.Check(mApi.get(mHandle.get(), property.c_str(), MPV_FORMAT_STRING,
            &type), "Reading stream type");
         const std::unique_ptr<char, decltype(&mpv_free)> value{ type, mApi.free };
         if (value && std::string{ value.get() } == "audio")
            return true;
      }
      return false;
   }

   double Property(const char *name) { return Number(name).value_or(0.0); }

   void SetPaused(bool value)
   {
      int paused = value;
      mApi.Check(mApi.set(mHandle.get(), "pause", MPV_FORMAT_FLAG, &paused),
         "Pausing/resuming video");
   }

   void SetSpeed(double value)
   {
      mApi.Check(mApi.set(mHandle.get(), "speed", MPV_FORMAT_DOUBLE, &value),
         "Setting video speed");
   }

   void Seek(double position)
   {
      std::ostringstream number;
      number.imbue(std::locale::classic());
      number << std::setprecision(17) << position;
      const auto text = number.str();
      const char *arguments[]{ "seek", text.c_str(), "absolute+exact", nullptr };
      mApi.Check(mApi.command(mHandle.get(), arguments), "Seeking video");
   }
};

class VideoPreviewFrame final : public wxFrame {
   std::weak_ptr<AudacityProject> mProject;
   wxTimer mTimer{ this };
   wxStopWatch mLoadTime;
   wxPanel *mVideoArea{};
   wxPanel *mSurface{};
   wxStaticText *mStatus{};
   std::unique_ptr<MpvPlayer> mPlayer;
   Observer::Subscription mProjectClosing;
   std::shared_ptr<const VideoPreview::PreparedMedia> mMedia;
   double mSourceDuration{}, mSourceFrameRate{30};
   bool mSourceHasAudio{}, mDurationChecked{};
   struct PlaybackSample { double audio, video, dropped, decoderDropped; bool paused; };
   std::deque<PlaybackSample> mPlaybackSamples;
   long mLastSampleTime{};
   wxString mPath;
   bool mWasPlaying{};
   double mProjectTime{};

   void OnClose(wxCloseEvent &event)
   {
      if (event.CanVeto()) {
         Hide();
         event.Veto();
      }
      else
         event.Skip();
   }

   void OnTimer(wxTimerEvent &)
   {
      if (IsBeingDeleted()) {
         mTimer.Stop();
         return;
      }
      const auto project = mProject.lock();
      if (!project || ProjectWindow::Get(*project).IsBeingDeleted()) {
         mTimer.Stop();
         Destroy();
         return;
      }
      if (!mPlayer)
         return;
      try {
         auto *audio = AudioIO::Get();
         const bool ownsPlayback = audio &&
            ProjectAudioIO::Get(*project).IsAudioActive() &&
            ProjectAudioManager::Get(*project).Playing();
         const auto transport = VideoPreview::TransportSnapshot::ForProjectPlayback(
            ownsPlayback, ownsPlayback && audio->IsPaused(),
            ownsPlayback ? audio->GetStreamTime() : 0.0,
            ViewInfo::Get(*project).selectedRegion.t0());
         if (transport.playing && !mWasPlaying && !IsShown())
            ShowWithoutActivating();
         if (transport.playing && !mWasPlaying) {
            mPlaybackSamples.clear(); mLastSampleTime = 0;
         }
         mWasPlaying = transport.playing;

          const auto &timeline = VideoTimeline::GetProjectTimeline(*project);
          const auto *clip = VideoTimeline::ClipAt(timeline, transport.position);
          const auto picturePosition = VideoTimeline::PictureAt(timeline, transport.position);
          mProjectTime = transport.position;
          if (mSurface->IsShown() != (clip != nullptr)) {
             mSurface->Show(clip != nullptr);
             mVideoArea->Layout();
          }
          if (clip) {
             auto picture = transport;
             picture.position = picturePosition.sourceTime;
             VideoPreview::SynchronizeVideoPlayback(*mPlayer, picture);
          }
          else if (!mPlayer->ReadState().paused) mPlayer->SetPaused(true);
         const auto video = mPlayer->ReadState();
         if (transport.playing && video.ready && mLoadTime.Time() - mLastSampleTime >= 100) {
            mPlaybackSamples.push_back({transport.position, video.position,
               mPlayer->Property("frame-drop-count"), mPlayer->Property("decoder-frame-drop-count"), video.paused});
            if (mPlaybackSamples.size() > 256) mPlaybackSamples.pop_front();
            mLastSampleTime = mLoadTime.Time();
         }
         if (video.ready && !mDurationChecked) {
            if (std::abs(video.duration - mSourceDuration) > std::max(0.1, 2.0 / mSourceFrameRate))
               throw std::runtime_error("The source video duration changed. Import the video again.");
            mDurationChecked = true;
         }
         if (!video.ready && mLoadTime.Time() > 15000)
            throw std::runtime_error{ "The video did not become ready within 15 seconds." };
          mStatus->SetLabel(!clip ? wxString::Format(_("Video gap  |  AuTopsy %.2f s"), transport.position) : video.ready
             ? wxString::Format(_("Video %.2f s  |  AuTopsy %.2f s"),
               video.position, transport.position)
            : _("Loading video..."));
      }
      catch (const std::exception &error) {
         std::fprintf(stderr, "AuTopsy preview timer: %s\n", error.what());
         // Destroying the decoder also stops video. Audio is never stopped here.
         mTimer.Stop();
         mPlayer.reset();
         mStatus->SetLabel(_("Video preview unavailable"));
         wxMessageBox(mPath + "\n\n" + wxString::FromUTF8(error.what()),
            _("Video Preview"), wxOK | wxICON_ERROR, this);
      }
   }

public:
   explicit VideoPreviewFrame(AudacityProject &project)
      : wxFrame{ &ProjectWindow::Get(project), wxID_ANY, _("Video Preview"),
           wxDefaultPosition, wxSize{ 640, 400 },
           wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT | wxFRAME_NO_TASKBAR }
      , mProject{ project.shared_from_this() }
   {
      auto *layout = new wxBoxSizer{ wxVERTICAL };
       mVideoArea = new wxPanel{ this };
       mVideoArea->SetBackgroundColour(*wxBLACK);
       mSurface = new wxPanel{ mVideoArea };
       mSurface->SetBackgroundColour(*wxBLACK);
       auto *pictureLayout = new wxBoxSizer(wxVERTICAL);
       pictureLayout->Add(mSurface, 1, wxEXPAND);
       mVideoArea->SetSizer(pictureLayout);
       mStatus = new wxStaticText{ this, wxID_ANY, _("Loading video...") };
       layout->Add(mVideoArea, 1, wxEXPAND);
      layout->Add(mStatus, 0, wxEXPAND | wxALL, 6);
      SetSizer(layout);
      SetMinSize(wxSize{ 320, 200 });
      Bind(wxEVT_TIMER, &VideoPreviewFrame::OnTimer, this, mTimer.GetId());
      Bind(wxEVT_CLOSE_WINDOW, &VideoPreviewFrame::OnClose, this);
      // Shut down mpv while its HWND is still alive. A project closes its
      // children before releasing attachments, so attachment destruction alone
      // is too late for a renderer that owns native child windows.
      mProjectClosing = ProjectWindow::Get(project).Subscribe(
         [this](const ProjectWindowDestroyedMessage &) {
            mTimer.Stop();
            mPlayer.reset();
            Destroy();
         });
   }

   ~VideoPreviewFrame() override
   {
      mTimer.Stop();
      // Shut down rendering before wx destroys the native child surface.
      mPlayer.reset();
   }

   bool Load(const wxString &path, const VideoTimeline::Timeline *saved = nullptr)
   {
      mPath = path;
      SetTitle(_("Video Preview") + " - " + wxFileName{ path }.GetFullName());
      Layout();
      if (saved) {
         // Reopening/undo must not enter a nested modal event loop: queued
         // Close commands could otherwise destroy the project during loading.
         mSourceDuration = saved->sourceDuration; mSourceFrameRate = saved->frameRate;
         mMedia = VideoPreview::PrepareVideoMedia(path, mSourceDuration, true, this, false);
         if (!mMedia) mMedia = VideoPreview::PrepareVideoMedia(path, mSourceDuration, false, this, false);
      }
      mPlayer = std::make_unique<MpvPlayer>(*mSurface, mMedia ? mMedia->playbackPath : path);
      mLoadTime.Start();
      if (saved) return true;

      // Finish opening before importing audio or replacing the old reference.
      // The modal dialog prevents transport changes during this bounded wait;
      // the timer starts only after soundtrack import has finished.
      wxProgressDialog progress{ _("Open Video Reference"), _("Loading video..."),
         100, GetParent(), wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE };
      while (!mPlayer->ReadState().ready) {
         if (!progress.Pulse())
            return false;
         if (mLoadTime.Time() > 15000)
            throw std::runtime_error{ "The video did not become ready within 15 seconds." };
         wxMilliSleep(10);
      }
      const auto state = mPlayer->ReadState();
      mSourceHasAudio = mPlayer->HasAudio();
      mSourceDuration = state.duration; mSourceFrameRate = state.frameRate;
      const bool lightweight = mPlayer->Property("width") * mPlayer->Property("height") > 1280.0 * 720.0 ||
         state.frameRate > 60;
      mMedia = VideoPreview::PrepareVideoMedia(path, state.duration, lightweight, this);
      if (lightweight) {
         mPlayer = std::make_unique<MpvPlayer>(*mSurface, mMedia->playbackPath);
         mLoadTime.Start();
      }
      return true;
   }

   bool HasAudio() const { return mSourceHasAudio; }

   const wxString &GetVideoPath() const { return mPath; }
   double GetVideoDuration() const { return mSourceDuration; }
   double GetVideoFrameRate() const { return mSourceFrameRate; }
   auto Media() const { return mMedia; }
   auto PlayerState() { return mPlayer ? mPlayer->ReadState() : VideoPreview::PlayerSnapshot{}; }
   double PlayerProperty(const char *name) { return mPlayer ? mPlayer->Property(name) : 0.0; }
   const auto &PlaybackSamples() const { return mPlaybackSamples; }

   void StartPreview()
   {
      mTimer.Start(20);
   }
};

class ProjectVideoPreview final : public ClientData::Base {
public:
   ProjectVideoPreview() { VideoExport::CleanAbandonedWorkspace(); VideoPreview::CleanPreviewCache(); }
   wxWeakRef<VideoPreviewFrame> frame;

   ~ProjectVideoPreview() override
   {
      if (frame && !frame->IsBeingDeleted())
         frame->Destroy();
   }
};

const AudacityProject::AttachedObjects::RegisteredFactory sVideoPreviewKey{
   [](AudacityProject &) { return std::make_shared<ProjectVideoPreview>(); }
};

ProjectVideoPreview &PreviewFor(AudacityProject &project)
{
   return project.AttachedObjects::Get<ProjectVideoPreview>(sVideoPreviewKey);
}

// Public diagnostics report actual decoded output and dropped frames, so
// playback performance can be checked through the normal scripting interface.
class GetVideoPreviewInfoCommand final : public AudacityCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Get video preview performance."); }
   bool Apply(const CommandContext &context) override {
      auto &preview = PreviewFor(context.project);
      auto *frame = preview.frame.get();
      const auto state = frame ? frame->PlayerState() : VideoPreview::PlayerSnapshot{};
      const auto media = frame ? frame->Media() : nullptr;
      const auto strip = VideoTimeline::GetProjectVideoTrackStatus(context.project);
      rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
      out.StartObject();
      out.Key("ready"); out.Bool(state.ready);
      out.Key("lightweight"); out.Bool(media && media->lightweight);
      out.Key("position"); out.Double(state.position);
      out.Key("paused"); out.Bool(state.paused);
      out.Key("width"); out.Double(frame ? frame->PlayerProperty("width") : 0);
      out.Key("height"); out.Double(frame ? frame->PlayerProperty("height") : 0);
      out.Key("droppedFrames"); out.Double(frame ? frame->PlayerProperty("frame-drop-count") : 0);
      out.Key("decoderDroppedFrames"); out.Double(frame ? frame->PlayerProperty("decoder-frame-drop-count") : 0);
      out.Key("audioPosition"); out.Double(ProjectAudioIO::Get(context.project).IsAudioActive()
         ? AudioIO::Get()->GetStreamTime() : ViewInfo::Get(context.project).selectedRegion.t0());
      out.Key("playbackPath"); out.String(media ? Utf8(media->playbackPath).c_str() : "");
      out.Key("thumbnails"); out.Bool(media != nullptr);
      out.Key("videoTrackVisible"); out.Bool(strip.visible);
      out.Key("videoTrackThumbnails"); out.Bool(strip.thumbnails);
      out.Key("videoTrackLeftOffset"); out.Int(strip.leftOffset);
      out.Key("videoTrackDataWidth"); out.Int(strip.usableWidth);
      out.Key("viewStart"); out.Double(strip.viewStart);
      out.Key("viewEnd"); out.Double(strip.viewEnd);
      out.Key("samples"); out.StartArray();
      if (frame) for (const auto &sample : frame->PlaybackSamples()) {
         out.StartObject();
         out.Key("audioPosition"); out.Double(sample.audio);
         out.Key("position"); out.Double(sample.video);
         out.Key("paused"); out.Bool(sample.paused);
         out.Key("droppedFrames"); out.Double(sample.dropped);
         out.Key("decoderDroppedFrames"); out.Double(sample.decoderDropped);
         out.EndObject();
      }
      out.EndArray();
      out.EndObject(); context.Status(wxString::FromUTF8(buffer.GetString())); return true;
   }
};
const ComponentInterfaceSymbol GetVideoPreviewInfoCommand::Symbol{XO("GetVideoPreviewInfo")};
BuiltinCommandsModule::Registration<GetVideoPreviewInfoCommand> sGetVideoPreviewInfo;
// Native configured commands stop audio. Retain only a bounded observation of
// the last playback, so reading diagnostics never needs to change that contract.
MenuRegistry::AttachedItem sVideoPreviewInfo{
   MenuRegistry::Command("GetVideoPreviewInfo", XXO("Get Video Preview Info"),
      CommandDispatch::OnAudacityCommand, AlwaysEnabledFlag),
   wxT("Optional/Extra/Part2/Scriptables2")
};

bool ImportVideoReference(const CommandContext &context, const wxString &path)
{
   auto &project = context.project;
   if (!wxFileExists(path)) {
      context.Error(_("The video file does not exist."));
      return false;
   }
   auto &preview = PreviewFor(project);
   const auto destroyCandidate = [](VideoPreviewFrame *frame) { frame->Destroy(); };
   std::unique_ptr<VideoPreviewFrame, decltype(destroyCandidate)> candidate{
      new VideoPreviewFrame{ project }, destroyCandidate };
   try {
       if (!candidate->Load(path)) return false;
       auto timeline = VideoTimeline::CreateVideoReference(Utf8(path),
          candidate->GetVideoDuration(), candidate->GetVideoFrameRate());
       const auto hasAudio = candidate->HasAudio();
       if (hasAudio && !ProjectFileManager::Get(project).ImportVideoAudio(path)) return false;
       VideoTimeline::AdoptProjectVideo(project, std::move(timeline), hasAudio);
       VideoTimeline::ShowProjectVideoTrack(project, candidate->Media());
      auto &viewport = Viewport::Get(project);
      viewport.ZoomFitHorizontally();
      const auto tracks = TrackList::Get(project).Any<Track>();
      if (!tracks.empty()) {
         auto *lastTrack = *tracks.rbegin();
         TrackFocus::Get(project).Set(lastTrack, true);
         viewport.ShowTrack(*lastTrack);
      }
      viewport.HandleResize();
      if (preview.frame && !preview.frame->IsBeingDeleted())
         preview.frame->Destroy();
      candidate->StartPreview();
      candidate->ShowWithoutActivating();
      preview.frame = candidate.release();
      return true;
   }
   catch (const std::exception &error) {
      context.Error(path + "\n" + wxString::FromUTF8(error.what()));
      return false;
   }
}

// Both the File menu and scripting invoke these same operations. PromptUser
// supplies native file pickers; configured scripting calls supply filenames.
class ImportVideoReferenceCommand final : public AudacityCommand {
   wxString mFileName;
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
   { return XO("Open a video reference and import its editable soundtrack."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   { visitor.Define(mFileName, wxT("Filename"), wxString{}); return true; }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool PromptUser(AudacityProject &project) override
   {
      wxFileDialog dialog{ &ProjectWindow::Get(project), _("Open Video Reference"),
         {}, {}, _("Video files (*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v)|*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v|All files (*.*)|*.*"),
         wxFD_OPEN | wxFD_FILE_MUST_EXIST };
      if (dialog.ShowModal() != wxID_OK) return false;
      mFileName = dialog.GetPath();
      return true;
   }
   bool Apply(const CommandContext &context) override
   { return ImportVideoReference(context, mFileName); }
};
const ComponentInterfaceSymbol ImportVideoReferenceCommand::Symbol{ XO("ImportVideoReference") };
BuiltinCommandsModule::Registration<ImportVideoReferenceCommand> sImportVideoCommand;

class ExportVideoMixCommand final : public AudacityCommand {
   wxString mFileName;
   bool mOverwrite{};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
   { return XO("Export the video picture with the current project audio mix."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   {
      visitor.Define(mFileName, wxT("Filename"), wxString{});
      visitor.Define(mOverwrite, wxT("Overwrite"), false);
      return true;
   }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool PromptUser(AudacityProject &project) override
   {
       const auto &timeline = VideoTimeline::GetProjectTimeline(project);
       if (timeline.sourcePath.empty()) {
         wxMessageBox(_("Import a video reference first."), _("Export Video with Audio Mix"),
            wxOK | wxICON_INFORMATION, &ProjectWindow::Get(project));
         return false;
      }
       const wxFileName source{ wxString::FromUTF8(timeline.sourcePath) };
      wxFileDialog dialog{ &ProjectWindow::Get(project), _("Export Video with Audio Mix"),
         source.GetPath(), source.GetName() + "-mix.mp4",
         _("MP4 video (*.mp4)|*.mp4|Matroska video (*.mkv)|*.mkv"),
         wxFD_SAVE | wxFD_OVERWRITE_PROMPT };
      if (dialog.ShowModal() != wxID_OK) return false;
      mFileName = dialog.GetPath();
      mOverwrite = wxFileExists(mFileName);
      return true;
   }
   bool Apply(const CommandContext &context) override
   {
       const auto timeline = VideoTimeline::GetProjectTimeline(context.project);
       if (timeline.sourcePath.empty()) {
         context.Error(_("Import a video reference first."));
         return false;
      }
      try {
          const bool exported = VideoExport::ExportMix(context.project, timeline,
            mFileName, mOverwrite, &ProjectWindow::Get(context.project));
         if (exported) context.Status(_("Exported video with the project audio mix: ") + mFileName);
         return exported;
      }
      catch (const ExportException &error) { context.Error(error.What()); }
      catch (const ExportErrorException &error) { context.Error(error.GetMessage().Translation()); }
      catch (const ExportDiskFullError &) { context.Error(_("Not enough space to export the video.")); }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); }
      return false;
   }
};
const ComponentInterfaceSymbol ExportVideoMixCommand::Symbol{ XO("ExportVideoMix") };
BuiltinCommandsModule::Registration<ExportVideoMixCommand> sExportVideoCommand;

void OnShowVideo(const CommandContext &context)
{
   if (VideoTimeline::GetProjectTimeline(context.project).sourcePath.empty())
      wxMessageBox(_("Use File > Import > Video Reference to choose a video."),
         _("Video Preview"), wxOK | wxICON_INFORMATION,
         &ProjectWindow::Get(context.project));
   else VideoTimeline::ShowProjectVideoPreview(context.project, true);
}

void OnRemoveVideo(const CommandContext &context)
{
   VideoTimeline::RemoveProjectVideo(context.project);
   auto &preview = PreviewFor(context.project);
   if (preview.frame && !preview.frame->IsBeingDeleted())
      preview.frame->Destroy();
   preview.frame = nullptr;
}

using namespace MenuRegistry;
AttachedItem sImportVideo{
   Command("ImportVideoReference", XXO("&Video Reference..."), CommandDispatch::OnAudacityCommand,
      AudioIONotBusyFlag()),
   wxT("File/Import-Export/Import")
};
AttachedItem sExportVideo{
   Command("ExportVideoMix", XXO("Export &Video with Audio Mix..."),
      CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
   wxT("File/Import-Export")
};
AttachedItem sVideoView{
   Section("VideoReference",
      Command("ShowVideoPreview", XXO("Video &Preview"), OnShowVideo,
         AlwaysEnabledFlag),
      Command("RemoveVideoReference", XXO("Remove Video Reference"), OnRemoveVideo,
         AudioIONotBusyFlag())),
   wxT("View")
};

} // namespace

void VideoTimeline::ShowProjectVideoPreview(AudacityProject &project, bool raise)
{
   auto &preview = PreviewFor(project);
   if (ProjectWindow::Get(project).IsBeingDeleted()) return;
   const auto timeline = GetProjectTimeline(project);
   ShowProjectVideoTrack(project);
   if (timeline.sourcePath.empty()) {
      if (preview.frame && !preview.frame->IsBeingDeleted()) preview.frame->Destroy();
      preview.frame = nullptr;
      return;
   }
   const auto path = wxString::FromUTF8(timeline.sourcePath);
   if (!preview.frame || preview.frame->IsBeingDeleted() || preview.frame->GetVideoPath() != path) {
      const auto destroy = [](VideoPreviewFrame *frame) { frame->Destroy(); };
      std::unique_ptr<VideoPreviewFrame, decltype(destroy)> candidate(new VideoPreviewFrame(project), destroy);
      try {
         if (!candidate->Load(path, &timeline)) return;
         ShowProjectVideoTrack(project, candidate->Media());
         if (preview.frame && !preview.frame->IsBeingDeleted()) preview.frame->Destroy();
         candidate->StartPreview();
         preview.frame = candidate.release();
      }
      catch (const std::exception &error) {
         std::fprintf(stderr, "AuTopsy preview opening: %s\n", error.what());
         wxMessageBox(path + "\n" + wxString::FromUTF8(error.what()), _("Video Preview"),
            wxOK | wxICON_ERROR, &ProjectWindow::Get(project));
         return;
      }
   }
   if (raise) { preview.frame->Show(); preview.frame->Raise(); }
}
