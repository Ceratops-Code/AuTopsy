// Native adapters for project attachment/XML, undo, commands and the clip editor.
// Picture files remain external references; project XML owns only clip metadata.
#include "EditVideoTimeline.h"
#include "EditVideoComposition.h"
#include "ExportVideoMix.h"
#include <wx/filedlg.h>
#include <wx/textdlg.h>
#include <wx/filename.h>
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
#include <limits>

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

class SetVideoClipSpeedCommand final : public AudacityCommand {
   wxString mId;
   int mTrack{-1};
   double mSpeed{1};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Set video clip speed (1 is normal), including linked media."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   {
      visitor.Define(mId, wxT("Id"), wxString{});
      visitor.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      visitor.Define(mSpeed, wxT("Speed"), 1.0, 0.01, 100.0);
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override
   {
      try {
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id) throw std::runtime_error("Supply a video clip Id from GetVideoClips.");
         auto &track = FindClipTrack(context.project, id, mTrack);
         ClipGroups::ChangeLinkedVideoClipSpeed(context.project, track, id, mSpeed);
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetVideoClipSpeedCommand::Symbol{ XO("SetVideoClipSpeed") };
BuiltinCommandsModule::Registration<SetVideoClipSpeedCommand> sSetVideoClipSpeed;

class SetVideoClipBoundsCommand final : public AudacityCommand {
   wxString mId;
   int mTrack{-1};
   double mStart{}, mEnd{};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Trim or restore video clip edges at project times, including linked media."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mId, wxT("Id"), wxString{});
      visitor.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      visitor.Define(mStart, wxT("Start"), 0.0, 0.0, 100000000.0);
      visitor.Define(mEnd, wxT("End"), 0.0, 0.0, 100000000.0);
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         if (ProjectAudioIO::Get(context.project).IsAudioActive())
            throw std::runtime_error("Stop playback or recording before changing clip edges.");
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id) throw std::runtime_error("Supply a video clip Id from GetVideoClips.");
         auto &track = FindClipTrack(context.project, id, mTrack);
         auto candidate = track.Timeline();
         VideoTimeline::TrimVideoClip(candidate, id, mStart, mEnd);
         ClipGroups::CommitVideoClipTrim(context.project, track, std::move(candidate));
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetVideoClipBoundsCommand::Symbol{XO("SetVideoClipBounds")};
BuiltinCommandsModule::Registration<SetVideoClipBoundsCommand> sSetVideoClipBounds;


class SetVideoClipCropCommand final : public AudacityCommand {
   wxString mId;
   int mTrack{-1};
   double mLeft{}, mTop{}, mRight{}, mBottom{};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override {
      return XO("Crop the picture edges using fractions from zero to less than one.");
   }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mId, wxT("Id"), wxString{});
      visitor.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      visitor.Define(mLeft, wxT("Left"), 0.0, 0.0, 1.0);
      visitor.Define(mTop, wxT("Top"), 0.0, 0.0, 1.0);
      visitor.Define(mRight, wxT("Right"), 0.0, 0.0, 1.0);
      visitor.Define(mBottom, wxT("Bottom"), 0.0, 0.0, 1.0);
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id)
            throw std::runtime_error("Supply a video clip Id from GetVideoClips.");
         auto &track = FindClipTrack(context.project, id, mTrack);
         VideoEditing::ChangeProjectVideoClipCrop(
            context.project, track, id, {mLeft, mTop, mRight, mBottom});
         RefreshPreviewLater(context.project);
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetVideoClipCropCommand::Symbol{XO("SetVideoClipCrop")};
BuiltinCommandsModule::Registration<SetVideoClipCropCommand> sSetVideoClipCrop;

class SetVideoClipMuteCommand final : public AudacityCommand {
   wxString mId;
   int mTrack{-1};
   bool mMuted{true};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override {
      return XO("Hide one video clip's picture without changing its separate audio.");
   }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mId, wxT("Id"), wxString{});
      visitor.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      visitor.Define(mMuted, wxT("Muted"), true);
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id)
            throw std::runtime_error("Supply a video clip Id from GetVideoClips.");
         auto &track = FindClipTrack(context.project, id, mTrack);
         VideoEditing::ChangeProjectVideoClipMute(context.project, track, id, mMuted);
         RefreshPreviewLater(context.project);
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetVideoClipMuteCommand::Symbol{XO("SetVideoClipMute")};
BuiltinCommandsModule::Registration<SetVideoClipMuteCommand> sSetVideoClipMute;

