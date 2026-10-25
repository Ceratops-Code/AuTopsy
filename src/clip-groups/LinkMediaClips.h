// Video/audio links use native clip attachments, shifters and Undo.
#pragma once
#include <string>
#include <map>
#include <memory>
#include <vector>
class AudacityProject;
class WaveTrack;
class WaveClip;
class Track;
class TrackList;
class AuTopsyVideoTrack;
namespace VideoTimeline { struct Timeline; }
namespace ClipGroups {
// Each paste/duplicate owns one mapping shared by all copied tracks. Undo and
// saved-project clones retain IDs; only explicit user copies receive new IDs.
class MediaCopyGroups {
   std::map<std::string, std::string> mGroups;
public:
   void AssignToCopiedTrack(Track &track);
};
std::shared_ptr<const TrackList> CopyMediaForPaste(const TrackList &source);
// The import adapter supplies new audio tracks before native import records
// its Undo state. The scope is stack-owned and never persists execution state.
class ScopedVideoAudioImportLinks {
   AudacityProject &mProject;
   AuTopsyVideoTrack &mPicture;
   std::vector<WaveTrack*> mExistingAudio;
   ScopedVideoAudioImportLinks *mPrevious{};
   friend void LinkNewlyImportedVideoAudio(AudacityProject &project);
public:
   ScopedVideoAudioImportLinks(AudacityProject &project, AuTopsyVideoTrack &picture);
   ~ScopedVideoAudioImportLinks();
   ScopedVideoAudioImportLinks(const ScopedVideoAudioImportLinks &) = delete;
   ScopedVideoAudioImportLinks &operator=(const ScopedVideoAudioImportLinks &) = delete;
};
void LinkNewlyImportedVideoAudio(AudacityProject &project);
void LinkImportedMedia(AudacityProject &project, AuTopsyVideoTrack &picture,
   const std::vector<WaveTrack*> &audio);
bool MoveLinkedVideoClip(AudacityProject &project, AuTopsyVideoTrack &track,
   unsigned long long id, double start, bool recordHistory);
bool ChangeLinkedVideoClipSpeed(AudacityProject &project, AuTopsyVideoTrack &track,
   unsigned long long id, double playbackRate);
// One final affine placement, checked before any linked picture/sound changes.
bool ChangeLinkedMediaClipTiming(AudacityProject &project, Track &track,
   WaveClip *audio, unsigned long long videoId, double start, double playbackRate,
   bool recordHistory = true);
void UpdateVideoGroupIds(AudacityProject &project,
   const std::vector<std::string> &oldGroups, const std::string &newGroup);
// Called inside the native command's single Undo transaction. New right-hand
// pieces get a separate shared group so moving them does not move the left.
bool SplitMediaAtSelection(AudacityProject &project, double from, double to,
   Track *only = nullptr);
void TrimLinkedMediaToSelection(AudacityProject &project, double from, double to);
// Border gestures commit all linked members together or restore native history.
bool CommitLinkedAudioTrim(AudacityProject &project, WaveClip &clip,
   bool leftBorder, double previousBorder);
void CommitVideoClipTrim(AudacityProject &project, AuTopsyVideoTrack &track,
   VideoTimeline::Timeline candidate);
}
