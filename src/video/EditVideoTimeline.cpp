// Native adapters for project attachment/XML, undo, commands and the clip editor.
// Picture files remain external references; project XML owns only clip metadata.
#include "EditVideoTimeline.h"
#include "Project.h"
#include "ProjectHistory.h"
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
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/textdlg.h>
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

class ProjectVideoTimeline final : public ClientData::Base, public XMLTagHandler {
   AudacityProject &mProject;
   std::shared_ptr<Timeline> mReading;
public:
   std::shared_ptr<Timeline> current = std::make_shared<Timeline>();
   explicit ProjectVideoTimeline(AudacityProject &project) : mProject(project) {}

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
         current = std::move(mReading);
         RefreshPreviewLater(mProject);
      }
   }
};

const AudacityProject::AttachedObjects::RegisteredFactory sTimelineKey{
   [](AudacityProject &project) { return std::make_shared<ProjectVideoTimeline>(project); }
};
ProjectVideoTimeline &TimelineFor(AudacityProject &project)
{ return project.AttachedObjects::Get<ProjectVideoTimeline>(sTimelineKey); }

const ProjectFileIORegistry::ObjectReaderEntry sReadTimeline{
   "autopsyvideo", [](AudacityProject &project) { return &TimelineFor(project); }
};
const ProjectFileIORegistry::ObjectWriterEntry sWriteTimeline{
   [](const AudacityProject &project, XMLWriter &writer) {
      const auto &timeline = VideoTimeline::GetProjectTimeline(project);
      if (timeline.sourcePath.empty()) return;
      writer.StartTag("autopsyvideo");
      writer.WriteAttr("version", 1);
      writer.WriteAttr("source", wxString::FromUTF8(timeline.sourcePath));
      writer.WriteAttr("duration", timeline.sourceDuration, 17);
      writer.WriteAttr("fps", timeline.frameRate, 17);
      writer.WriteAttr("nextId", static_cast<long long>(timeline.nextId));
      for (const auto &clip : timeline.clips) {
         writer.StartTag("videoclip");
         writer.WriteAttr("id", static_cast<long long>(clip.id));
         writer.WriteAttr("start", clip.start, 17);
         writer.WriteAttr("sourceStart", clip.sourceStart, 17);
         writer.WriteAttr("duration", clip.duration, 17);
         writer.EndTag("videoclip");
      }
      writer.EndTag("autopsyvideo");
   }
};

class VideoUndoState final : public UndoStateExtension {
   const std::shared_ptr<Timeline> mTimeline;
public:
   explicit VideoUndoState(std::shared_ptr<Timeline> timeline) : mTimeline(std::move(timeline)) {}
   void RestoreUndoRedoState(AudacityProject &project) override
   {
      TimelineFor(project).current = mTimeline;
      RefreshPreviewLater(project);
   }
};
const UndoRedoExtensionRegistry::Entry sVideoUndo{
   [](AudacityProject &project) {
      return std::make_shared<VideoUndoState>(TimelineFor(project).current);
   }
};

template<typename Edit>
bool EditProjectTimeline(AudacityProject &project, const TranslatableString &description, Edit edit)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing video clips.");
   auto &state = TimelineFor(project);
   auto changed = std::make_shared<Timeline>(*state.current);
   if (!edit(*changed)) return false;
   VideoTimeline::RequireWellFormedTimeline(*changed);
   const auto previous = state.current;
   state.current = std::move(changed);
   try { ProjectHistory::Get(project).PushState(description, description); }
   catch (...) { state.current = previous; throw; }
   RefreshPreviewLater(project);
   return true;
}

class VideoClipDialog final : public wxDialog {
   AudacityProject &mProject;
   wxListBox *mClips;
   std::vector<uint64_t> mIds;
   void RefreshClips()
   {
      const auto selected = mClips->GetSelection();
      mClips->Clear(); mIds.clear();
      for (const auto &clip : VideoTimeline::GetProjectTimeline(mProject).clips) {
         mIds.push_back(clip.id);
         mClips->Append(wxString::Format("#%llu   %.3f - %.3f s   (source %.3f s)",
            static_cast<unsigned long long>(clip.id), clip.start, clip.End(), clip.sourceStart));
      }
      if (!mIds.empty()) mClips->SetSelection(std::min(std::max(selected, 0), int(mIds.size()) - 1));
   }
   template<typename Edit> void Apply(Edit edit)
   {
      try { edit(); RefreshClips(); }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Video Clips"), wxOK | wxICON_ERROR, this);
      }
   }
public:
   explicit VideoClipDialog(AudacityProject &project)
      : wxDialog(&ProjectWindow::Get(project), wxID_ANY, _("Video Clips"),
           wxDefaultPosition, wxSize(640, 300), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
      , mProject(project), mClips(new wxListBox(this, wxID_ANY))
   {
      auto *layout = new wxBoxSizer(wxVERTICAL);
      layout->Add(mClips, 1, wxEXPAND | wxALL, 8);
      auto *buttons = new wxBoxSizer(wxHORIZONTAL);
      auto *split = new wxButton(this, wxID_ANY, _("Split at Audio Cursor"));
      auto *move = new wxButton(this, wxID_ANY, _("Move Selected..."));
      auto *remove = new wxButton(this, wxID_ANY, _("Delete Selected"));
      buttons->Add(split, 0, wxALL, 4); buttons->Add(move, 0, wxALL, 4);
      buttons->Add(remove, 0, wxALL, 4);
      buttons->Add(new wxButton(this, wxID_CANCEL, _("Close")), 0, wxALL, 4);
      layout->Add(buttons, 0, wxALIGN_CENTER | wxALL, 4);
      SetSizer(layout);
      split->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Apply([&] {
         VideoTimeline::SplitProjectVideo(mProject, ViewInfo::Get(mProject).selectedRegion.t0());
      }); });
      move->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Apply([&] {
         const auto selection = mClips->GetSelection(); if (selection == wxNOT_FOUND) return;
         const auto id = mIds.at(selection);
         wxTextEntryDialog input(this, _("New start time in seconds:"), _("Move Video Clip"));
         if (input.ShowModal() != wxID_OK) return;
         double start{};
         if (!input.GetValue().ToDouble(&start)) throw std::runtime_error("Enter a time in seconds.");
         EditProjectTimeline(mProject, XO("Moved video clip"), [&](auto &timeline) {
            return VideoTimeline::MoveClipTo(timeline, id, start);
         });
      }); });
      remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Apply([&] {
         const auto selection = mClips->GetSelection(); if (selection == wxNOT_FOUND) return;
         const auto id = mIds.at(selection);
         EditProjectTimeline(mProject, XO("Deleted video clip"), [&](auto &timeline) {
            return VideoTimeline::RemoveClip(timeline, id);
         });
      }); });
      RefreshClips();
   }
};

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
protected:
   virtual bool Removing() const { return false; }
