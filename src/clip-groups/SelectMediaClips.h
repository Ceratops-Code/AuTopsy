// Temporary move selection owns opaque identities, never native clip objects.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

class AudacityProject;
class Track;
class WaveClip;
class ChannelGroupInterval;
class wxRect;
struct ClipMoveState;

namespace ClipGroups {
class TemporaryClipSelection {
public:
   using Key = std::uint64_t;
   void Click(Key key, bool additive) {
      if (!key) return;
      if (additive) {
         mExplicit = true;
         const auto found = std::find(mKeys.begin(), mKeys.end(), key);
         if (found == mKeys.end()) mKeys.push_back(key);
         else mKeys.erase(found);
      }
      else if (!mExplicit || !Contains(key)) mKeys = {key};
      mAnchor = key;
   }
   void Clear() { mKeys.clear(); mAnchor = 0; mExplicit = false; }
   bool IsExplicit() const { return mExplicit; }
   bool Contains(Key key) const {
      return std::find(mKeys.begin(), mKeys.end(), key) != mKeys.end();
   }
   void Retain(const std::vector<Key> &live) {
      mKeys.erase(std::remove_if(mKeys.begin(), mKeys.end(), [&](Key key) {
         return std::find(live.begin(), live.end(), key) == live.end();
      }), mKeys.end());
      if (!Contains(mAnchor)) mAnchor = mKeys.empty() ? 0 : mKeys.front();
   }
   std::vector<Key> MovingKeys() const {
      if (!mExplicit) return {};
      auto result = mKeys;
      const auto anchor = std::find(result.begin(), result.end(), mAnchor);
      if (anchor != result.end()) std::rotate(result.begin(), anchor, anchor + 1);
      return result;
   }
   const std::vector<Key> &Keys() const { return mKeys; }
private:
   std::vector<Key> mKeys;
   Key mAnchor{};
   bool mExplicit{};
};

// Native adapters resolve live objects at every gesture; selection is not saved.
bool ClickTemporaryMediaClip(AudacityProject &project, Track &track, const ChannelGroupInterval &hit, bool additive);
bool ClickTemporaryMediaClipAtPointer(AudacityProject &project, Track &track, const wxRect &rect, int x, int y, bool additive);
void ClearTemporaryClipSelection(AudacityProject &project);
bool HasTemporaryClipSelection(AudacityProject &project);
bool IsAudioClipInTemporarySelection(AudacityProject &project, const WaveClip &clip);
bool IsVideoClipInTemporarySelection(AudacityProject &project, const Track &track, std::uint64_t id);
bool ApplyTemporaryClipMoveSelection(AudacityProject &project, ClipMoveState &state);
TemporaryClipSelection::Key TemporaryVideoClipSelectionKey(AudacityProject &project, const Track &track, std::uint64_t id);
void MoveTemporaryVideoClipReference(AudacityProject &project, TemporaryClipSelection::Key key, Track &destination, std::uint64_t id);
}
