// libmpv owns decoding/output; this boundary owns its DLL and player lifetime.
#include "PlayVideoMedia.h"
#include "SyncVideoPlayback.h"
#include "RenderVideoComposition.h"
#include <mpv/client.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/window.h>
#include <wx/msw/wrapwin.h>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
std::string Utf8(const wxString &text)
{
   const auto bytes = text.ToUTF8();
   return {bytes.data(), bytes.length()};
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
   decltype(&mpv_set_property_string) setPropertyString{};
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
      setPropertyString = Symbol<decltype(setPropertyString)>("mpv_set_property_string");
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

}
namespace VideoPreview {
class VideoMediaPlayer::Impl final {
   // Member order ensures the player is destroyed before its DLL is unloaded.
   MpvApi mApi;
   std::unique_ptr<mpv_handle, decltype(&mpv_terminate_destroy)>
      mHandle{ nullptr, nullptr };
   bool mLoaded{};
   wxString mRequestedPath;
   VideoEditing::VideoFrameCrop mCrop;
   std::string mAppliedCrop;

   bool ApplyCrop()
   {
      if (!mLoaded) return false;
      // An empty value restores the source's ordinary container crop. "0x0"
      // is not a valid rectangle in the bundled player's option parser.
      std::string geometry;
      if (!(mCrop == VideoEditing::VideoFrameCrop{})) {
         const auto width = Number("video-params/w"), height = Number("video-params/h");
         const auto rotation = Number("video-params/rotate").value_or(0);
         if (!width || !height) return false;
         if (!std::isfinite(*width) || !std::isfinite(*height) ||
             !std::isfinite(rotation) || *width < 1 || *height < 1 ||
             *width > 65536 || *height > 65536 || std::abs(rotation) > 360000)
            throw std::runtime_error("The decoder has no usable crop geometry.");
         const auto rect = VideoEditing::CalculateVideoCropRectangle(
            mCrop, int(std::llround(*width)), int(std::llround(*height)), int(std::llround(rotation)));
         geometry = std::to_string(rect.width) + "x" + std::to_string(rect.height) +
            "+" + std::to_string(rect.x) + "+" + std::to_string(rect.y);
      }
      if (geometry == mAppliedCrop) return true;
      // The typed string API expects a pointer to a string pointer. Use the
      // dedicated setter so crop geometry always crosses the DLL as a C string.
      mApi.Check(mApi.setPropertyString(mHandle.get(), "video-crop", geometry.c_str()),
         "Applying picture crop");
      mAppliedCrop = geometry;
      return true;
   }

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
   Impl(wxWindow &surface, const wxString &path)
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
      if (!mAppliedCrop.empty()) {
         mApi.Check(mApi.setPropertyString(mHandle.get(), "video-crop", ""),
            "Clearing the previous picture crop");
      }
      mAppliedCrop.clear();
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
      if (!ApplyCrop()) return result;
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

   void SetCrop(const VideoEditing::VideoFrameCrop &crop)
   {
      VideoEditing::RequireUsableVideoFrameCrop(crop);
      mCrop = crop;
      ApplyCrop();
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

VideoMediaPlayer::VideoMediaPlayer(wxWindow &surface, const wxString &path)
   : mImpl(std::make_unique<Impl>(surface, path)) {}
VideoMediaPlayer::~VideoMediaPlayer() = default;
void VideoMediaPlayer::OpenVideoFile(const wxString &path) { mImpl->OpenFile(path); }
PlayerSnapshot VideoMediaPlayer::ReadPlaybackState() { return mImpl->ReadState(); }
bool VideoMediaPlayer::HasAudioStream() { return mImpl->HasAudio(); }
double VideoMediaPlayer::ReadNumericVideoProperty(const char *name) { return mImpl->Property(name); }
void VideoMediaPlayer::SetPlaybackPaused(bool paused) { mImpl->SetPaused(paused); }
void VideoMediaPlayer::SetPlaybackSpeed(double speed) { mImpl->SetSpeed(speed); }
void VideoMediaPlayer::SeekSourceTime(double position) { mImpl->Seek(position); }
void VideoMediaPlayer::SetPictureCrop(const VideoEditing::VideoFrameCrop &crop) { mImpl->SetCrop(crop); }
}
