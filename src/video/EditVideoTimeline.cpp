// Native adapters for project attachment/XML, undo, commands and the clip editor.
// Picture files remain external references; project XML owns only clip metadata.
#include "EditVideoTimeline.h"
#include "CreateVideoTrack.h"
#include "PlanVideoEdits.h"
#include "../clip-groups/LinkMediaClips.h"
#include "TrackFocus.h"
#include "Viewport.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectFileIOExtension.h"
#include "UndoManager.h"
#include "XMLTagHandler.h"
#include "CommandContext.h"
#include "MenuRegistry.h"
#include "CommonCommandFlags.h"
#include "ProjectAudioIO.h"
#include "ProjectWindow.h"
#include "ViewInfo.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include "SettingsVisitor.h"
#include <wx/app.h>
#include <wx/msgdlg.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <memory>

namespace {
using VideoTimeline::Timeline;

void RefreshPreviewLater(AudacityProject &project)
{
   const std::weak_ptr<AudacityProject> weak = project.shared_from_this();
   wxTheApp->CallAfter([weak] {
      if (const auto project = weak.lock())
         for (const auto &open : AllProjects{})
            if (open == project && !ProjectWindow::Get(*project).IsBeingDeleted()) {
               VideoTimeline::ShowProjectVideoPreview(*project);
               break;
            }
   });
}

// Opening the player belongs to the completed-project lifecycle, after every
// native track has been read. XML handlers only construct the editable model.
class VideoProjectLifecycle final : public ProjectFileIOExtension {
public:
   OnOpenAction OnOpen(AudacityProject &, const std::string &) override
   { return OnOpenAction::Continue; }
   void OnLoad(AudacityProject &project) override {
      if (VideoTimeline::HasProjectVideoClips(project)) RefreshPreviewLater(project);
   }
   OnSaveAction OnSave(AudacityProject &, const ProjectSaveCallback &) override
   { return OnSaveAction::Continue; }
   OnCloseAction OnClose(AudacityProject &) override { return OnCloseAction::Continue; }
   void OnUpdateSaved(AudacityProject &, const ProjectSerializer &) override {}
   bool IsBlockLocked(const AudacityProject &, int64_t) const override { return false; }
} sVideoProjectLifecycle;
const ProjectFileIOExtensionRegistry::Extension sVideoProjectLifecycleRegistration{sVideoProjectLifecycle};

template<typename Value>
bool ReadVideoAttribute(const AttributesList &attributes,
   std::string_view name, Value &value)
{
   const auto found = std::find_if(attributes.begin(), attributes.end(),
      [name](const auto &attribute) { return attribute.first == name; });
   return found != attributes.end() && found->second.TryGet(value);
}

// Native project XML stores these IDs as signed 64-bit integers. Read that
// exact wire type: older XMLAttributeValueView implementations cannot convert
// a positive signed value directly to uint64_t without overflowing their bound.
bool ReadVideoAttribute(const AttributesList &attributes,
   std::string_view name, uint64_t &value)
{
   long long wire{};
   if (!ReadVideoAttribute(attributes, name, wire) || wire <= 0) return false;
   value = static_cast<uint64_t>(wire);
   return true;
}

class VideoProjectFormatReader final : public ClientData::Base, public XMLTagHandler {
   AudacityProject &mProject;
   std::shared_ptr<Timeline> mReading;
public:
   explicit VideoProjectFormatReader(AudacityProject &project) : mProject(project) {}