class SetVideoEmbeddingCommand final : public AudacityCommand {
   int mTrack{-1}, mParent{-1};
   double mX{.65}, mY{.05}, mWidth{.3}, mHeight{.3};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Embed a video track using stable saved parent and placement properties."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mTrack, wxT("Track"), -1, -1, 100000);
      visitor.Define(mParent, wxT("Parent"), -1, -1, 100000);
      visitor.Define(mX, wxT("X"), .65, 0., 1.); visitor.Define(mY, wxT("Y"), .05, 0., 1.);
      visitor.Define(mWidth, wxT("Width"), .3, .000001, 1.); visitor.Define(mHeight, wxT("Height"), .3, .000001, 1.);
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         auto &track = FindClipTrack(context.project, 0, mTrack);
         auto placement = track.Timeline().placement;
         placement.parentId = mParent < 0 ? std::string{} : FindClipTrack(context.project, 0, mParent).Timeline().placement.id;
         placement.rectangle = mParent < 0 ? VideoEditing::VideoFrameRectangle{} : VideoEditing::VideoFrameRectangle{mX,mY,mWidth,mHeight};
         VideoEditing::ChangeProjectVideoTrackPlacement(context.project, track, placement);
         RefreshPreviewLater(context.project); return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetVideoEmbeddingCommand::Symbol{XO("SetVideoEmbedding")};
BuiltinCommandsModule::Registration<SetVideoEmbeddingCommand> sSetVideoEmbedding;

class ChangeVideoPictureCommand : public AudacityCommand {
   wxString mId;
   int mTrack{-1}, mCopies{1};
   double mAt{-1}, mSeconds{5};
protected:
   virtual VideoEditing::VideoClipPictureAction Action() const = 0;
public:
   TranslatableString GetDescription() const override { return XO("Edit a video clip's picture without changing project audio."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mId, wxT("Id"), wxString{}); v.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      v.Define(mAt, wxT("At"), -1.0, -1.0, 100000000.0);
      // Report invalid scripted values through Apply, without the host opening
      // a modal parameter-error dialog on the scripting thread.
      v.Define(mSeconds, wxT("Seconds"), 5.0, std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
      v.Define(mCopies, wxT("Copies"), 1, std::numeric_limits<int>::lowest(), std::numeric_limits<int>::max());
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id) throw std::runtime_error("Supply a video clip Id from GetVideoClips.");
         auto &track = FindClipTrack(context.project, id, mTrack);
         VideoEditing::ApplyProjectVideoPictureAction(context.project, track, id, Action(),
            mAt < 0 ? ViewInfo::Get(context.project).selectedRegion.t0() : mAt, mSeconds, mCopies);
         RefreshPreviewLater(context.project); return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
#define VIDEO_PICTURE_COMMAND(Type, Name, Kind) \
class Type final : public ChangeVideoPictureCommand { \
   VideoEditing::VideoClipPictureAction Action() const override { return VideoEditing::VideoClipPictureAction::Kind; } \
public: static const ComponentInterfaceSymbol Symbol; \
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; } \
}; \
const ComponentInterfaceSymbol Type::Symbol{XO(Name)}; \
BuiltinCommandsModule::Registration<Type> s##Type;
VIDEO_PICTURE_COMMAND(ReverseVideoPictureCommand, "ReverseVideoPicture", Reverse)
VIDEO_PICTURE_COMMAND(FreezeVideoFrameCommand, "FreezeVideoFrame", Freeze)
VIDEO_PICTURE_COMMAND(RepeatVideoPictureCommand, "RepeatVideoPicture", Repeat)
#undef VIDEO_PICTURE_COMMAND

class AddVideoPictureCommand : public AudacityCommand {
   wxString mFilename, mText;
   double mAt{-1}, mSeconds{5};
   int mWidth{}, mHeight{};
protected:
   virtual bool IsTitle() const = 0;
public:
   TranslatableString GetDescription() const override { return XO("Add a still image or title card on its own video track."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mFilename, wxT("Filename"), wxString{}); v.Define(mText, wxT("Text"), wxString{});
      v.Define(mAt, wxT("At"), -1.0, -1.0, 100000000.0);
      v.Define(mSeconds, wxT("Seconds"), 5.0, std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
      v.Define(mWidth, wxT("Width"), 0, 0, 8192); v.Define(mHeight, wxT("Height"), 0, 0, 8192); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      auto *parent = &ProjectWindow::Get(project);
      if (IsTitle()) {
         wxTextEntryDialog text{parent, _("Title text (white on a black title card):"), _("Add Title Card"),
            mText, wxOK | wxCANCEL | wxCENTRE | wxTE_MULTILINE};
         if (text.ShowModal() != wxID_OK) return false; mText = text.GetValue();
      }
      else {
         wxFileDialog image{parent, _("Add Still Image"), {}, {},
            _("Images (*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff)|*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff"), wxFD_OPEN | wxFD_FILE_MUST_EXIST};
         if (image.ShowModal() != wxID_OK) return false; mFilename = image.GetPath();
      }
      wxTextEntryDialog length{parent, _("Picture length in seconds:"), _("Picture Duration"), wxString::Format("%.8g", mSeconds)};
      if (length.ShowModal() != wxID_OK) return false;
      if (!length.GetValue().ToDouble(&mSeconds)) { wxMessageBox(_("Enter a numeric duration."), _("Add Video Picture"), wxOK | wxICON_ERROR, parent); return false; }
      return true;
   }
   bool Apply(const CommandContext &context) override {
      try {
         const auto at = mAt < 0 ? ViewInfo::Get(context.project).selectedRegion.t0() : mAt;
         int width = mWidth, height = mHeight;
         if (IsTitle()) {
            const auto timeline = VideoTimeline::GetProjectTimeline(context.project);
            if (!width) width = timeline.sourceWidth > 0 ? timeline.sourceWidth : 1280;
            if (!height) height = timeline.sourceHeight > 0 ? timeline.sourceHeight : 720;
            VideoEditing::AddProjectVideoPicture(context.project, VideoTimeline::MakeTitleVideoClip(1, at, mSeconds,
               std::string(mText.ToUTF8()), width, height), _("Title Card"));
         }
         else {
            const auto size = VideoExport::ReadVideoSourcePictureSize(mFilename, &ProjectWindow::Get(context.project));
            if (!size) return false;
            VideoEditing::AddProjectVideoPicture(context.project, VideoTimeline::MakeStillVideoClip(1, at, mSeconds,
               std::string(mFilename.ToUTF8()), size->first, size->second), wxFileName(mFilename).GetName());
         }
         RefreshPreviewLater(context.project); return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
class AddVideoTitleCommand final : public AddVideoPictureCommand {
   bool IsTitle() const override { return true; }
public: static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
class ImportStillImageCommand final : public AddVideoPictureCommand {
   bool IsTitle() const override { return false; }
public: static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol AddVideoTitleCommand::Symbol{XO("AddVideoTitle")};
const ComponentInterfaceSymbol ImportStillImageCommand::Symbol{XO("ImportStillImage")};
BuiltinCommandsModule::Registration<AddVideoTitleCommand> sAddVideoTitle;
BuiltinCommandsModule::Registration<ImportStillImageCommand> sImportStillImage;

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
            const auto &placement = video->Timeline().placement;
            out.Key("layerId"); out.String(placement.id.c_str());
            out.Key("parentLayerId"); out.String(placement.parentId.c_str());
            out.Key("placement"); out.StartObject();
            out.Key("x"); out.Double(placement.rectangle.x); out.Key("y"); out.Double(placement.rectangle.y);
            out.Key("width"); out.Double(placement.rectangle.width); out.Key("height"); out.Double(placement.rectangle.height);
            out.EndObject();
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
                out.Key("playbackRate"); out.Double(clip.playbackRate);
               out.Key("muted"); out.Bool(clip.muted);
               out.Key("fades"); out.StartObject();
               out.Key("inStart"); out.Double(clip.opacity.inStart);
               out.Key("inDuration"); out.Double(clip.opacity.inDuration);
               out.Key("outEnd"); out.Double(clip.opacity.outEnd);
               out.Key("outDuration"); out.Double(clip.opacity.outDuration); out.EndObject();
               out.Key("retainedFades"); out.String(VideoEditing::EncodeVideoOpacityRamps(clip.opacityModifiers).c_str());
               out.Key("crop"); out.StartObject();
               out.Key("left"); out.Double(clip.crop.left); out.Key("top"); out.Double(clip.crop.top);
               out.Key("right"); out.Double(clip.crop.right); out.Key("bottom"); out.Double(clip.crop.bottom);
               out.EndObject();
               out.Key("source"); out.String(source.path.c_str()); out.Key("fps"); out.Double(VideoTimeline::VideoContentFrameRate(clip, source));
               out.Key("joined"); out.Bool(clip.content && clip.content->operation == VideoTimeline::VideoClipContent::Operation::Sequence);
               const char *operations[]{"none", "reverse", "freeze", "image", "title"};
               out.Key("pictureEffect"); out.String(clip.content ? operations[static_cast<int>(clip.content->operation)] : "none");
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
AttachedItem sImportStillImageMenu{
   Command("ImportStillImage", XXO("Still &Image..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
   Placement{wxT("File/Import-Export/Import"), {OrderingHint::After, "ImportVideo"}}
};
AttachedItem sAddVideoTitleMenu{
   Command("AddVideoTitle", XXO("&Title Card..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
   Placement{wxT("File/Import-Export/Import"), {OrderingHint::After, "ImportStillImage"}}
};
AttachedItem sVideoStructureMenu{
   Section("VideoStructure",
      Command("HealVideoSplit", XXO("Heal Video Split"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("SeparateVideoClips", XXO("Separate Joined Video"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())),
   wxT("Edit/Other/Clip")
};
AttachedItem sVideoClipScripting{
   Items("",
      Command("ReverseVideoPicture", XXO("Reverse Video Picture"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("FreezeVideoFrame", XXO("Freeze Video Frame..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("RepeatVideoPicture", XXO("Repeat Video Picture..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("JoinVideoClips", XXO("Join Video Clips"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("SplitVideo", XXO("Split Video at Cursor"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("GetVideoClips", XXO("Get Video Clips"), CommandDispatch::OnAudacityCommand, AlwaysEnabledFlag),
      Command("SetVideoEmbedding", XXO("Set Picture-in-Picture..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("SetVideoClipCrop", XXO("Set Video Clip Crop..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("SetVideoClipMute", XXO("Set Video Clip Mute..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("MoveVideoClip", XXO("Move Video Clip..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("DeleteVideoClip", XXO("Delete Video Clip..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())),
   wxT("Optional/Extra/Part2/Scriptables2")
};
}

VideoTimeline::Timeline VideoTimeline::GetProjectTimeline(const AudacityProject &project)
{
   const auto tracks = ProjectVideoTracks(const_cast<AudacityProject&>(project));
   const auto visibleLayers = GetProjectVisibleVideoLayers(project);
   auto result = ComposeVideoTracks(visibleLayers);
   // An embedded source cannot choose the parent's canvas or default rate.
   if (std::any_of(visibleLayers.begin(), visibleLayers.end(), [](const auto *layer) {
         return !layer->placement.parentId.empty();
      })) for (const auto *layer : visibleLayers) {
      if (!layer->placement.parentId.empty() || layer->clips.empty()) continue;
      const auto source = SourceFor(*layer, layer->clips.front());
      result.sourcePath = source.path; result.sourceDuration = source.duration;
      result.frameRate = source.frameRate; result.sourceWidth = source.width; result.sourceHeight = source.height;
      break;
   }
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
   // A generator's content clock is not the duration of its original movie.
   // Keep its render metadata, but bootstrap preview with derived media rather
   // than pretending that this virtual composition is a source file.
   if (std::any_of(tracks.begin(), tracks.end(), [](const auto *track) {
         return std::any_of(track->Timeline().clips.begin(), track->Timeline().clips.end(), HasGeneratedVideoContent);
      })) result.sourcePath.clear();
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
      const auto id = track->Timeline().placement.id;
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
