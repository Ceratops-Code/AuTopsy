// Audacity adapters for native video tracks. The TrackList owns ordering,
// lifetime, selection, clipboard cloning and Undo; video owns only picture data.
#include "CreateVideoTrack.h"
#include "../clip-groups/LinkMediaClips.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectAudioIO.h"
#include "TrackFocus.h"
#include "Viewport.h"
#include "ViewInfo.h"
#include "SelectUtilities.h"
#include "XMLTagHandler.h"
#include "WaveTrack.h"
#include "AudioIO.h"
#include "AudioIOSequences.h"
#include <algorithm>
#include <cmath>

namespace {
template<typename Value> bool ReadAttribute(const AttributesList &attributes,
   std::string_view name, Value &value)
{
   const auto found = std::find_if(attributes.begin(), attributes.end(),
      [name](const auto &attribute) { return attribute.first == name; });
   return found != attributes.end() && found->second.TryGet(value);
}
// Absent optional fields use their declared defaults. A present field with
// an unreadable wire type must fail rather than silently changing saved state.
template<typename Value> bool ReadOptionalAttribute(const AttributesList &attributes,
   std::string_view name, Value &value)
{
   const auto found = std::find_if(attributes.begin(), attributes.end(),
      [name](const auto &attribute) { return attribute.first == name; });
   return found == attributes.end() || found->second.TryGet(value);
}
bool ReadAttribute(const AttributesList &attributes, std::string_view name, uint64_t &value)
{
   long long wire{};
   if (!ReadAttribute(attributes, name, wire) || wire <= 0) return false;
   value = uint64_t(wire); return true;
}
struct VideoInterval final : WideChannelGroupInterval {
   std::weak_ptr<const AuTopsyVideoTrack> track;
   uint64_t id;
   VideoInterval(const AuTopsyVideoTrack &owner, uint64_t clip)
      : track(owner.SharedPointer<const AuTopsyVideoTrack>()), id(clip) {}
   const VideoTimeline::Clip *Find() const {
      const auto owner = track.lock();
      if (!owner) return nullptr;
      const auto &clips = owner->Timeline().clips;
      const auto found = std::find_if(clips.begin(), clips.end(),
         [&](const auto &clip) { return clip.id == id; });
      return found == clips.end() ? nullptr : &*found;
   }
   double Start() const override { const auto *clip = Find(); return clip ? clip->start : 0; }
   double End() const override { const auto *clip = Find(); return clip ? clip->End() : 0; }
   size_t NChannels() const override { return 1; }
   std::shared_ptr<ChannelInterval> DoGetChannel(size_t channel) override
   { return channel == 0 ? std::make_shared<ChannelInterval>() : nullptr; }
};
const ProjectFileIORegistry::ObjectReaderEntry sReadVideoTrack{
   "videotrack", [](AudacityProject &project) -> XMLTagHandler* {
      auto track = std::make_shared<AuTopsyVideoTrack>();
      auto *result = TrackList::Get(project).Add(track);
      result->BuildTrackAdapters();
      return result;
   }
};
class VideoPlaybackClock final : public PlayableSequence {
   std::shared_ptr<const AuTopsyVideoTrack> mGroup;
   double mEnd, mRate;
public:
   VideoPlaybackClock(std::shared_ptr<const AuTopsyVideoTrack> group, double end, double rate)
      : mGroup(std::move(group)), mEnd(end), mRate(rate) {}
   const ChannelGroup *FindChannelGroup() const override { return mGroup.get(); }
   bool GetSolo() const override { return false; }
   bool GetMute() const override { return false; }
   size_t NChannels() const override { return 1; }
   AudioGraph::ChannelType GetChannelType() const override { return AudioGraph::MonoChannel; }
   float GetChannelVolume(int) const override { return 0; }
   double GetStartTime() const override { return 0; }
   double GetEndTime() const override { return mEnd; }
   double GetRate() const override { return mRate; }
   sampleFormat WidestEffectiveFormat() const override { return floatSample; }
   bool HasTrivialEnvelope() const override { return true; }
   void GetEnvelopeValues(double *buffer, size_t len, double, bool) const override
   { std::fill_n(buffer,len,1.0); }
   bool DoGet(size_t channel, size_t count, const samplePtr buffers[], sampleFormat format,
      sampleCount, size_t len, bool, fillFormat, bool, sampleCount *within) const override {
      if (channel + count > 1) return false;
      for (size_t i{}; i < count; ++i) ClearSamples(buffers[i],format,0,len);
      if (within) *within = sampleCount{len};
      return true;
   }
};
}

