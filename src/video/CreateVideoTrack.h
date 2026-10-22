// Native track boundary for AuTopsy video. Timing and source values stay in
// EditVideoTimeline.h; this class supplies Audacity's normal track contracts.
#pragma once
#include "Track.h"
#include "EditVideoTimeline.h"
#include "CommonCommandFlags.h"
struct TransportSequences;

class AuTopsyVideoTrack final : public UniqueChannelTrack<> {
   VideoTimeline::Timeline mTimeline;
   double mClipboardDuration{};
   bool mVisible{true};
   bool mSolo{};
   struct ReadingClip {
      VideoTimeline::Clip clip;
      std::shared_ptr<VideoTimeline::VideoClipContent> content;
   };
   std::vector<ReadingClip> mReadingClips;
   size_t mReadingCount{};
   bool mReadError{};
   Track::Holder Clone(bool backup) const override;
   std::shared_ptr<WideChannelGroupInterval> DoGetInterval(size_t index) override;
public:
   AuTopsyVideoTrack();
   AuTopsyVideoTrack(const AuTopsyVideoTrack &other, ProtectedCreationArg&&);
   ~AuTopsyVideoTrack() override;
   // Called only after shared ownership and TrackList insertion are established.
   void BuildTrackAdapters() { AttachedTrackObjects::BuildAll(); }
   const TypeInfo &GetTypeInfo() const override;
   static const TypeInfo &ClassTypeInfo();
   const VideoTimeline::Timeline &Timeline() const { return mTimeline; }
   void SetTimeline(VideoTimeline::Timeline timeline);
   bool VideoVisible() const { return mVisible; }
   void SetVideoVisible(bool visible);
   bool VideoSolo() const { return mSolo; }
   void SetVideoSolo(bool solo);
   Track::Holder Cut(double from, double to) override;
   Track::Holder Copy(double from, double to, bool forClipboard = true) const override;
   void Clear(double from, double to) override;
   void Paste(double at, const Track &source) override;
   void Silence(double from, double to, ProgressReporter = {}) override;
   void InsertSilence(double at, double duration) override;
   void MoveTo(double start) override;
   void ShiftBy(double from, double delta) override;
   Track::Holder PasteInto(AudacityProject &project, TrackList &tracks) const override;
   size_t NIntervals() const override;
   bool HandleXMLTag(const std::string_view &tag, const AttributesList &attributes) override;
   XMLTagHandler *HandleXMLChild(const std::string_view &tag) override;
   void HandleXMLEndTag(const std::string_view &tag) override;
   void WriteXML(XMLWriter &writer) const override;
   std::optional<TranslatableString> GetErrorOpening() const override;
};
ENUMERATE_TRACK_TYPE(AuTopsyVideoTrack);

namespace VideoEditing { enum class VideoEditKind; }
namespace VideoTimeline {
// Native project operations stay at this boundary, outside the owned model.
Timeline GetProjectTimeline(const AudacityProject &project);
std::vector<const Timeline*> GetProjectVisibleVideoLayers(const AudacityProject &project);
double GetProjectVideoDuration(const AudacityProject &project);
bool SetProjectVideoTrackVisibility(AudacityProject &project, AuTopsyVideoTrack &track,
   bool visible, bool solo);
bool SplitProjectVideo(AudacityProject &project, double at);
bool EditSelectedVideoStructure(AudacityProject &project, VideoEditing::VideoEditKind kind,
   double from, double to, bool recordHistory = true);
bool MoveProjectVideoClip(AudacityProject &project, uint64_t id, double start);
bool RemoveProjectVideoClip(AudacityProject &project, uint64_t id);
void ShowProjectVideoPreview(AudacityProject &project, bool raise = false);

// Inserts into the real TrackList, optionally relative to any audio/video track.
// An import attaches its prepared track before native audio import commits Undo.
std::shared_ptr<AuTopsyVideoTrack> CreateProjectVideoTrack(AudacityProject &project,
   Track *anchor = nullptr, bool before = false, bool recordHistory = true,
   Timeline initial = {}, const wxString &name = {});
std::vector<AuTopsyVideoTrack*> ProjectVideoTracks(AudacityProject &project);
struct ProjectPicture {
   bool visible{};
   MediaSource source;
   double sourceTime{};
   uint64_t clipId{};
};
ProjectPicture FindProjectPicture(AudacityProject &project, double time);
bool MoveVideoClipBetweenTracks(AudacityProject &project, AuTopsyVideoTrack &source,
   AuTopsyVideoTrack &destination, uint64_t id, double start, bool recordHistory = true);
void SplitSelectedVideoTracks(AudacityProject &project, double from, double to, Track *only = nullptr);
void TrimSelectedVideoTracks(AudacityProject &project, double from, double to);
const ReservedCommandFlag &AudioOrVideoTracksSelectedFlag();
bool HasProjectVideoClips(const AudacityProject &project);
// Feed the existing transport clock when a project has picture but no sound.
// This adds an in-memory silent sequence, never a hidden track or audio file.
void AppendVideoPlaybackClock(TrackList &tracks, TransportSequences &transport, double rate);
}
