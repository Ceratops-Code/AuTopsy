// Owned media-player boundary; native project and track objects stay outside.
#pragma once
#include <memory>
class wxWindow;
class wxString;
namespace VideoEditing { struct VideoFrameCrop; }
namespace VideoPreview {
struct PlayerSnapshot;
class VideoMediaPlayer final {
   class Impl;
   std::unique_ptr<Impl> mImpl;
public:
   VideoMediaPlayer(wxWindow &surface, const wxString &path);
   ~VideoMediaPlayer();
   void OpenVideoFile(const wxString &path);
   PlayerSnapshot ReadPlaybackState();
   bool HasAudioStream();
   double ReadNumericVideoProperty(const char *name);
   void SetPlaybackPaused(bool paused);
   void SetPlaybackSpeed(double speed);
   void SeekSourceTime(double position);
   void SetPictureCrop(const VideoEditing::VideoFrameCrop &crop);
};
}
