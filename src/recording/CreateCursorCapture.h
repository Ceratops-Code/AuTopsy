#pragma once

#include <memory>

class WaveTrack;
class WaveClip;
struct RecordableSequence;

namespace CursorRecording {
// Pin capture to one newly created clip in a pending track. AudioIO owns the
// returned sink until its final flush; pending-track commit owns publication.
std::shared_ptr<RecordableSequence> CreateCursorCapture(
   std::shared_ptr<WaveTrack> track, std::shared_ptr<WaveClip> clip);
}