AuTopsyVideoTrack::AuTopsyVideoTrack() { SetName(_("Video Track")); }
AuTopsyVideoTrack::AuTopsyVideoTrack(const AuTopsyVideoTrack &other, ProtectedCreationArg &&arg)
   : UniqueChannelTrack<>(other, std::move(arg)), mTimeline(other.mTimeline),
     mClipboardDuration(other.mClipboardDuration), mVisible(other.mVisible) {}
AuTopsyVideoTrack::~AuTopsyVideoTrack() = default;
const Track::TypeInfo &AuTopsyVideoTrack::ClassTypeInfo()
{
   static const TypeInfo info{{"video", "video", XO("Video Track")}, true, &Track::ClassTypeInfo()};
   return info;
}
const Track::TypeInfo &AuTopsyVideoTrack::GetTypeInfo() const { return ClassTypeInfo(); }
Track::Holder AuTopsyVideoTrack::Clone(bool) const
{
   auto result = std::make_shared<AuTopsyVideoTrack>(*this, ProtectedCreationArg{});
   result->Init(*this); return result;
}
void AuTopsyVideoTrack::SetTimeline(VideoTimeline::Timeline timeline)
{
   VideoTimeline::RequireWellFormedTimeline(timeline);
   mTimeline = std::move(timeline); Notify(false);
}
void AuTopsyVideoTrack::SetVideoVisible(bool visible)
{ mVisible = visible; Notify(false); }
size_t AuTopsyVideoTrack::NIntervals() const { return mTimeline.clips.size(); }
std::shared_ptr<WideChannelGroupInterval> AuTopsyVideoTrack::DoGetInterval(size_t index)
{
   return index < mTimeline.clips.size()
      ? std::make_shared<VideoInterval>(*this, mTimeline.clips[index].id) : nullptr;
}
Track::Holder AuTopsyVideoTrack::Copy(double from, double to, bool) const
{
   auto result = std::make_shared<AuTopsyVideoTrack>();
   result->Init(*this); result->mVisible = mVisible;
   result->mTimeline = VideoTimeline::CopyVideoRange(mTimeline, from, to);
   result->mClipboardDuration = to - from;
   return result;
}
Track::Holder AuTopsyVideoTrack::Cut(double from, double to)
{ auto result = Copy(from, to); Clear(from, to); return result; }
void AuTopsyVideoTrack::Clear(double from, double to)
{ auto changed = mTimeline; VideoTimeline::EraseVideoRange(changed, from, to, true); SetTimeline(std::move(changed)); }
void AuTopsyVideoTrack::Silence(double from, double to, ProgressReporter)
{ auto changed = mTimeline; VideoTimeline::EraseVideoRange(changed, from, to, false); SetTimeline(std::move(changed)); }
void AuTopsyVideoTrack::InsertSilence(double at, double duration)
{ auto changed = mTimeline; VideoTimeline::InsertVideoGap(changed, at, duration); SetTimeline(std::move(changed)); }
void AuTopsyVideoTrack::Paste(double at, const Track &source)
{
   const auto *video = dynamic_cast<const AuTopsyVideoTrack*>(&source);
   if (!video) throw std::runtime_error("Paste video clips into a video track.");
   auto changed = mTimeline;
   VideoTimeline::PasteVideoRange(changed, at, video->mTimeline,
      std::max(video->mClipboardDuration, video->mTimeline.End()));
   SetTimeline(std::move(changed));
}
Track::Holder AuTopsyVideoTrack::PasteInto(AudacityProject &, TrackList &tracks) const
{
   auto result = std::make_shared<AuTopsyVideoTrack>();
   result->Init(*this); result->mVisible = mVisible; result->Paste(0, *this);
   tracks.Add(result); return result;
}
void AuTopsyVideoTrack::MoveTo(double start)
{ ShiftBy(-1, start - GetStartTime()); }
void AuTopsyVideoTrack::ShiftBy(double from, double delta)
{
   if (!std::isfinite(delta)) throw std::runtime_error("A video track offset must be finite.");
   auto changed = mTimeline;
   for (auto &clip : changed.clips) if (clip.start >= from) clip.start += delta;
   SetTimeline(std::move(changed));
}

