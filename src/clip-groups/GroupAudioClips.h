// Group motion crosses the native drag boundary through this single adapter.
#pragma once
struct ClipMoveState;
class TrackList;
namespace ClipGroups {
bool ExpandMovingClipGroups(ClipMoveState &state, TrackList &tracks);
}
