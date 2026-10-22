// Group motion crosses the native drag boundary through this single adapter.
#pragma once
#include <string>
#include <vector>
class WaveClip;
struct ClipMoveState;
class TrackList;
namespace ClipGroups {
const std::string &GetClipGroupId(const WaveClip &clip);
void SetClipGroupId(WaveClip &clip, std::string groupId);
std::string CreateClipGroupId();
// Owned media intervals expose group identity without upstream video types.
struct LinkedClipInterval {
   virtual ~LinkedClipInterval() = default;
   virtual std::vector<std::string> MediaGroupIds() const = 0;
   virtual unsigned long long VideoClipId() const = 0;
};
bool ExpandMovingClipGroups(ClipMoveState &state, TrackList &tracks);
}