public:
   TranslatableString GetDescription() const override { return XO("Move or delete a video clip by its persistent ID."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   {
      visitor.Define(mId, wxT("Id"), wxString{});
      visitor.Define(mStart, wxT("Start"), 0.0, 0.0, 100000000.0);
      return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override
   {
      try {
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id) throw std::runtime_error("Supply a video clip Id from GetVideoClips.");
         EditProjectTimeline(context.project, Removing() ? XO("Deleted video clip") : XO("Moved video clip"),
            [&](auto &timeline) { return Removing() ? VideoTimeline::RemoveClip(timeline, id)
               : VideoTimeline::MoveClipTo(timeline, id, mStart); });
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
      const auto &timeline = VideoTimeline::GetProjectTimeline(context.project);
      rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
      out.StartObject();
      out.Key("source"); out.String(timeline.sourcePath.c_str());
      out.Key("duration"); out.Double(timeline.sourceDuration);
      out.Key("end"); out.Double(timeline.End());
      out.Key("fps"); out.Double(timeline.frameRate);
      out.Key("clips"); out.StartArray();
      for (const auto &clip : timeline.clips) {
         out.StartObject(); out.Key("id"); out.Uint64(clip.id);
         out.Key("start"); out.Double(clip.start); out.Key("end"); out.Double(clip.End());
         out.Key("sourceStart"); out.Double(clip.sourceStart);
         out.Key("duration"); out.Double(clip.duration); out.EndObject();
      }
      out.EndArray(); out.EndObject(); context.Status(wxString::FromUTF8(buffer.GetString()));
      return true;
   }
};
const ComponentInterfaceSymbol GetVideoClipsCommand::Symbol{ XO("GetVideoClips") };
BuiltinCommandsModule::Registration<GetVideoClipsCommand> sGetVideoClips;

using namespace MenuRegistry;
AttachedItem sVideoEditing{
   Section("VideoClips",
      Command("SplitVideo", XXO("Split Video at Cursor"), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("EditVideoClips", XXO("Video Clips..."),
         [](const CommandContext &context) { VideoTimeline::ShowProjectVideoClips(context.project); }, AudioIONotBusyFlag())),
   wxT("Edit/Other/Clip")
};
AttachedItem sVideoClipScripting{
   Items("",
      Command("GetVideoClips", XXO("Get Video Clips"), CommandDispatch::OnAudacityCommand, AlwaysEnabledFlag),
      Command("MoveVideoClip", XXO("Move Video Clip..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("DeleteVideoClip", XXO("Delete Video Clip..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())),
   wxT("Optional/Extra/Part2/Scriptables2")
};
}

const VideoTimeline::Timeline &VideoTimeline::GetProjectTimeline(const AudacityProject &project)
{ return *TimelineFor(const_cast<AudacityProject &>(project)).current; }

void VideoTimeline::AdoptProjectVideo(AudacityProject &project, Timeline timeline, bool importedAudio)
{
   RequireWellFormedTimeline(timeline);
   auto changed = std::make_shared<Timeline>(std::move(timeline));
   auto &state = TimelineFor(project);
   const auto previous = state.current;
   state.current = std::move(changed);
   try {
      if (importedAudio) ProjectHistory::Get(project).ModifyState(true);
      else ProjectHistory::Get(project).PushState(XO("Imported video"), XO("Import Video"));
   }
   catch (...) { state.current = previous; throw; }
}

void VideoTimeline::RemoveProjectVideo(AudacityProject &project)
{
   EditProjectTimeline(project, XO("Removed video reference"), [](auto &timeline) {
      if (timeline.sourcePath.empty()) return false;
      timeline = {}; return true;
   });
}

bool VideoTimeline::SplitProjectVideo(AudacityProject &project, double at)
{
   return EditProjectTimeline(project, XO("Split video clip"), [at](auto &timeline) {
      if (timeline.sourcePath.empty()) throw std::runtime_error("Import a video reference first.");
      return SplitClipAt(timeline, at);
   });
}

bool VideoTimeline::MoveProjectVideoClip(AudacityProject &project, uint64_t id, double start)
{
   return EditProjectTimeline(project, XO("Moved video clip"), [=](auto &timeline) {
      return MoveClipTo(timeline, id, start);
   });
}
bool VideoTimeline::RemoveProjectVideoClip(AudacityProject &project, uint64_t id)
{
   return EditProjectTimeline(project, XO("Deleted video clip"), [=](auto &timeline) {
      return RemoveClip(timeline, id);
   });
}

void VideoTimeline::ShowProjectVideoClips(AudacityProject &project)
{ VideoClipDialog(project).ShowModal(); }