bool AuTopsyVideoTrack::HandleXMLTag(const std::string_view &tag, const AttributesList &attributes)
{
   try {
      if (tag == "videotrack") {
         mTimeline = {}; mReadingClips.clear(); mReadingCount = 0; mReadError = false;
         int schema{};
         if (!ReadOptionalAttribute(attributes, "videoSchema", schema) || schema < 0 || schema > 1) return false;
         int visible = 1;
         if (!ReadOptionalAttribute(attributes, "videoVisible", visible)) return false;
         mVisible = visible != 0;
         if (!ReadAttribute(attributes, "videoNextId", mTimeline.nextId)) return false;
         std::string_view source;
         if (ReadAttribute(attributes, "videoSource", source)) mTimeline.sourcePath = source;
         if (!ReadOptionalAttribute(attributes, "videoDuration", mTimeline.sourceDuration) ||
             !ReadOptionalAttribute(attributes, "videoFps", mTimeline.frameRate) ||
             !ReadOptionalAttribute(attributes, "videoWidth", mTimeline.sourceWidth) ||
             !ReadOptionalAttribute(attributes, "videoHeight", mTimeline.sourceHeight)) return false;
         AttachedTrackObjects::BuildAll();
         for (const auto &attribute : attributes) HandleCommonXMLAttribute(attribute.first, attribute.second);
         VideoTimeline::RequireWellFormedTimeline(mTimeline);
         return true;
      }
      if (tag != "videoclip") return false;
      VideoTimeline::Clip clip;
      if (!ReadAttribute(attributes, "id", clip.id) || !ReadAttribute(attributes, "start", clip.start) ||
          !ReadAttribute(attributes, "sourceStart", clip.sourceStart) ||
          !ReadAttribute(attributes, "videoDuration", clip.duration)) return false;
      std::string_view markers;
      if (ReadAttribute(attributes, "clipMarkers", markers))
         clip.markers = ClipMarkers::DecodeMarkers(std::string(markers));
      std::string_view link;
      if (ReadAttribute(attributes, "mediaLink", link)) clip.linkGroup = link;
      std::string_view source;
      if (ReadAttribute(attributes, "mediaSource", source)) clip.media.path = source;
      if (!ReadOptionalAttribute(attributes, "mediaDuration", clip.media.duration) ||
          !ReadOptionalAttribute(attributes, "mediaFps", clip.media.frameRate) ||
          !ReadOptionalAttribute(attributes, "mediaWidth", clip.media.width) ||
          !ReadOptionalAttribute(attributes, "mediaHeight", clip.media.height)) return false;
      if (++mReadingCount > 10000 || mReadingClips.size() >= 17) return false;
      int version{};
      if (!ReadOptionalAttribute(attributes, "contentVersion", version) || version < 0) return false;
      ReadingClip reading{std::move(clip)};
      if (version) {
         if (version != 1) return false;
         reading.content = std::make_shared<VideoTimeline::VideoClipContent>();
         if (!ReadAttribute(attributes, "contentDuration", reading.content->duration) ||
             !ReadAttribute(attributes, "contentFps", reading.content->frameRate)) return false;
      }
      if (!mReadingClips.empty() && !mReadingClips.back().content) return false;
      mReadingClips.push_back(std::move(reading)); return true;
   }
   catch (const std::exception &) { mReadError = true; return false; }
}
void AuTopsyVideoTrack::HandleXMLEndTag(const std::string_view &tag)
{
   if (tag != "videoclip" || mReadingClips.empty() || mReadError) return;
   try {
      auto reading = std::move(mReadingClips.back()); mReadingClips.pop_back();
      reading.clip.content = std::move(reading.content);
      if (!mReadingClips.empty()) mReadingClips.back().content->parts.push_back(std::move(reading.clip));
      else {
         auto changed = mTimeline; changed.clips.push_back(std::move(reading.clip));
         VideoTimeline::RequireWellFormedTimeline(changed); mTimeline = std::move(changed);
      }
   }
   catch (const std::exception &) { mReadError = true; mReadingClips.clear(); }
}
XMLTagHandler *AuTopsyVideoTrack::HandleXMLChild(const std::string_view &tag)
{ return tag == "videoclip" ? this : nullptr; }
void AuTopsyVideoTrack::WriteXML(XMLWriter &writer) const
{
   VideoTimeline::RequireWellFormedTimeline(mTimeline);
   writer.StartTag("videotrack"); WriteCommonXMLAttributes(writer);
   writer.WriteAttr("videoSchema", 1);
   writer.WriteAttr("videoVisible", mVisible ? 1 : 0);
   writer.WriteAttr("videoNextId", static_cast<long long>(mTimeline.nextId));
   if (!mTimeline.sourcePath.empty()) {
      writer.WriteAttr("videoSource", wxString::FromUTF8(mTimeline.sourcePath));
      writer.WriteAttr("videoDuration", mTimeline.sourceDuration, 17);
      writer.WriteAttr("videoFps", mTimeline.frameRate, 17);
      writer.WriteAttr("videoWidth", mTimeline.sourceWidth); writer.WriteAttr("videoHeight", mTimeline.sourceHeight);
   }
   const auto writeClip = [&](const auto &self, const VideoTimeline::Clip &clip) -> void {
      const auto source = VideoTimeline::SourceFor(mTimeline, clip);
      writer.StartTag("videoclip");
      writer.WriteAttr("id", static_cast<long long>(clip.id));
      if (!clip.linkGroup.empty()) writer.WriteAttr("mediaLink", wxString::FromUTF8(clip.linkGroup));
      if (!clip.markers.empty()) writer.WriteAttr("clipMarkers", wxString::FromUTF8(ClipMarkers::EncodeMarkers(clip.markers)));
      writer.WriteAttr("start", clip.start, 17); writer.WriteAttr("sourceStart", clip.sourceStart, 17);
      writer.WriteAttr("videoDuration", clip.duration, 17);
      writer.WriteAttr("mediaSource", wxString::FromUTF8(source.path));
      writer.WriteAttr("mediaDuration", source.duration, 17); writer.WriteAttr("mediaFps", source.frameRate, 17);
      writer.WriteAttr("mediaWidth", source.width); writer.WriteAttr("mediaHeight", source.height);
      if (clip.content) {
         writer.WriteAttr("contentVersion", 1);
         writer.WriteAttr("contentDuration", clip.content->duration, 17);
         writer.WriteAttr("contentFps", clip.content->frameRate, 17);
         for (const auto &part : clip.content->parts) self(self, part);
      }
      writer.EndTag("videoclip");
   };
   for (const auto &clip : mTimeline.clips) writeClip(writeClip, clip);
   writer.EndTag("videotrack");
}
std::optional<TranslatableString> AuTopsyVideoTrack::GetErrorOpening() const
{
   try {
      if (mReadError || !mReadingClips.empty()) throw std::runtime_error("Incomplete video content.");
      VideoTimeline::RequireWellFormedTimeline(mTimeline); return {};
   }
   catch (...) { return XO("The video track contains invalid clip data."); }
}

