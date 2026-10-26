// Audacity adapters for native video tracks. The TrackList owns ordering,
// lifetime, selection, clipboard cloning and Undo; video owns only picture data.
#include "CreateVideoTrack.h"
#include "PrepareVideoEffects.h"
#include "../media/CollectProjectMedia.h"
#include "../clip-groups/LinkMediaClips.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectAudioIO.h"
#include "TrackFocus.h"
#include "Viewport.h"
#include "Uuid.h"
#include "ViewInfo.h"
#include "SelectUtilities.h"
#include "XMLTagHandler.h"
#include "WaveTrack.h"
#include "AudioIO.h"
#include "AudioIOSequences.h"
#include "CommonCommandFlags.h"
#include <algorithm>
#include <cmath>
#include <atomic>

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

AuTopsyVideoTrack::AuTopsyVideoTrack() {
   mTimeline.placement.id = audacity::Uuid::Generate().ToHexString();
   SetName(_("Video Track"));
}
AuTopsyVideoTrack::AuTopsyVideoTrack(const AuTopsyVideoTrack &other, ProtectedCreationArg &&arg)
   : UniqueChannelTrack<>(other, std::move(arg)), mTimeline(other.mTimeline),
     mClipboardDuration(other.mClipboardDuration), mVisible(other.mVisible), mSolo(other.mSolo) {}
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
uint64_t AuTopsyVideoTrack::NextPictureRevision()
{
   static std::atomic<uint64_t> revision{};
   return revision.fetch_add(1, std::memory_order_relaxed) + 1;
}
void AuTopsyVideoTrack::SetTimeline(VideoTimeline::Timeline timeline)
{
   // Range-only copies have no layer identity. Preserve this native layer's
   // owned presentation rather than giving a copied clip a second parent.
   if (timeline.placement.id.empty()) timeline.placement = mTimeline.placement;
   VideoTimeline::RequireWellFormedTimeline(timeline);
   mTimeline = std::move(timeline); mPictureRevision = NextPictureRevision(); Notify(false);
}
void AuTopsyVideoTrack::SetVideoVisible(bool visible)
{ mVisible = visible; Notify(false); }
void AuTopsyVideoTrack::SetVideoSolo(bool solo)
{ mSolo = solo; Notify(false); }
size_t AuTopsyVideoTrack::NIntervals() const { return mTimeline.clips.size(); }
std::shared_ptr<WideChannelGroupInterval> AuTopsyVideoTrack::DoGetInterval(size_t index)
{
   return index < mTimeline.clips.size()
      ? std::make_shared<VideoInterval>(*this, mTimeline.clips[index].id) : nullptr;
}
Track::Holder AuTopsyVideoTrack::Copy(double from, double to, bool) const
{
   auto result = std::make_shared<AuTopsyVideoTrack>();
   result->Init(*this); result->mVisible = mVisible; result->mSolo = mSolo;
   const auto identity = result->mTimeline.placement.id;
   result->mTimeline = VideoTimeline::CopyVideoRange(mTimeline, from, to);
   result->mTimeline.placement.id = identity;
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
   result->Init(*this); result->mVisible = mVisible; result->mSolo = mSolo; result->Paste(0, *this);
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
      if (mReadError) return false;
      if (tag == "videotrack") {
         mTimeline = {}; mReadingClips.clear(); mReadingCount = 0; mReadError = false;
         mTimeline.placement.id = audacity::Uuid::Generate().ToHexString();
         std::string_view identity, parent;
         if (!ReadOptionalAttribute(attributes, "videoLayerId", identity) ||
             !ReadOptionalAttribute(attributes, "videoParentId", parent) ||
             !ReadOptionalAttribute(attributes, "videoX", mTimeline.placement.rectangle.x) ||
             !ReadOptionalAttribute(attributes, "videoY", mTimeline.placement.rectangle.y) ||
             !ReadOptionalAttribute(attributes, "videoScaleX", mTimeline.placement.rectangle.width) ||
             !ReadOptionalAttribute(attributes, "videoScaleY", mTimeline.placement.rectangle.height)) return false;
         if (!identity.empty()) mTimeline.placement.id = identity;
         mTimeline.placement.parentId = parent;
         VideoEditing::RequireUsableVideoPlacement(mTimeline.placement);
         int schema{};
         if (!ReadOptionalAttribute(attributes, "videoSchema", schema) || schema < 0 || schema > 1) return false;
         int visible = 1;
         int solo{};
         if (!ReadOptionalAttribute(attributes, "videoVisible", visible) || visible < 0 || visible > 1 ||
             !ReadOptionalAttribute(attributes, "videoSolo", solo) || solo < 0 || solo > 1) return false;
         mVisible = visible != 0;
         mSolo = solo != 0;
         if (!ReadAttribute(attributes, "videoNextId", mTimeline.nextId)) return false;
         std::string_view source;
         if (ReadAttribute(attributes, "videoSource", source)) {
            const auto owner = GetOwner();
            std::string_view local;
            if (!ReadOptionalAttribute(attributes, "videoLocalSource", local)) return false;
            mTimeline.sourcePath = ProjectMedia::OpenVideoSourcePath(owner ? owner->GetOwner() : nullptr, std::string(source), std::string(local));
         }
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
      int muted{};
      if (!ReadOptionalAttribute(attributes, "videoMuted", muted) || muted < 0 || muted > 1) return false;
      clip.muted = muted != 0;
      int locked{};
      if (!ReadOptionalAttribute(attributes, "videoTimingLocked", locked) || locked < 0 || locked > 1) return false;
      clip.timingLocked = locked != 0;
      if (!ReadAttribute(attributes, "id", clip.id) || !ReadAttribute(attributes, "start", clip.start) ||
          !ReadAttribute(attributes, "sourceStart", clip.sourceStart) ||
          !ReadAttribute(attributes, "videoDuration", clip.duration) ||
          !ReadOptionalAttribute(attributes, "videoRate", clip.playbackRate) ||
          !ReadOptionalAttribute(attributes, "videoCropLeft", clip.crop.left) ||
          !ReadOptionalAttribute(attributes, "videoCropTop", clip.crop.top) ||
          !ReadOptionalAttribute(attributes, "videoCropRight", clip.crop.right) ||
          !ReadOptionalAttribute(attributes, "videoCropBottom", clip.crop.bottom) ||
          !ReadOptionalAttribute(attributes, "videoFadeInStart", clip.opacity.inStart) ||
          !ReadOptionalAttribute(attributes, "videoFadeInDuration", clip.opacity.inDuration) ||
          !ReadOptionalAttribute(attributes, "videoFadeOutEnd", clip.opacity.outEnd) ||
          !ReadOptionalAttribute(attributes, "videoFadeOutDuration", clip.opacity.outDuration)) return false;
      std::string_view markers;
      std::string_view ramps;
      if (ReadAttribute(attributes, "videoOpacityRamps", ramps))
         clip.opacityModifiers = VideoEditing::DecodeVideoOpacityRamps(std::string(ramps));
      if (ReadAttribute(attributes, "clipMarkers", markers))
         clip.markers = ClipMarkers::DecodeMarkers(std::string(markers));
      std::string_view link;
      if (ReadAttribute(attributes, "mediaLink", link)) clip.linkGroup = link;
      std::string_view source;
      if (ReadAttribute(attributes, "mediaSource", source)) {
         const auto owner = GetOwner();
         std::string_view local;
         if (!ReadOptionalAttribute(attributes, "mediaLocalSource", local)) return false;
         clip.media.path = ProjectMedia::OpenVideoSourcePath(owner ? owner->GetOwner() : nullptr, std::string(source), std::string(local));
      }
      if (!ReadOptionalAttribute(attributes, "mediaDuration", clip.media.duration) ||
          !ReadOptionalAttribute(attributes, "mediaFps", clip.media.frameRate) ||
          !ReadOptionalAttribute(attributes, "mediaWidth", clip.media.width) ||
          !ReadOptionalAttribute(attributes, "mediaHeight", clip.media.height)) return false;
      if (mReadingClips.empty()) mReadingCount = 0;
      if (++mReadingCount > VideoTimeline::MaximumVideoContentClips ||
          mReadingClips.size() > VideoTimeline::MaximumVideoContentDepth) return false;
      int version{};
      if (!ReadOptionalAttribute(attributes, "contentVersion", version) || version < 0) return false;
      ReadingClip reading{std::move(clip)};
      if (version) {
         if (version != 1) return false;
         reading.content = std::make_shared<VideoTimeline::VideoClipContent>();
         if (!ReadAttribute(attributes, "contentDuration", reading.content->duration) ||
             !ReadAttribute(attributes, "contentFps", reading.content->frameRate)) return false;
         int operation{}; std::string_view image, localImage, title;
         if (!ReadOptionalAttribute(attributes, "contentOperation", operation) || operation < 0 || operation > 4 ||
             !ReadOptionalAttribute(attributes, "contentHeldTime", reading.content->heldTime) ||
             !ReadOptionalAttribute(attributes, "contentAudioOffset", reading.content->audioContentOffset) ||
             !ReadOptionalAttribute(attributes, "contentImage", image) ||
             !ReadOptionalAttribute(attributes, "contentLocalImage", localImage) ||
             !ReadOptionalAttribute(attributes, "contentTitle", title) ||
             !ReadOptionalAttribute(attributes, "contentWidth", reading.content->width) ||
             !ReadOptionalAttribute(attributes, "contentHeight", reading.content->height)) return false;
         reading.content->operation = static_cast<VideoTimeline::VideoClipContent::Operation>(operation);
         reading.content->title = title;
         if (!image.empty()) {
            const auto owner = GetOwner();
            reading.content->imagePath = ProjectMedia::OpenVideoSourcePath(owner ? owner->GetOwner() : nullptr,
               std::string(image), std::string(localImage));
         }
      }
      if (!mReadingClips.empty() && !mReadingClips.back().content) return false;
      mReadingClips.push_back(std::move(reading)); return true;
   }
   catch (const std::exception &) { mReadError = true; return false; }
}
void AuTopsyVideoTrack::HandleXMLEndTag(const std::string_view &tag)
{
   if (mReadError) return;
   try {
      if (tag == "videotrack") {
         VideoTimeline::RequireWellFormedTimeline(mTimeline);
         return;
      }
      if (tag != "videoclip" || mReadingClips.empty()) return;
      auto reading = std::move(mReadingClips.back()); mReadingClips.pop_back();
      reading.clip.content = std::move(reading.content);
      if (!mReadingClips.empty()) mReadingClips.back().content->parts.push_back(std::move(reading.clip));
      // This track is private to the host's project-load transaction. Append
      // once and check the complete timeline at its closing tag, rather than
      // repeatedly copying and checking every earlier clip for a large track.
      else {
         if (mTimeline.clips.size() >= VideoTimeline::MaximumVideoTrackClips)
            throw std::runtime_error("A video track exceeds its clip limit.");
         mTimeline.clips.push_back(std::move(reading.clip));
      }
   }
   catch (const std::exception &) { mReadError = true; mReadingClips.clear(); }
}
XMLTagHandler *AuTopsyVideoTrack::HandleXMLChild(const std::string_view &tag)
{ return tag == "videoclip" ? this : nullptr; }
void AuTopsyVideoTrack::WriteXML(XMLWriter &writer) const
{
   const auto owner = GetOwner();
   const auto *project = owner ? owner->GetOwner() : nullptr;
   VideoTimeline::RequireWellFormedTimeline(mTimeline);
   writer.StartTag("videotrack"); WriteCommonXMLAttributes(writer);
   writer.WriteAttr("videoSchema", 1);
   writer.WriteAttr("videoLayerId", wxString::FromUTF8(mTimeline.placement.id));
   if (!mTimeline.placement.parentId.empty())
      writer.WriteAttr("videoParentId", wxString::FromUTF8(mTimeline.placement.parentId));
   if (!(mTimeline.placement.rectangle == VideoEditing::VideoFrameRectangle{})) {
      writer.WriteAttr("videoX", mTimeline.placement.rectangle.x, 17);
      writer.WriteAttr("videoY", mTimeline.placement.rectangle.y, 17);
      writer.WriteAttr("videoScaleX", mTimeline.placement.rectangle.width, 17);
      writer.WriteAttr("videoScaleY", mTimeline.placement.rectangle.height, 17);
   }
   writer.WriteAttr("videoVisible", mVisible ? 1 : 0);
   writer.WriteAttr("videoSolo", mSolo ? 1 : 0);
   writer.WriteAttr("videoNextId", static_cast<long long>(mTimeline.nextId));
   if (!mTimeline.sourcePath.empty()) {
      writer.WriteAttr("videoSource", wxString::FromUTF8(ProjectMedia::StoreVideoSourcePath(project, mTimeline.sourcePath)));
      const auto local = ProjectMedia::StoreLocalVideoSourcePath(project, mTimeline.sourcePath);
      if (!local.empty()) writer.WriteAttr("videoLocalSource", wxString::FromUTF8(local));
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
      if (clip.muted) writer.WriteAttr("videoMuted", 1);
      if (clip.timingLocked) writer.WriteAttr("videoTimingLocked", 1);
      if (!clip.opacityModifiers.empty()) writer.WriteAttr("videoOpacityRamps",
         wxString::FromUTF8(VideoEditing::EncodeVideoOpacityRamps(clip.opacityModifiers)));
      if (clip.playbackRate != 1) writer.WriteAttr("videoRate", clip.playbackRate, 17);
      if (!(clip.opacity == VideoEditing::VideoOpacityEnvelope{})) {
         writer.WriteAttr("videoFadeInStart", clip.opacity.inStart, 17);
         writer.WriteAttr("videoFadeInDuration", clip.opacity.inDuration, 17);
         writer.WriteAttr("videoFadeOutEnd", clip.opacity.outEnd, 17);
         writer.WriteAttr("videoFadeOutDuration", clip.opacity.outDuration, 17);
      }
      if (!(clip.crop == VideoEditing::VideoFrameCrop{})) {
         writer.WriteAttr("videoCropLeft", clip.crop.left, 17);
         writer.WriteAttr("videoCropTop", clip.crop.top, 17);
         writer.WriteAttr("videoCropRight", clip.crop.right, 17);
         writer.WriteAttr("videoCropBottom", clip.crop.bottom, 17);
      }
      writer.WriteAttr("mediaSource", wxString::FromUTF8(ProjectMedia::StoreVideoSourcePath(project, source.path)));
      const auto local = ProjectMedia::StoreLocalVideoSourcePath(project, source.path);
      if (!local.empty()) writer.WriteAttr("mediaLocalSource", wxString::FromUTF8(local));
      writer.WriteAttr("mediaDuration", source.duration, 17); writer.WriteAttr("mediaFps", source.frameRate, 17);
      writer.WriteAttr("mediaWidth", source.width); writer.WriteAttr("mediaHeight", source.height);
      if (clip.content) {
         writer.WriteAttr("contentVersion", 1);
         writer.WriteAttr("contentDuration", clip.content->duration, 17);
         writer.WriteAttr("contentFps", clip.content->frameRate, 17);
         if (clip.content->operation != VideoTimeline::VideoClipContent::Operation::Sequence) {
            writer.WriteAttr("contentOperation", static_cast<int>(clip.content->operation));
            writer.WriteAttr("contentHeldTime", clip.content->heldTime, 17);
            writer.WriteAttr("contentAudioOffset", clip.content->audioContentOffset, 17);
            writer.WriteAttr("contentWidth", clip.content->width); writer.WriteAttr("contentHeight", clip.content->height);
            if (!clip.content->title.empty()) writer.WriteAttr("contentTitle", wxString::FromUTF8(clip.content->title));
            if (!clip.content->imagePath.empty()) {
               writer.WriteAttr("contentImage", wxString::FromUTF8(ProjectMedia::StoreVideoSourcePath(project, clip.content->imagePath)));
               const auto localImage = ProjectMedia::StoreLocalVideoSourcePath(project, clip.content->imagePath);
               if (!localImage.empty()) writer.WriteAttr("contentLocalImage", wxString::FromUTF8(localImage));
            }
         }
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
   const auto picture = EvaluateVideoLayers(GetProjectVisibleVideoLayers(project), time);
   if (!picture.clip) return {};
   return {true, picture.source,
       picture.sourceTime, picture.clip->id, picture.playbackRate, picture.crop};
}
std::vector<const Timeline*> GetProjectVisibleVideoLayers(const AudacityProject &project)
{
   std::vector<VideoTimelineLayer> layers;
   for (const auto *track : ProjectVideoTracks(const_cast<AudacityProject&>(project)))
      layers.push_back({&track->Timeline(), {!track->VideoVisible(), track->VideoSolo()}});
   return VisibleVideoLayers(layers);
}
double GetProjectVideoDuration(const AudacityProject &project)
{
   double end{};
   for (const auto *track : ProjectVideoTracks(const_cast<AudacityProject&>(project)))
      end = std::max(end, track->Timeline().End());
   return end;
}
bool SetProjectVideoTrackVisibility(AudacityProject &project, AuTopsyVideoTrack &track,
   bool visible, bool solo)
{
   if (track.GetOwner().get() != &TrackList::Get(project))
      throw std::runtime_error("The video track is no longer in this project.");
   const bool oldVisible = track.VideoVisible(), oldSolo = track.VideoSolo();
   if (oldVisible == visible && oldSolo == solo) return false;
   track.SetVideoVisible(visible); track.SetVideoSolo(solo);
   try {
      ProjectHistory::Get(project).PushState(XO("Changed video track mute/solo"), XO("Video Mute/Solo"));
   }
   catch (...) { track.SetVideoVisible(oldVisible); track.SetVideoSolo(oldSolo); throw; }
   Viewport::Get(project).Redraw();
   return true;
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
   const auto boundFamilies = ClipGroups::BoundVideoTrackMoveFamilies(TrackList::Get(project));
   const bool bound = boundFamilies.count(&source) != 0;
   if (bound && &source != &destination)
      throw std::runtime_error("Detach this embedded video track before moving its clips to another track.");
   if (linked != members.end() && (!VideoContentLinkGroups(*linked).empty() || bound)) {
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
namespace {
const Clip *VideoClipAtSelectedCursor(const AudacityProject &project)
{
   const auto &region = ViewInfo::Get(project).selectedRegion;
   if (!region.isPoint()) return nullptr;
   const AuTopsyVideoTrack *selected{};
   for (const auto *track : TrackList::Get(project).Selected<const Track>()) {
      if (selected) return nullptr;
      selected = dynamic_cast<const AuTopsyVideoTrack*>(track);
      if (!selected) return nullptr;
   }
   return selected ? ClipAt(selected->Timeline(), region.t0()) : nullptr;
}
}
const ReservedCommandFlag &TimeOrVideoClipSelectedFlag()
{
   static ReservedCommandFlag flag{
      [](const AudacityProject &project) {
         return TimeSelectedPred(project) || VideoClipAtSelectedCursor(project);
      }, cutCopyOptions()
   };
   return flag;
}
void SelectVideoClipForDeletion(AudacityProject &project)
{
   if (const auto *clip = VideoClipAtSelectedCursor(project))
      ViewInfo::Get(project).selectedRegion.setTimes(clip->start, clip->End());
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
   if (auto *project = tracks.GetOwner()) VideoEditing::PrepareProjectVideoPlayback(*project);
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
