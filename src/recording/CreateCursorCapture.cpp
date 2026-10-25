#include "CreateCursorCapture.h"

#include "AudioIOSequences.h"
#include "WaveClip.h"
#include "WaveTrack.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace CursorRecording {
namespace {
// WaveTrack's ordinary append/flush target is its rightmost clip. A cursor
// recording can precede existing clips, so retain its exact pending interval
// instead. Neither sample writes nor dropout repair may shift a neighbor.
class CursorCapture final : public RecordableSequence {
public:
   CursorCapture(std::shared_ptr<WaveTrack> track, std::shared_ptr<WaveClip> clip)
      : mTrack(std::move(track)), mClip(std::move(clip))
   {
      if (!mTrack || !mClip)
         throw std::invalid_argument("Cursor capture requires its pending track and clip.");
      const auto intervals = mTrack->Intervals();
      if (std::none_of(intervals.begin(), intervals.end(),
            [&](const auto& interval) { return interval.get() == mClip.get(); }))
         throw std::invalid_argument("Cursor capture clip must belong to its pending track.");
   }

   sampleFormat GetSampleFormat() const override { return mTrack->GetSampleFormat(); }
   double GetRate() const override { return mTrack->GetRate(); }
   size_t NChannels() const override { return mTrack->NChannels(); }
   bool Append(size_t channel, constSamplePtr buffer, sampleFormat format,
      size_t length, unsigned int stride, sampleFormat effectiveFormat) override
   {
      constSamplePtr buffers[]{buffer};
      return mClip->Append(channel, 1, buffers, format, length, stride, effectiveFormat);
   }
   void Flush() override { mClip->Flush(); }
   void RepairChannels() override { mClip->RepairChannels(); }
   void InsertSilence(double time, double length) override
   {
      mClip->InsertSilence(time, length);
   }

private:
   // Keep the clip's native owner alive through AudioIO's asynchronous writes.
   std::shared_ptr<WaveTrack> mTrack;
   std::shared_ptr<WaveClip> mClip;
};
}

std::shared_ptr<RecordableSequence> CreateCursorCapture(
   std::shared_ptr<WaveTrack> track, std::shared_ptr<WaveClip> clip)
{
   return std::make_shared<CursorCapture>(std::move(track), std::move(clip));
}
}