namespace VideoTimeline {
std::vector<AuTopsyVideoTrack*> ProjectVideoTracks(AudacityProject &project)
{
   std::vector<AuTopsyVideoTrack*> result;
   for (auto *track : TrackList::Get(project).Any<Track>())
      if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track)) result.push_back(video);
   return result;
}
std::shared_ptr<AuTopsyVideoTrack> CreateProjectVideoTrack(AudacityProject &project,
   Track *anchor, bool before, bool recordHistory, Timeline initial, const wxString &name)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback before adding a video track.");
   auto &tracks = TrackList::Get(project);
   const Track *next{};
   if (anchor) {
      auto position = tracks.Find(anchor);
      if (position == tracks.end()) throw std::runtime_error("The anchor track is no longer in this project.");
      next = before ? anchor : *++position;
   }
   auto track = std::make_shared<AuTopsyVideoTrack>();
   track->SetTimeline(std::move(initial));
   track->SetName(tracks.MakeUniqueTrackName(name.empty() ? _("Video Track") : name));
   tracks.Insert(next, track, true);
   try { track->BuildTrackAdapters(); }
   catch (...) { tracks.Remove(*track); throw; }
   if (recordHistory) {
      SelectUtilities::SelectNone(project); track->SetSelected(true);
      try { ProjectHistory::Get(project).PushState(XO("Added video track"), XO("Add Video Track")); }
      catch (...) { tracks.Remove(*track); throw; }
      TrackFocus::Get(project).Set(track.get(), true); Viewport::Get(project).ShowTrack(*track);
      Viewport::Get(project).HandleResize();
   }
   return track;
}
ProjectPicture FindProjectPicture(AudacityProject &project, double time)
{
   std::vector<const Timeline*> layers;
   for (const auto *track : ProjectVideoTracks(project))
      if (track->VideoVisible()) layers.push_back(&track->Timeline());
   const auto picture = EvaluateVideoLayers(layers, time);
   if (!picture.clip) return {};
   return {true, picture.source,
      picture.sourceTime, picture.clip->id};
}
bool MoveVideoClipBetweenTracks(AudacityProject &project, AuTopsyVideoTrack &source,
   AuTopsyVideoTrack &destination, uint64_t id, double start, bool recordHistory)
{
   if (source.GetOwner().get() != &TrackList::Get(project) ||
       destination.GetOwner().get() != &TrackList::Get(project))
      throw std::runtime_error("A video track was removed while the operation was pending.");
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback before moving video clips.");
   const auto &members = source.Timeline().clips;
   const auto linked = std::find_if(members.begin(), members.end(), [id](const auto &clip) { return clip.id == id; });
   if (linked != members.end() && !VideoContentLinkGroups(*linked).empty()) {
      const auto rate = VideoContentFrameRate(*linked, SourceFor(source.Timeline(), *linked));
      const auto target = std::round(start * rate) / rate;
      if (std::abs(target - linked->start) > 1e-10) {
         if (&source == &destination)
            return ClipGroups::MoveLinkedVideoClip(project, source, id, target, recordHistory);
         ClipGroups::MoveLinkedVideoClip(project, source, id, target, false);
         try { return MoveVideoClipBetweenTracks(project, source, destination, id, target, recordHistory); }
         catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
      }
   }
   auto from = source.Timeline(), to = destination.Timeline();
   if (&source == &destination) {
      if (!MoveClipTo(to, id, start)) return false;
   }
   else {
      const auto found = std::find_if(from.clips.begin(), from.clips.end(),
         [id](const auto &clip) { return clip.id == id; });
      if (found == from.clips.end()) throw std::runtime_error("The dragged video clip no longer exists.");
      auto clip = *found; clip.media = SourceFor(from, clip);
      const auto rate = clip.media.frameRate;
      clip.start = std::round(start * rate) / rate; clip.id = to.nextId++;
      to.clips.push_back(std::move(clip));
      std::sort(to.clips.begin(), to.clips.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
      RequireWellFormedTimeline(to); RemoveClip(from, id);
   }
   const auto oldSource = source.Timeline(), oldDestination = destination.Timeline();
   // Both candidates are checked before either track changes.
   if (&source != &destination) source.SetTimeline(std::move(from));
   destination.SetTimeline(std::move(to));
   if (recordHistory) try {
      ProjectHistory::Get(project).PushState(XO("Moved video clip"), XO("Move Video Clip"));
   }
   catch (...) {
      source.SetTimeline(oldSource);
      if (&source != &destination) destination.SetTimeline(oldDestination);
      throw;
   }
   return true;
}
void SplitSelectedVideoTracks(AudacityProject &project, double from, double to, Track *only)
{
   for (auto *track : ProjectVideoTracks(project)) if (only ? track == only : track->GetSelected()) {
      auto changed = track->Timeline();
      const bool first = SplitClipAt(changed, from);
      const bool last = to > from && SplitClipAt(changed, to);
      if (first || last) track->SetTimeline(std::move(changed));
   }
}
void TrimSelectedVideoTracks(AudacityProject &project, double from, double to)
{
   if (to <= from) return;
   for (auto *track : ProjectVideoTracks(project)) if (track->GetSelected()) {
      auto changed = CopyVideoRange(track->Timeline(), from, to);
      for (auto &clip : changed.clips) clip.start += from;
      track->SetTimeline(std::move(changed));
   }
}
const ReservedCommandFlag &AudioOrVideoTracksSelectedFlag()
{
   static ReservedCommandFlag flag{
      [](const AudacityProject &project) {
         const auto &tracks = TrackList::Get(project);
         if (!tracks.Selected<const WaveTrack>().empty()) return true;
         for (const auto *track : tracks.Selected<const Track>())
            if (dynamic_cast<const AuTopsyVideoTrack*>(track)) return true;
         return false;
      }, CommandFlagOptions{}.DisableDefaultMessage()
   };
   return flag;
}
bool HasProjectVideoClips(const AudacityProject &project)
{
   for (const auto *track : TrackList::Get(project).Any<const Track>())
      if (const auto *video = dynamic_cast<const AuTopsyVideoTrack*>(track))
         if (!video->Timeline().clips.empty()) return true;
   return false;
}
void AppendVideoPlaybackClock(TrackList &tracks, TransportSequences &transport, double rate)
{
   if (!transport.playbackSequences.empty() || !transport.otherPlayableSequences.empty()) return;
   AuTopsyVideoTrack *group{};
   double end{};
   for (auto *track : tracks.Any<Track>())
      if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track)) {
         if (!video->Timeline().clips.empty() && !group) group = video;
         end = std::max(end,video->Timeline().End());
      }
   if (group && end > 0)
      transport.playbackSequences.push_back(std::make_shared<VideoPlaybackClock>(
         group->SharedPointer<const AuTopsyVideoTrack>(),end,rate));
}
}
