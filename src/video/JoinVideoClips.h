// Pure structural edits. All candidates are checked before the caller's state
// changes; repeating a completed request is a no-op and creates no new identity.
#pragma once
#include "EditVideoTimeline.h"
namespace VideoTimeline {
bool HealVideoClips(Timeline &timeline, double from, double to);
bool JoinVideoClips(Timeline &timeline, double from, double to);
bool SeparateVideoClips(Timeline &timeline, double from, double to);
}
