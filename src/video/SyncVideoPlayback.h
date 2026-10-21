/**********************************************************************

  Audacity: A Digital Audio Editor

  SyncVideoPlayback.h

  Video follows the audio clock. Each call reads the player's
  current state; there are no saved commands or recovery checkpoints.

**********************************************************************/

#pragma once

#include <algorithm>
#include <cmath>

namespace VideoPreview {

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
void SynchronizeVideoPlayback(Player &player, const TransportSnapshot &audio)
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
      : 1.0 / frameRate + 0.001;
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
