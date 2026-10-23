/**********************************************************************

  Audacity: A Digital Audio Editor

  SyncVideoPlayback.h

  Video follows the audio clock. Each call reads the player's
  current state; there are no saved commands or recovery checkpoints.

**********************************************************************/

#pragma once

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace VideoPreview {

enum class PreviewTransportAction { Play, Pause, Stop };

// Project transport is authoritative. A foreign stream and recording are never
// effects of preview controls; the adapter supplies current ownership on every
// request, including after a stopped or interrupted UI event.
struct ProjectTransportState {
   bool hasVideo{}, canControl{true}, playing{}, paused{}, recording{}, stopping{};
};
struct PreviewTransportControls {
   bool play{}, pause{}, stop{};
};
inline PreviewTransportControls GetPreviewTransportControls(const ProjectTransportState &state)
{
   if (!state.canControl || state.recording || state.stopping) return {};
   return {state.hasVideo && (!state.playing || state.paused),
      state.playing && !state.paused, state.playing};
}

constexpr int PreviewTimelineSteps = 1000000;
inline double PreviewTimelineTime(int value, double duration)
{
   if (!std::isfinite(duration) || duration < 0)
      throw std::runtime_error("The preview timeline needs a finite duration.");
   return duration * std::clamp(value, 0, PreviewTimelineSteps) / PreviewTimelineSteps;
}
inline int PreviewTimelineValue(double time, double duration)
{
   if (!std::isfinite(time) || !std::isfinite(duration) || duration < 0)
      throw std::runtime_error("The preview timeline needs a finite time and duration.");
   return duration > 0 ? int(std::llround(std::clamp(time / duration, 0.0, 1.0) * PreviewTimelineSteps)) : 0;
}

struct PreviewNavigationState {
   ProjectTransportState transport;
   double position{}, cursor{}, duration{};
};
struct PreviewSeekTarget {
   double position{};
   bool playing{}, paused{};
};

// A seek has an absolute destination and desired transport mode. Re-reading
// native state between effects lets the same request converge after interruption
// without a saved execution step. Starting a paused stream happens only after
// pause is set, so navigation cannot emit a burst of sound.
template<typename Transport>
bool ApplyPreviewSeek(Transport &transport, const PreviewSeekTarget &target)
{
   if (!std::isfinite(target.position) || target.position < 0)
      throw std::runtime_error("Choose a finite preview destination.");
   auto state = transport.ReadNavigation();
   const auto allowed = [](const auto &s) {
      return s.transport.hasVideo && s.transport.canControl &&
         !s.transport.recording && !s.transport.stopping;
   };
   if (!allowed(state)) return false;
   if (!std::isfinite(state.duration) || !std::isfinite(state.position) || !std::isfinite(state.cursor) ||
       state.duration < 0 || target.position > state.duration)
      throw std::runtime_error("The preview destination is outside the project.");
   const bool playing = target.playing && target.position < state.duration;
   bool changed{};
   const auto effect = [&](auto action) {
      action(); changed = true; state = transport.ReadNavigation();
      if (!allowed(state)) throw std::runtime_error("Project playback changed during preview navigation.");
   };
   // A paused frame step must not disappear inside the running-clock drift
   // tolerance, even for high-FPS sources with very short frame durations.
   const auto tolerance = state.transport.paused || target.paused ? 1e-7 : 0.04;
   if (state.transport.playing && (!playing || std::abs(state.position - target.position) > tolerance))
      effect([&] { transport.StopPlayback(); });
   if (state.cursor != target.position)
      effect([&] { transport.SetProjectCursor(target.position); });
   if (state.transport.paused != (playing && target.paused))
      effect([&] { transport.SetNavigationPaused(playing && target.paused); });
   if (playing && !state.transport.playing)
      effect([&] { transport.StartPlaybackAtCursor(); });
   return changed;
}

// Each request has at most one native effect. Repeating it reads live state and
// performs only the unmet effect; Pause is a desired state, not a toggle.
template<typename Transport>
bool ApplyPreviewTransport(Transport &transport, PreviewTransportAction action)
{
   const auto state = transport.ReadState();
   const auto controls = GetPreviewTransportControls(state);
   switch (action) {
   case PreviewTransportAction::Play:
      if (!controls.play) return false;
      if (state.playing) transport.SetPaused(false);
      else transport.StartPlayback();
      break;
   case PreviewTransportAction::Pause:
      if (!controls.pause) return false;
      transport.SetPaused(true);
      break;
   case PreviewTransportAction::Stop:
      if (!controls.stop) return false;
      transport.StopPlayback();
      break;
   default: throw std::runtime_error("Choose Play, Pause or Stop for video preview.");
   }
   return true;
}

// Timecode is non-drop-frame: the frame counter uses the nearest nominal
// integer rate, while conversion to seconds retains the source's exact rate.
// Below one source frame per second, use an elapsed-second display with a zero
// frame field; source-frame stepping still uses the actual source rate.
inline std::string FormatFrameTimecode(double seconds, double rate)
{
   if (!std::isfinite(seconds) || seconds < 0 || seconds > 100000000.0 ||
       !std::isfinite(rate) || rate <= 0 || rate > 1000)
      throw std::runtime_error("Timecode requires a finite time and frame rate.");
   rate = std::max(1.0, rate);
   const auto nominal = static_cast<long long>(std::round(rate));
   const auto frames = static_cast<long long>(std::llround(seconds * rate));
   const auto whole = frames / nominal;
   std::ostringstream out;
   out << std::setfill('0') << std::setw(2) << whole / 3600 << ':'
       << std::setw(2) << (whole / 60) % 60 << ':' << std::setw(2) << whole % 60
       << ':' << std::setw(2) << frames % nominal;
   return out.str();
}

inline double ParseFrameTimecode(const std::string &text, double rate)
{
   if (!std::isfinite(rate) || rate <= 0 || rate > 1000)
      throw std::runtime_error("Timecode requires a usable frame rate.");
   rate = std::max(1.0, rate);
   std::istringstream input{text};
   long long hours{}, minutes{}, seconds{}, frames{};
   char first{}, second{}, third{};
   if (!(input >> hours >> first >> minutes >> second >> seconds >> third >> frames) ||
       first != ':' || second != ':' || third != ':' || hours < 0 || hours > 27777 ||
       minutes < 0 || minutes > 59 || seconds < 0 || seconds > 59 ||
       frames < 0 || frames >= std::llround(rate))
      throw std::runtime_error("Enter non-drop-frame timecode as HH:MM:SS:FF.");
   input >> std::ws;
   if (!input.eof()) throw std::runtime_error("Unexpected text after timecode.");
   return ((hours * 3600 + minutes * 60 + seconds) * std::llround(rate) + frames) / rate;
}

inline double AdjacentSourceFrame(double projectTime, double clipStart,
   double sourceStart, double rate, int direction, double playbackRate = 1)
{
   if (!std::isfinite(projectTime) || !std::isfinite(clipStart) ||
       !std::isfinite(sourceStart) || !std::isfinite(rate) || rate <= 0 || rate > 1000 ||
       !std::isfinite(playbackRate) || playbackRate < 0.01 || playbackRate > 100 ||
       (direction != -1 && direction != 1))
      throw std::runtime_error("Choose the previous or next video frame.");
   const auto frame = ((projectTime - clipStart) * playbackRate + sourceStart) * rate;
   const auto target = direction > 0 ? std::floor(frame + 1e-7) + 1
                                    : std::ceil(frame - 1e-7) - 1;
   return std::max(0.0, clipStart + (target / rate - sourceStart) / playbackRate);
}

struct TransportSnapshot {
   double position{};
   double speed{ 1.0 };
   bool playing{};

   static TransportSnapshot ForProjectPlayback(bool ownsPlayback,
      bool audioPaused, double audioPosition, double cursorPosition)
   {
      return { ownsPlayback ? audioPosition : cursorPosition, 1.0,
         ownsPlayback && !audioPaused };
   }
};

struct PlayerSnapshot {
   bool ready{};
   bool paused{ true };
   bool seeking{};
   double position{};
   double duration{};
   double frameRate{ 30.0 };
   double speed{ 1.0 };
};

//! Follow one current audio snapshot without ever allowing video to drive audio.
/*!
   Player supplies ReadPlaybackState(), SetPlaybackPaused(bool),
   SeekSourceTime(double) and SetPlaybackSpeed(double).
   Effects throw on failure. A later call reads actual player state again,
   including after an interruption on either side of an effect.

   Seeking is asynchronous: keep video paused until it has finished. Tolerances
   include frame quantization, preventing an endless seek loop on low-FPS video.
   The GUI calls this on its own timer, never from the audio callback.
*/
template<typename Player>
void SynchronizeVideoPlayback(Player &player, const TransportSnapshot &audio,
   bool preciseFrames = false)
{
   auto video = player.ReadPlaybackState();
   if (!video.ready)
      return;

   if (!std::isfinite(audio.position) || !std::isfinite(audio.speed) ||
       audio.speed <= 0.0 || !std::isfinite(video.position) ||
       !std::isfinite(video.duration) || video.duration <= 0.0) {
      if (!video.paused)
         player.SetPlaybackPaused(true);
      return;
   }

   const double target = std::clamp(audio.position, 0.0,
      (std::max)(0.0, video.duration - 0.001));
   const bool shouldPlay = audio.playing && audio.position >= 0.0 &&
      audio.position < video.duration;
   const double frameRate = std::isfinite(video.frameRate) &&
      video.frameRate > 0.0 ? video.frameRate : 30.0;
   const double tolerance = shouldPlay
      ? (std::max)(0.080, 2.0 / frameRate)
       : (preciseFrames ? 0.45 / frameRate + 0.0001 : 1.0 / frameRate + 0.001);
   const auto needsSeek = [&](const PlayerSnapshot &state) {
      return std::abs(state.position - target) > tolerance;
   };
   const auto needsSpeed = [&](const PlayerSnapshot &state) {
      return shouldPlay && std::abs(state.speed - audio.speed) > 0.001;
   };

   if (!video.paused &&
       (!shouldPlay || needsSeek(video) || needsSpeed(video)))
      player.SetPlaybackPaused(true);

   video = player.ReadPlaybackState();
   if (!video.ready || video.seeking)
      return;

   if (needsSeek(video)) {
      player.SeekSourceTime(target);
      return; // Observe completion on a fresh call before resuming.
   }

   video = player.ReadPlaybackState();
   if (!video.ready || video.seeking)
      return;
   if (needsSpeed(video))
      player.SetPlaybackSpeed(audio.speed);

   video = player.ReadPlaybackState();
   if (video.ready && !video.seeking && video.paused == shouldPlay)
      player.SetPlaybackPaused(!shouldPlay);
}

} // namespace VideoPreview