   bool HandleXMLTag(const std::string_view &tag, const AttributesList &attributes) override
   {
      try {
         if (tag == "autopsyvideo") {
            int version{};
            auto reading = std::make_shared<Timeline>();
            std::string_view source;
            if (!ReadVideoAttribute(attributes, "version", version) || version != 1 ||
                !ReadVideoAttribute(attributes, "source", source) || source.empty() ||
                !ReadVideoAttribute(attributes, "duration", reading->sourceDuration) ||
                !ReadVideoAttribute(attributes, "fps", reading->frameRate) ||
                !ReadVideoAttribute(attributes, "nextId", reading->nextId)) return false;
            reading->sourcePath = source;
            VideoTimeline::RequireWellFormedTimeline(*reading);
            mReading = std::move(reading);
            return true;
         }
         if (tag != "videoclip" || !mReading) return false;
         VideoTimeline::Clip clip;
         if (!ReadVideoAttribute(attributes, "id", clip.id) ||
             !ReadVideoAttribute(attributes, "start", clip.start) ||
             !ReadVideoAttribute(attributes, "sourceStart", clip.sourceStart) ||
             !ReadVideoAttribute(attributes, "duration", clip.duration)) return false;
         auto changed = *mReading;
         changed.clips.push_back(clip);
         VideoTimeline::RequireWellFormedTimeline(changed);
         *mReading = std::move(changed);
         return true;
      }
      catch (const std::exception &) { mReading.reset(); return false; }
   }
   XMLTagHandler *HandleXMLChild(const std::string_view &tag) override
   { return tag == "videoclip" ? this : nullptr; }
   void HandleXMLEndTag(const std::string_view &tag) override
   {
      if (tag == "autopsyvideo" && mReading) {
         // Existing project-format input is read into the native TrackList.
         // New saves use each video track's normal XML writer.
         auto &tracks = TrackList::Get(mProject);
         const auto range = tracks.Any<Track>();
          auto track = VideoTimeline::CreateProjectVideoTrack(mProject,
             range.empty() ? nullptr : *range.begin(), true, false, std::move(*mReading));
          mReading.reset();
      }
   }
};

const AudacityProject::AttachedObjects::RegisteredFactory sVideoReaderKey{
   [](AudacityProject &project) { return std::make_shared<VideoProjectFormatReader>(project); }
};
VideoProjectFormatReader &VideoReaderFor(AudacityProject &project)
{ return project.AttachedObjects::Get<VideoProjectFormatReader>(sVideoReaderKey); }

const ProjectFileIORegistry::ObjectReaderEntry sReadTimeline{
   "autopsyvideo", [](AudacityProject &project) { return &VideoReaderFor(project); }
};
AuTopsyVideoTrack &FindClipTrack(AudacityProject &project, uint64_t id, int index = -1)
{
   auto &tracks = TrackList::Get(project);
   if (index >= 0) {
      int position{};
      for (auto *track : tracks.Any<Track>()) if (position++ == index) {
         auto *video = dynamic_cast<AuTopsyVideoTrack*>(track);
         if (!video) throw std::runtime_error("Choose a video track.");
         if (id && std::none_of(video->Timeline().clips.begin(), video->Timeline().clips.end(),
               [id](const auto &clip) { return clip.id == id; }))
            throw std::runtime_error("The video clip is not in that track.");
         return *video;
      }
      throw std::runtime_error("The video track is no longer in the project.");
   }
   const auto matches = [id](AuTopsyVideoTrack &track) {
      return !id || std::any_of(track.Timeline().clips.begin(), track.Timeline().clips.end(),
         [id](const auto &clip) { return clip.id == id; });
   };
   if (const auto focused = TrackFocus::Get(project).Get())
      if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(focused); video && matches(*video)) return *video;
   AuTopsyVideoTrack *found{};
   for (auto *track : VideoTimeline::ProjectVideoTracks(project)) if (matches(*track)) {
      if (found) throw std::runtime_error("Select a video track or supply its Track index; the clip ID is ambiguous.");
      found = track;
   }
   if (!found) throw std::runtime_error("Select a video track containing that clip.");
   return *found;
}

template<typename Edit>
bool EditProjectTimeline(AudacityProject &project, const TranslatableString &description,
   Edit edit, AuTopsyVideoTrack *selected = nullptr)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing video clips.");
   auto &track = selected ? *selected : FindClipTrack(project, 0);
   auto changed = track.Timeline();
   if (!edit(changed)) return false;
   VideoTimeline::RequireWellFormedTimeline(changed);
   const auto previous = track.Timeline();
   track.SetTimeline(std::move(changed));
   try { ProjectHistory::Get(project).PushState(description, description); }
   catch (...) { track.SetTimeline(previous); throw; }
   Viewport::Get(project).HandleResize();
   RefreshPreviewLater(project);
   return true;
}

