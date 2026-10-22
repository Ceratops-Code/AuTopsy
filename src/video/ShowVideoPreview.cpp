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
#include "CreateVideoTrack.h"
#include "RelinkVideoMedia.h"
#include "../clip-groups/LinkMediaClips.h"
#include "WaveTrack.h"
#include "ProjectHistory.h"
#include "SelectUtilities.h"

#include "AudioIO.h"
#include "CommandContext.h"
#include "CommandManager.h"
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
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/stopwatch.h>
#include <wx/timer.h>
#include <wx/textdlg.h>
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

void StepProjectVideoFrame(AudacityProject &project, int direction);
void GoToProjectVideoTimecode(AudacityProject &project);

std::string Utf8(const wxString &text)
{
   const auto bytes = text.ToUTF8();
   return { bytes.data(), bytes.length() };
}

// Only this adapter knows native playback types. Buttons and the independent
// desired-state tests use the same transport policy; mpv remains a follower.
class NativePreviewTransport final {
   AudacityProject &mProject;
public:
   explicit NativePreviewTransport(AudacityProject &project) : mProject(project) {}
   VideoPreview::ProjectTransportState ReadState() const {
      auto &audio = ProjectAudioManager::Get(mProject);
      return {VideoTimeline::HasProjectVideoClips(mProject), audio.CanStopAudioStream(),
         audio.Playing(), audio.Paused(), audio.Recording(), audio.Stopping()};
   }
   void StartPlayback() { ProjectAudioManager::Get(mProject).PlayCurrentRegion(); }
   void SetPaused(bool paused) {
      const auto state = ReadState();
      if (state.canControl && state.playing && !state.recording && state.paused != paused)
         ProjectAudioManager::Get(mProject).OnPause();
   }
   void StopPlayback() {
      const auto state = ReadState();
      if (state.canControl && state.playing && !state.recording)
         ProjectAudioManager::Get(mProject).Stop();
   }
};




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
   wxString mRequestedPath;

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
            char *loaded{};
            mApi.Check(mApi.get(mHandle.get(), "path", MPV_FORMAT_STRING, &loaded), "Reading loaded video path");
            const std::unique_ptr<char, decltype(&mpv_free)> path{loaded, mApi.free};
            if (!path || !wxFileName(wxString::FromUTF8(path.get())).SameAs(wxFileName(mRequestedPath))) continue;
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

      OpenFile(path);
   }

   void OpenFile(const wxString &path)
   {
      ReadEvents();
      SetPaused(true);
      mLoaded = false; mRequestedPath = path;
      const auto filename = Utf8(path);
      const char *arguments[]{"loadfile", filename.c_str(), "replace", nullptr};
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
   wxStopWatch mPlaybackSampleClock;
   wxPanel *mVideoArea{};
   wxPanel *mSurface{};
   wxStaticText *mStatus{};
   wxButton *mPlayButton{}, *mPauseButton{}, *mStopButton{};
   std::unique_ptr<MpvPlayer> mPlayer;
   Observer::Subscription mProjectClosing;
   std::shared_ptr<const VideoPreview::PreparedMedia> mMedia;
   std::deque<std::shared_ptr<const VideoPreview::PreparedMedia>> mPreviousMedia;
   double mSourceDuration{}, mSourceFrameRate{30};
   int mSourceWidth{}, mSourceHeight{};
   bool mSourceHasAudio{}, mDurationChecked{};
   struct PlaybackSample {
      double audio, video, dropped, decoderDropped; bool paused;
      double expected{}; std::string source; double speed{1};
   };
   std::deque<PlaybackSample> mPlaybackSamples;
   long mLastSampleTime{};
   wxString mPath;
   bool mWasPlaying{};
   bool mPreciseFrames{}, mUsingOriginalFrames{};
   double mProjectTime{};

   void UpdateTransportControls(AudacityProject &project)
   {
      const auto state = NativePreviewTransport(project).ReadState();
      const auto controls = VideoPreview::GetPreviewTransportControls(state);
      mPlayButton->Enable(controls.play); mPauseButton->Enable(controls.pause); mStopButton->Enable(controls.stop);
      const auto label = state.playing && state.paused ? _("Resume") : _("Play");
      if (mPlayButton->GetLabel() != label) mPlayButton->SetLabel(label);
   }

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
      UpdateTransportControls(*project);
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
             mPlaybackSamples.clear(); mLastSampleTime = 0; mPlaybackSampleClock.Start();
         }
         mWasPlaying = transport.playing;

          const auto picturePosition = VideoTimeline::FindProjectPicture(*project, transport.position);
         mProjectTime = transport.position;
         if (picturePosition.visible && mPath != wxString::FromUTF8(picturePosition.source.path)) {
            const auto &source = picturePosition.source;
            const auto metadata = VideoTimeline::CreateVideoTimeline(source.path,source.duration,
               source.frameRate,source.width,source.height);
             Load(wxString::FromUTF8(source.path), &metadata);
          }
          // Playback keeps its low-cost proxy. Frame inspection reads the source
          // so a 30 fps proxy cannot hide frames from a 60 fps original.
          if (transport.playing) mPreciseFrames = false;
          const bool original = mPreciseFrames && mMedia && mMedia->lightweight;
          if (original != mUsingOriginalFrames) {
             mPlayer->OpenFile(original ? mPath : mMedia->playbackPath);
             mUsingOriginalFrames = original;
             mLoadTime.Start();
          }
         if (mSurface->IsShown() != picturePosition.visible) {
            mSurface->Show(picturePosition.visible); mVideoArea->Layout();
         }
         if (picturePosition.visible) {
            auto picture = transport; picture.position = picturePosition.sourceTime;
            picture.speed *= picturePosition.playbackRate;
             VideoPreview::SynchronizeVideoPlayback(*mPlayer,picture,mPreciseFrames);
         }
         else if (!mPlayer->ReadState().paused) mPlayer->SetPaused(true);
         const auto video = mPlayer->ReadState();
          if (transport.playing && video.ready && mPlaybackSampleClock.Time() - mLastSampleTime >= 100) {
            mPlaybackSamples.push_back({transport.position, video.position,
               mPlayer->Property("frame-drop-count"), mPlayer->Property("decoder-frame-drop-count"),
               video.paused, picturePosition.sourceTime, picturePosition.source.path, video.speed});
            if (mPlaybackSamples.size() > 256) mPlaybackSamples.pop_front();
             mLastSampleTime = mPlaybackSampleClock.Time();
         }
         if (video.ready) mPreviousMedia.clear();
         if (video.ready && !mDurationChecked) {
            if (std::abs(video.duration - mSourceDuration) > std::max(0.1, 2.0 / mSourceFrameRate))
               throw std::runtime_error("The source video duration changed. Import the video again.");
            mDurationChecked = true;
         }
         if (!video.ready && mLoadTime.Time() > 15000)
            throw std::runtime_error{ "The video did not become ready within 15 seconds." };
          const auto rate = picturePosition.visible ? picturePosition.source.frameRate : mSourceFrameRate;
          // Native audio selections and preroll may precede video time zero.
          const auto timecode = (transport.position < 0 ? wxString{"-"} : wxString{}) +
             wxString::FromUTF8(VideoPreview::FormatFrameTimecode(std::abs(transport.position), rate));
          mStatus->SetLabel(video.ready
             ? wxString::Format(_("%s  |  %.3f fps (non-drop)  |  %.3f s%s"),
                timecode.c_str(), rate, transport.position,
                picturePosition.visible ? wxT("") : wxT("  |  no picture"))
             : _("Loading video..."));
      }
      catch (const std::exception &error) {
         std::fprintf(stderr, "AuTopsy preview timer: %s\n", error.what());
         // Destroying the decoder also stops video. Audio is never stopped here.
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
       auto *playback = new wxBoxSizer{wxHORIZONTAL};
       const auto addPlayback = [&](const wxString &label, VideoPreview::PreviewTransportAction action) {
          auto *button = new wxButton{this, wxID_ANY, label};
          playback->Add(button, 0, wxRIGHT, 6);
          button->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent &) {
             if (const auto project = mProject.lock()) {
                if (ProjectWindow::Get(*project).IsBeingDeleted()) return;
                try {
                   NativePreviewTransport transport(*project);
                   VideoPreview::ApplyPreviewTransport(transport, action);
                }
                catch (const std::exception &error) {
                   wxMessageBox(wxString::FromUTF8(error.what()), _("Video Preview"), wxOK | wxICON_ERROR, this);
                }
                UpdateTransportControls(*project);
             }
          });
          return button;
       };
       mPlayButton = addPlayback(_("Play"), VideoPreview::PreviewTransportAction::Play);
       mPauseButton = addPlayback(_("Pause"), VideoPreview::PreviewTransportAction::Pause);
       mStopButton = addPlayback(_("Stop"), VideoPreview::PreviewTransportAction::Stop);
       mPlayButton->SetToolTip(_("Play or resume project audio and video"));
       mPauseButton->SetToolTip(_("Pause project audio and video at the current position"));
       mStopButton->SetToolTip(_("Stop project playback"));
       layout->Add(playback, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
       UpdateTransportControls(project);
       auto *transport = new wxBoxSizer{wxHORIZONTAL};
       const auto addControl = [&](const wxString &label, auto operation) {
          auto *button = new wxButton{this, wxID_ANY, label};
          transport->Add(button, 0, wxRIGHT, 6);
          button->Bind(wxEVT_BUTTON, [this, operation](wxCommandEvent &) {
             if (const auto project = mProject.lock()) try { operation(*project); }
             catch (const std::exception &error) {
                wxMessageBox(wxString::FromUTF8(error.what()), _("Video Preview"), wxOK | wxICON_ERROR, this);
             }
          });
       };
       addControl(_("Previous frame"), [](auto &p) { StepProjectVideoFrame(p, -1); });
       addControl(_("Next frame"), [](auto &p) { StepProjectVideoFrame(p, 1); });
       addControl(_("Go to timecode..."), [](auto &p) { GoToProjectVideoTimecode(p); });
       layout->Add(transport, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
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
      if (mMedia) {
         mPreviousMedia.push_back(mMedia);
         if (mPreviousMedia.size() > 3) mPreviousMedia.pop_front();
      }
       mDurationChecked = false; mPath = path; mUsingOriginalFrames = false;
      SetTitle(_("Video Preview") + " - " + wxFileName{ path }.GetFullName());
      Layout();
      if (saved) {
         // Reopening/undo must not enter a nested modal event loop: queued
         // Close commands could otherwise destroy the project during loading.
         mSourceDuration = saved->sourceDuration; mSourceFrameRate = saved->frameRate;
         mSourceWidth = saved->sourceWidth; mSourceHeight = saved->sourceHeight;
         mMedia = VideoPreview::PrepareVideoMedia(path, mSourceDuration, true, this, false);
         if (!mMedia) mMedia = VideoPreview::PrepareVideoMedia(path, mSourceDuration, false, this, false);
      }
      const auto playbackPath = mMedia ? mMedia->playbackPath : path;
      if (mPlayer) mPlayer->OpenFile(playbackPath);
      else mPlayer = std::make_unique<MpvPlayer>(*mSurface,playbackPath);
      mLoadTime.Start();
      if (saved) return true;

      // Finish opening before importing audio or replacing the old reference.
      // The modal dialog prevents transport changes during this bounded wait;
      // the timer starts only after soundtrack import has finished.
      wxProgressDialog progress{ _("Open Video"), _("Loading video..."),
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
      // Capture ORIGINAL display geometry before replacing this decoder's file
      // with a low-resolution proxy. Rotation is applied by the output, not dw/dh.
      double width = mPlayer->Property("video-params/dw"), height = mPlayer->Property("video-params/dh");
      if (width <= 0 || height <= 0) { width = mPlayer->Property("width"); height = mPlayer->Property("height"); }
      if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 || width > 65536 || height > 65536)
         throw std::runtime_error("The video has no usable source canvas size.");
      mSourceWidth = int(std::llround(width)); mSourceHeight = int(std::llround(height));
      const auto sourceRotation = mPlayer->Property("video-params/rotate");
      if (!std::isfinite(sourceRotation) || std::abs(sourceRotation) > 360000)
         throw std::runtime_error("The video has no usable source rotation.");
      const auto rotation = int(std::llround(sourceRotation)) % 360;
      if (rotation == 90 || rotation == 270 || rotation == -90 || rotation == -270) std::swap(mSourceWidth, mSourceHeight);
      const bool lightweight = mPlayer->Property("width") * mPlayer->Property("height") > 1280.0 * 720.0 ||
         state.frameRate > 60;
      mMedia = VideoPreview::PrepareVideoMedia(path, state.duration, lightweight, this);
      if (lightweight) {
         mPlayer->OpenFile(mMedia->playbackPath);
         mDurationChecked = false; mLoadTime.Start();
      }
      return true;
   }

    void ReloadSource() { mPath.clear(); }
    bool HasAudio() const { return mSourceHasAudio; }
    void InspectOriginalFrames() { mPreciseFrames = true; }
    bool UsesOriginalFrames() const { return !mMedia || !mMedia->lightweight || mUsingOriginalFrames; }

   const wxString &GetVideoPath() const { return mPath; }
   double GetVideoDuration() const { return mSourceDuration; }
   double GetVideoFrameRate() const { return mSourceFrameRate; }
   int GetVideoSourceWidth() const { return mSourceWidth; }
   int GetVideoSourceHeight() const { return mSourceHeight; }
   auto Media() const { return mMedia; }
   auto PlayerState() { return mPlayer ? mPlayer->ReadState() : VideoPreview::PlayerSnapshot{}; }
   wxButton *TransportButton(VideoPreview::PreviewTransportAction action) const {
      switch (action) {
      case VideoPreview::PreviewTransportAction::Play: return mPlayButton;
      case VideoPreview::PreviewTransportAction::Pause: return mPauseButton;
      case VideoPreview::PreviewTransportAction::Stop: return mStopButton;
      default: return nullptr;
      }
   }
   uintptr_t StatusHandle() const { return reinterpret_cast<uintptr_t>(mStatus->GetHandle()); }
   uintptr_t SurfaceHandle() const { return reinterpret_cast<uintptr_t>(mSurface->GetHandle()); }
   bool PictureVisible() const { return mSurface->IsShown(); }
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

double StopAtProjectVideoCursor(AudacityProject &project)
{
   const bool playing = ProjectAudioManager::Get(project).Playing();
   if (ProjectAudioIO::Get(project).IsAudioActive() && !playing)
      throw std::runtime_error("Stop recording before inspecting video frames.");
   const auto time = playing ? AudioIO::Get()->GetStreamTime()
                            : ViewInfo::Get(project).selectedRegion.t0();
   if (playing) ProjectAudioManager::Get(project).Stop();
   return time < 0 ? 0 : time;
}

void SetProjectVideoCursor(AudacityProject &project, double time)
{
   const auto timeline = VideoTimeline::GetProjectTimeline(project);
   if (timeline.clips.empty()) throw std::runtime_error("Import a video first.");
   time = std::clamp(time, 0.0, timeline.End());
   ViewInfo::Get(project).selectedRegion.setTimes(time, time);
   VideoTimeline::ShowProjectVideoPreview(project, true);
   if (auto *frame = PreviewFor(project).frame.get()) frame->InspectOriginalFrames();
   Viewport::Get(project).ScrollIntoView(time);
   Viewport::Get(project).Redraw();
}

void StepProjectVideoFrame(AudacityProject &project, int direction)
{
   const auto time = StopAtProjectVideoCursor(project);
   const auto timeline = VideoTimeline::GetProjectTimeline(project);
   const auto *clip = VideoTimeline::ClipAt(timeline, time);
   if (!clip && direction < 0) clip = VideoTimeline::ClipAt(timeline, std::max(0.0, time - 1e-7));
   const auto rate = clip ? VideoTimeline::SourceFor(timeline, *clip).frameRate : timeline.frameRate;
   SetProjectVideoCursor(project, VideoPreview::AdjacentSourceFrame(time,
       clip ? clip->start : 0, clip ? clip->sourceStart : 0, rate, direction,
       clip ? clip->playbackRate : 1));
}

void GoToProjectVideoTimecode(AudacityProject &project)
{
   const auto time = StopAtProjectVideoCursor(project);
   const auto picture = VideoTimeline::FindProjectPicture(project, time);
   const auto rate = picture.visible ? picture.source.frameRate
                                    : VideoTimeline::GetProjectTimeline(project).frameRate;
   wxTextEntryDialog dialog{&ProjectWindow::Get(project),
      wxString::Format(_("Enter HH:MM:SS:FF at %.3f fps (non-drop-frame)."), rate),
      _("Go to Video Timecode"), wxString::FromUTF8(VideoPreview::FormatFrameTimecode(time, rate))};
   if (dialog.ShowModal() == wxID_OK)
      SetProjectVideoCursor(project, VideoPreview::ParseFrameTimecode(Utf8(dialog.GetValue()), rate));
}

class StepVideoFrameCommand final : public AudacityCommand {
   int mDirection{1};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Move the project cursor by one source video frame."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mDirection, wxT("Direction"), 1, -1, 1); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try { StepProjectVideoFrame(context.project, mDirection); return true; }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol StepVideoFrameCommand::Symbol{XO("StepVideoFrame")};
BuiltinCommandsModule::Registration<StepVideoFrameCommand> sStepVideoFrame;

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
       out.Key("previewFrameHandle"); out.Uint64(frame ? reinterpret_cast<uintptr_t>(frame->GetHandle()) : 0);
       out.Key("previewStatusHandle"); out.Uint64(frame ? frame->StatusHandle() : 0);
       out.Key("previewSurfaceHandle"); out.Uint64(frame ? frame->SurfaceHandle() : 0);
       out.Key("pictureVisible"); out.Bool(frame && frame->PictureVisible());
       const auto picture = VideoTimeline::FindProjectPicture(context.project, ViewInfo::Get(context.project).selectedRegion.t0());
       out.Key("pictureSource"); out.String(picture.visible ? picture.source.path.c_str() : "");
       out.Key("transportControls"); out.StartArray();
       for (const auto action : {VideoPreview::PreviewTransportAction::Play,
             VideoPreview::PreviewTransportAction::Pause, VideoPreview::PreviewTransportAction::Stop}) {
          const auto *button = frame ? frame->TransportButton(action) : nullptr;
          out.StartObject();
          out.Key("action"); out.String(action == VideoPreview::PreviewTransportAction::Play ? "play" :
             action == VideoPreview::PreviewTransportAction::Pause ? "pause" : "stop");
          out.Key("handle"); out.Uint64(button ? reinterpret_cast<uintptr_t>(button->GetHandle()) : 0);
          out.Key("enabled"); out.Bool(button && button->IsEnabled());
          out.EndObject();
       }
       out.EndArray();
       out.Key("originalFrames"); out.Bool(frame && frame->UsesOriginalFrames());
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
       out.Key("videoTrackPanelHandle"); out.Uint64(strip.panelHandle);
       out.Key("videoTrackPanelWidth"); out.Int(strip.width);
       out.Key("videoTrackPanelHeight"); out.Int(strip.height);
       // Report the actual native submenu through semantic command identities.
       // Unrelated plug-in command names need not cross this adapter boundary.
       auto &commands = CommandManager::Get(context.project);
       wxMenu *importMenu{};
       size_t importMatches{};
       const auto visitMenu = [&](const auto &visit, wxMenu *menu) -> void {
          for (auto *item : menu->GetMenuItems()) {
             if (commands.GetNameFromNumericID(item->GetId()) == "ImportAudio") {
                importMenu = menu; ++importMatches;
             }
             if (auto *child = item->GetSubMenu()) visit(visit,child);
          }
       };
       if (auto *bar = ProjectWindow::Get(context.project).GetMenuBar())
          for (size_t i{}; i < bar->GetMenuCount(); ++i) visitMenu(visitMenu,bar->GetMenu(i));
       out.Key("importMenu"); out.StartArray();
       if (importMatches == 1) for (auto *item : importMenu->GetMenuItems()) {
          out.StartObject();
          out.Key("id"); out.String(Utf8(commands.GetNameFromNumericID(item->GetId()).GET()).c_str());
          out.Key("label"); out.String(Utf8(item->GetItemLabelText()).c_str());
          out.EndObject();
       }
       out.EndArray();
       out.Key("videoTrackRows"); out.StartArray();
      for (const auto &row : strip.rows) {
         out.StartObject(); out.Key("index"); out.Int(row.index);
         out.Key("x"); out.Int(row.x); out.Key("y"); out.Int(row.y);
         out.Key("width"); out.Int(row.width); out.Key("height"); out.Int(row.height);
         out.Key("dataLeft"); out.Int(row.dataLeft); out.Key("dataWidth"); out.Int(row.dataWidth);
         out.Key("dataTop"); out.Int(row.dataTop);
         out.Key("selected"); out.Bool(row.selected);
         for (const auto &entry : {std::pair{"mute", row.mute}, std::pair{"solo", row.solo}}) {
            const auto &button = entry.second;
            out.Key(entry.first); out.StartObject();
            out.Key("x"); out.Int(button.x); out.Key("y"); out.Int(button.y);
            out.Key("width"); out.Int(button.width); out.Key("height"); out.Int(button.height);
            out.Key("active"); out.Bool(button.active); out.EndObject();
         }
         out.EndObject();
      }
      out.EndArray();
      out.Key("samples"); out.StartArray();
      if (frame) for (const auto &sample : frame->PlaybackSamples()) {
         out.StartObject();
         out.Key("audioPosition"); out.Double(sample.audio);
         out.Key("position"); out.Double(sample.video);
         out.Key("expectedPosition"); out.Double(sample.expected);
         out.Key("speed"); out.Double(sample.speed);
         out.Key("source"); out.String(sample.source.c_str());
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

bool ImportVideo(const CommandContext &context, const wxString &path)
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
   bool committed{};
   try {
       if (!candidate->Load(path)) return false;
       auto timeline = VideoTimeline::CreateVideoTimeline(Utf8(path),
          candidate->GetVideoDuration(), candidate->GetVideoFrameRate(),
          candidate->GetVideoSourceWidth(), candidate->GetVideoSourceHeight());
       const auto hasAudio = candidate->HasAudio();
       auto pictureTrack = VideoTimeline::CreateProjectVideoTrack(project, nullptr, false, false,
          std::move(timeline),wxFileName(path).GetName() + _(" Video"));
       try {
          ClipGroups::ScopedVideoAudioImportLinks links{project, *pictureTrack};
          // Attach picture before the native audio importer records its Undo
          // state, so a successful import is one complete audio/video action.
          if (hasAudio && !ProjectFileManager::Get(project).ImportVideoAudio(path)) {
             ProjectHistory::Get(project).RollbackState(); return false;
          }
          if (!hasAudio) ProjectHistory::Get(project).PushState(XO("Imported video"), XO("Import Video"));
          committed = true;
       }
       catch (...) {
          ProjectHistory::Get(project).RollbackState();
          throw;
       }
       SelectUtilities::SelectNone(project); pictureTrack->SetSelected(true);
       VideoTimeline::ShowProjectVideoTrack(project, candidate->Media());
      auto &viewport = Viewport::Get(project);
      viewport.ZoomFitHorizontally();
      TrackFocus::Get(project).Set(pictureTrack.get(), true);
      viewport.ShowTrack(*pictureTrack);
      viewport.HandleResize();
      if (preview.frame && !preview.frame->IsBeingDeleted())
         preview.frame->Destroy();
      candidate->StartPreview();
      candidate->ShowWithoutActivating();
      preview.frame = candidate.release();
      return true;
   }
   catch (const std::exception &error) {
      if (committed) {
         context.Status(_("Video tracks were imported; preview could not be shown: ") + wxString::FromUTF8(error.what()));
         return true;
      }
      context.Error(path + "\n" + wxString::FromUTF8(error.what()));
      return false;
   }
}

// Both the File menu and scripting invoke these same operations. PromptUser
// supplies native file pickers; configured scripting calls supply filenames.
class ImportVideoCommand final : public AudacityCommand {
   wxString mFileName;
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
   { return XO("Import video and its editable soundtrack into new tracks."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   { visitor.Define(mFileName, wxT("Filename"), wxString{}); return true; }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool PromptUser(AudacityProject &project) override
   {
      wxFileDialog dialog{ &ProjectWindow::Get(project), _("Open Video"),
         {}, {}, _("Video files (*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v)|*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v|All files (*.*)|*.*"),
         wxFD_OPEN | wxFD_FILE_MUST_EXIST };
      if (dialog.ShowModal() != wxID_OK) return false;
      mFileName = dialog.GetPath();
      return true;
   }
   bool Apply(const CommandContext &context) override
   { return ImportVideo(context, mFileName); }
};
const ComponentInterfaceSymbol ImportVideoCommand::Symbol{ XO("ImportVideo") };
BuiltinCommandsModule::Registration<ImportVideoCommand> sImportVideoCommand;

class ExportVideoMixCommand final : public AudacityCommand {
   wxString mFileName;
   bool mOverwrite{};
   wxString mPreset{"auto"};
   int mWidth{}, mHeight{}, mQuality{23};
   double mFrameRate{};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
   { return XO("Export the video picture with the current project audio mix."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   {
      visitor.Define(mFileName, wxT("Filename"), wxString{});
      visitor.Define(mOverwrite, wxT("Overwrite"), false);
      visitor.Define(mPreset, wxT("Preset"), wxString{"auto"});
      visitor.Define(mWidth, wxT("Width"), 0, 0, 16384);
      visitor.Define(mHeight, wxT("Height"), 0, 0, 16384);
      visitor.Define(mFrameRate, wxT("FrameRate"), 0.0, 0.0, 1000.0);
      visitor.Define(mQuality, wxT("Quality"), 23, 0, 51);
      return true;
   }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool PromptUser(AudacityProject &project) override
   {
       const auto &timeline = VideoTimeline::GetProjectTimeline(project);
       if (timeline.sourcePath.empty()) {
         wxMessageBox(_("Import a video first."), _("Export Video with Audio Mix"),
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
      const auto settings = VideoExport::ChooseVideoExport(&ProjectWindow::Get(project),
         {Utf8(mPreset), mWidth, mHeight, mFrameRate, mQuality});
      if (!settings) return false;
      mPreset = wxString::FromUTF8(settings->preset);
      mWidth = settings->width; mHeight = settings->height;
      mFrameRate = settings->frameRate; mQuality = settings->quality;
      return true;
   }
   bool Apply(const CommandContext &context) override
   {
       const auto timeline = VideoTimeline::GetProjectTimeline(context.project);
       if (timeline.sourcePath.empty()) {
         context.Error(_("Import a video first."));
         return false;
      }
      try {
          const bool exported = VideoExport::ExportMix(context.project, timeline,
            VideoTimeline::GetProjectVideoDuration(context.project), mFileName, mOverwrite, &ProjectWindow::Get(context.project),
            {Utf8(mPreset), mWidth, mHeight, mFrameRate, mQuality});
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
      wxMessageBox(_("Use File > Import > Video to choose a video."),
         _("Video Preview"), wxOK | wxICON_INFORMATION,
         &ProjectWindow::Get(context.project));
   else VideoTimeline::ShowProjectVideoPreview(context.project, true);
}

using namespace MenuRegistry;
AttachedItem sImportVideo{
   Command("ImportVideo", XXO("&Video..."), CommandDispatch::OnAudacityCommand,
      AudioIONotBusyFlag()),
   Placement{wxT("File/Import-Export/Import"), {OrderingHint::Begin}}
};
AttachedItem sExportVideo{
   Command("ExportVideoMix", XXO("Export &Video with Audio Mix..."),
      CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
   wxT("File/Import-Export")
};
AttachedItem sVideoView{
    Section("Video",
       Command("ShowVideoPreview", XXO("Video &Preview"), OnShowVideo,
          AlwaysEnabledFlag),
       Command("PreviousVideoFrame", XXO("Previous Video Frame"),
          [](const CommandContext &c) { StepProjectVideoFrame(c.project, -1); }, AudioIONotBusyFlag()),
       Command("NextVideoFrame", XXO("Next Video Frame"),
          [](const CommandContext &c) { StepProjectVideoFrame(c.project, 1); }, AudioIONotBusyFlag()),
       Command("GoToVideoTimecode", XXO("Go to Video Timecode..."),
          [](const CommandContext &c) { GoToProjectVideoTimecode(c.project); }, AudioIONotBusyFlag())),
    wxT("View")
};
AttachedItem sStepVideoFrameScripting{
   Command("StepVideoFrame", XXO("Step Video Frame..."), CommandDispatch::OnAudacityCommand,
      AudioIONotBusyFlag()), wxT("Optional/Extra/Part2/Scriptables2")
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

void VideoTimeline::ReloadProjectVideoPreview(AudacityProject &project)
{
   auto &preview = PreviewFor(project);
   if (preview.frame && !preview.frame->IsBeingDeleted()) {
      preview.frame->ReloadSource();
      ShowProjectVideoPreview(project, preview.frame->IsShown());
   }
}
