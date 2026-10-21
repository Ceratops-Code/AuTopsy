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

// Timecode is non-drop-frame: the frame counter uses the nearest nominal
// integer rate, while conversion to seconds retains the source's exact rate.
inline std::string FormatFrameTimecode(double seconds, double rate)
{
   if (!std::isfinite(seconds) || seconds < 0 || seconds > 100000000.0 ||
       !std::isfinite(rate) || rate < 1 || rate > 1000)
      throw std::runtime_error("Timecode requires a finite time and frame rate.");
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
   if (!std::isfinite(rate) || rate < 1 || rate > 1000)
      throw std::runtime_error("Timecode requires a usable frame rate.");
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
   double sourceStart, double rate, int direction)
{
   if (!std::isfinite(projectTime) || !std::isfinite(clipStart) ||
       !std::isfinite(sourceStart) || !std::isfinite(rate) || rate < 1 || rate > 1000 ||
       (direction != -1 && direction != 1))
      throw std::runtime_error("Choose the previous or next video frame.");
   const auto frame = (projectTime - clipStart + sourceStart) * rate;
   const auto target = direction > 0 ? std::floor(frame + 1e-7) + 1
                                    : std::ceil(frame - 1e-7) - 1;
   return std::max(0.0, clipStart - sourceStart + target / rate);
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
   Player supplies ReadState(), SetPaused(bool), Seek(double), SetSpeed(double).
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
   auto video = player.ReadState();
   if (!video.ready)
      return;

   if (!std::isfinite(audio.position) || !std::isfinite(audio.speed) ||
       audio.speed <= 0.0 || !std::isfinite(video.position) ||
       !std::isfinite(video.duration) || video.duration <= 0.0) {
      if (!video.paused)
         player.SetPaused(true);
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
      player.SetPaused(true);

   video = player.ReadState();
   if (!video.ready || video.seeking)
      return;

   if (needsSeek(video)) {
      player.Seek(target);
      return; // Observe completion on a fresh call before resuming.
   }

   video = player.ReadState();
   if (!video.ready || video.seeking)
      return;
   if (needsSpeed(video))
      player.SetSpeed(audio.speed);

   video = player.ReadState();
   if (video.ready && !video.seeking && video.paused == shouldPlay)
      player.SetPaused(!shouldPlay);
}

} // namespace VideoPreview