class SplitVideoCommand final : public AudacityCommand {
   double mAt{-1};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Split the video clip at the audio cursor."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   { visitor.Define(mAt, wxT("At"), -1.0, -1.0, 100000000.0); return true; }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override
   {
      try {
         VideoTimeline::SplitProjectVideo(context.project,
            mAt < 0 ? ViewInfo::Get(context.project).selectedRegion.t0() : mAt);
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SplitVideoCommand::Symbol{ XO("SplitVideo") };
BuiltinCommandsModule::Registration<SplitVideoCommand> sSplitVideo;

class EditVideoClipCommand : public AudacityCommand {
   wxString mId;
   double mStart{};
   int mTrack{-1}, mToTrack{-1};
protected:
   virtual bool Removing() const { return false; }
public:
   TranslatableString GetDescription() const override { return XO("Move or delete a video clip by its persistent ID."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   {
      visitor.Define(mId, wxT("Id"), wxString{});
      visitor.Define(mStart, wxT("Start"), 0.0, 0.0, 100000000.0);
      visitor.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      visitor.Define(mToTrack, wxT("ToTrack"), -1, -1, 1000000);
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override
   {
      try {
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id) throw std::runtime_error("Supply a video clip Id from GetVideoClips.");
         auto &source = FindClipTrack(context.project, Removing() ? 0 : id, mTrack);
         if (!Removing()) {
            auto &destination = mToTrack >= 0 ? FindClipTrack(context.project, 0, mToTrack) : source;
            VideoTimeline::MoveVideoClipBetweenTracks(context.project, source, destination, id, mStart);
         }
         else EditProjectTimeline(context.project, XO("Deleted video clip"),
            [&](auto &timeline) { return VideoTimeline::RemoveClip(timeline, id); }, &source);
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
class MoveVideoClipCommand final : public EditVideoClipCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol MoveVideoClipCommand::Symbol{ XO("MoveVideoClip") };
BuiltinCommandsModule::Registration<MoveVideoClipCommand> sMoveVideoClip;
class DeleteVideoClipCommand final : public EditVideoClipCommand {
   bool Removing() const override { return true; }
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol DeleteVideoClipCommand::Symbol{ XO("DeleteVideoClip") };
BuiltinCommandsModule::Registration<DeleteVideoClipCommand> sDeleteVideoClip;

class GetVideoClipsCommand final : public AudacityCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("List the saved video source and clip timing."); }
   bool Apply(const CommandContext &context) override
   {
      auto &project = context.project;
      const auto timeline = VideoTimeline::GetProjectTimeline(project);
      rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
      out.StartObject();
      out.Key("source"); out.String(timeline.sourcePath.c_str());
      out.Key("duration"); out.Double(timeline.sourceDuration);
      out.Key("end"); out.Double(timeline.End());
      out.Key("fps"); out.Double(timeline.frameRate);
      out.Key("tracks"); out.StartArray();
      int index{};
      for (auto *track : TrackList::Get(project).Any<Track>()) {
         if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track)) {
            out.StartObject(); out.Key("index"); out.Int(index);
            out.Key("name"); out.String(video->GetName().ToUTF8().data());
            out.Key("selected"); out.Bool(video->GetSelected());
            out.Key("visible"); out.Bool(video->VideoVisible());
            out.Key("muted"); out.Bool(!video->VideoVisible());
            out.Key("solo"); out.Bool(video->VideoSolo());
            out.Key("end"); out.Double(video->Timeline().End()); out.EndObject();
         }
         ++index;
      }
      out.EndArray(); out.Key("clips"); out.StartArray(); index = 0;
      for (auto *track : TrackList::Get(project).Any<Track>()) {
         if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track))
            for (const auto &clip : video->Timeline().clips) {
               const auto source = VideoTimeline::SourceFor(video->Timeline(), clip);
               out.StartObject(); out.Key("id"); out.Uint64(clip.id); out.Key("track"); out.Int(index);
               out.Key("start"); out.Double(clip.start); out.Key("end"); out.Double(clip.End());
               out.Key("sourceStart"); out.Double(clip.sourceStart); out.Key("duration"); out.Double(clip.duration);
               out.Key("source"); out.String(source.path.c_str()); out.Key("fps"); out.Double(VideoTimeline::VideoContentFrameRate(clip, source));
               out.Key("joined"); out.Bool(bool(clip.content));
               out.Key("pieces"); out.Uint64(clip.content ? clip.content->parts.size() : 1);
               out.EndObject();
            }
         ++index;
      }
      out.EndArray(); out.EndObject(); context.Status(wxString::FromUTF8(buffer.GetString()));
      return true;
   }
};
const ComponentInterfaceSymbol GetVideoClipsCommand::Symbol{ XO("GetVideoClips") };
BuiltinCommandsModule::Registration<GetVideoClipsCommand> sGetVideoClips;

class InsertVideoTrackCommand : public AudacityCommand {
   int mTrack{-1};
protected:
   virtual int RelativePosition() const { return 0; }
public:
   TranslatableString GetDescription() const override { return XO("Add an empty video track to the project."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mTrack,wxT("Track"),-1,-1,100000); return true;
   }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool PromptUser(AudacityProject &project) override {
      if (!RelativePosition()) return true;
      int index{};
      for (auto *track : TrackList::Get(project).Any<Track>()) {
         if (track == TrackFocus::Get(project).Get()) { mTrack = index; return true; }
         ++index;
      }
      return false;
   }
   bool Apply(const CommandContext &context) override {
      try {
         Track *anchor{};
         if (RelativePosition()) {
            int index{};
            for (auto *track : TrackList::Get(context.project).Any<Track>())
               if (index++ == mTrack) { anchor = track; break; }
            if (!anchor && mTrack < 0) anchor = TrackFocus::Get(context.project).Get();
            if (!anchor) throw std::runtime_error("Select an anchor track or supply its Track index.");
         }
         VideoTimeline::CreateProjectVideoTrack(context.project,anchor,RelativePosition() < 0);
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
class NewVideoTrackCommand final : public InsertVideoTrackCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
class AddVideoTrackBeforeCommand final : public InsertVideoTrackCommand {
   int RelativePosition() const override { return -1; }
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
class AddVideoTrackAfterCommand final : public InsertVideoTrackCommand {
   int RelativePosition() const override { return 1; }
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol NewVideoTrackCommand::Symbol{XO("NewVideoTrack")};
const ComponentInterfaceSymbol AddVideoTrackBeforeCommand::Symbol{XO("AddVideoTrackBefore")};
const ComponentInterfaceSymbol AddVideoTrackAfterCommand::Symbol{XO("AddVideoTrackAfter")};
BuiltinCommandsModule::Registration<NewVideoTrackCommand> sNewVideoTrack;
BuiltinCommandsModule::Registration<AddVideoTrackBeforeCommand> sVideoBefore;
BuiltinCommandsModule::Registration<AddVideoTrackAfterCommand> sVideoAfter;

class EditVideoStructureCommand : public AudacityCommand {
protected:
   virtual VideoEditing::VideoEditKind Kind() const = 0;
public:
   TranslatableString GetDescription() const override { return XO("Heal, join or separate selected video clips without rendering their sources."); }
   bool Apply(const CommandContext &context) override {
      try {
         const auto &region = ViewInfo::Get(context.project).selectedRegion;
         VideoTimeline::EditSelectedVideoStructure(context.project, Kind(), region.t0(), region.t1());
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
#define VIDEO_STRUCTURE_COMMAND(Type, Name, Operation) \
class Type final : public EditVideoStructureCommand { \
   VideoEditing::VideoEditKind Kind() const override { return VideoEditing::VideoEditKind::Operation; } \
public: \
   static const ComponentInterfaceSymbol Symbol; \
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; } \
}; \
const ComponentInterfaceSymbol Type::Symbol{XO(Name)}; \
BuiltinCommandsModule::Registration<Type> s##Type;
VIDEO_STRUCTURE_COMMAND(HealVideoSplitCommand, "HealVideoSplit", HealClips)
VIDEO_STRUCTURE_COMMAND(JoinVideoClipsCommand, "JoinVideoClips", JoinClips)
VIDEO_STRUCTURE_COMMAND(SeparateVideoClipsCommand, "SeparateVideoClips", SeparateClips)
#undef VIDEO_STRUCTURE_COMMAND

using namespace MenuRegistry;
AttachedItem sAddVideoTrack{
   Section("VideoTracks",
      Command("NewVideoTrack",XXO("&Video Track"),CommandDispatch::OnAudacityCommand,AudioIONotBusyFlag()),
      Command("AddVideoTrackBefore",XXO("Video Track Before Focused Track"),CommandDispatch::OnAudacityCommand,
         AudioIONotBusyFlag() | TracksExistFlag()),
      Command("AddVideoTrackAfter",XXO("Video Track After Focused Track"),CommandDispatch::OnAudacityCommand,
         AudioIONotBusyFlag() | TracksExistFlag())),
   wxT("Tracks/Add/Add")
};
AttachedItem sVideoStructureMenu{
   Section("VideoStructure",
      Command("HealVideoSplit", XXO("Heal Video Split"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("SeparateVideoClips", XXO("Separate Joined Video"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())),
   wxT("Edit/Other/Clip")
};
AttachedItem sVideoClipScripting{
   Items("",
      Command("JoinVideoClips", XXO("Join Video Clips"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("SplitVideo", XXO("Split Video at Cursor"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("GetVideoClips", XXO("Get Video Clips"), CommandDispatch::OnAudacityCommand, AlwaysEnabledFlag),
      Command("MoveVideoClip", XXO("Move Video Clip..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("DeleteVideoClip", XXO("Delete Video Clip..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())),
   wxT("Optional/Extra/Part2/Scriptables2")
};
}

VideoTimeline::Timeline VideoTimeline::GetProjectTimeline(const AudacityProject &project)
{
   const auto tracks = ProjectVideoTracks(const_cast<AudacityProject&>(project));
   auto result = ComposeVideoTracks(GetProjectVisibleVideoLayers(project));
   // A fully hidden composition still has a canvas and media identity. Export
   // renders black for the separately captured project duration, without opening
   // hidden sources or changing any native audio mute/solo state.
   if (result.sourcePath.empty()) for (const auto *track : tracks) {
      if (track->Timeline().clips.empty()) continue;
      const auto source = SourceFor(track->Timeline(), track->Timeline().clips.front());
      result.sourcePath = source.path; result.sourceDuration = source.duration;
      result.frameRate = source.frameRate; result.sourceWidth = source.width; result.sourceHeight = source.height;
      break;
   }
   for (const auto *track : tracks) for (const auto &clip : track->Timeline().clips)
      for (const auto &source : VideoContentSources(clip, SourceFor(track->Timeline(), clip)))
         result.protectedSourcePaths.push_back(source.path);
   return result;
}

bool VideoTimeline::EditSelectedVideoStructure(AudacityProject &project, VideoEditing::VideoEditKind kind,
   double from, double to, bool recordHistory)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing video clips.");
   VideoEditing::VideoProjectSnapshot snapshot;
   std::vector<AuTopsyVideoTrack*> targets;
   for (auto *track : ProjectVideoTracks(project)) if (track->GetSelected()) {
      const auto id = std::to_string(targets.size());
      targets.push_back(track); snapshot.tracks.push_back({id, 0, track->Timeline()});
   }
   if (targets.empty()) return false;
   // Each selected track gets its own semantic request, checked as a whole batch.
   std::vector<VideoEditing::VideoEditRequest> requests;
   for (const auto &track : snapshot.tracks) requests.push_back({kind, track.id, 0, from, to});
   const auto batch = VideoEditing::PlanVideoEdits(snapshot, requests);
   if (batch.ChangedTrackIds().empty()) return false;
   try {
      for (size_t i{}; i < targets.size(); ++i) targets[i]->SetTimeline(batch.After().tracks[i].timeline);
      if (recordHistory) {
         const auto description = kind == VideoEditing::VideoEditKind::HealClips ? XO("Healed video split")
            : kind == VideoEditing::VideoEditKind::JoinClips ? XO("Joined video clips") : XO("Separated video clips");
         ProjectHistory::Get(project).PushState(description, description);
      }
   }
   catch (...) {
      for (size_t i{}; i < targets.size(); ++i) targets[i]->SetTimeline(snapshot.tracks[i].timeline);
      throw;
   }
   Viewport::Get(project).HandleResize(); RefreshPreviewLater(project); return true;
}

bool VideoTimeline::SplitProjectVideo(AudacityProject &project, double at)
{
   auto &track = FindClipTrack(project, 0);
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing video clips.");
   if (!ClipGroups::SplitMediaAtSelection(project, at, at, &track)) return false;
   try { ProjectHistory::Get(project).PushState(XO("Split video clip"), XO("Split")); }
   catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
   Viewport::Get(project).HandleResize();
   RefreshPreviewLater(project);
   return true;
}
bool VideoTimeline::MoveProjectVideoClip(AudacityProject &project, uint64_t id, double start)
{
   auto &track = FindClipTrack(project, id);
   return MoveVideoClipBetweenTracks(project, track, track, id, start);
}
bool VideoTimeline::RemoveProjectVideoClip(AudacityProject &project, uint64_t id)
{
   auto &track = FindClipTrack(project, 0);
   return EditProjectTimeline(project, XO("Deleted video clip"),
      [&](auto &timeline) { return RemoveClip(timeline, id); }, &track);
}
