#include "EditClipMarkers.h"
#include "NavigateClipFlags.h"
#include "GuardMediaDialogs.h"
#include "GroupAudioClips.h"
#include "../video/CreateVideoTrack.h"
#include "WaveClip.h"
#include "WaveTrack.h"
#include "XMLWriter.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectHistory.h"
#include "ProjectWindow.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "TrackFocus.h"
#include "SelectUtilities.h"
#include "CommandContext.h"
#include "MenuRegistry.h"
#include "SettingsVisitor.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <wx/choicdlg.h>
#include <wx/textdlg.h>
#include <wx/dc.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <map>
#include <tuple>
namespace {
struct MarkerAttachment final : WaveClipListener {
   std::vector<ClipMarkers::Marker> markers;
   std::unique_ptr<WaveClipListener> Clone() const override
   { return std::make_unique<MarkerAttachment>(*this); }
   void MarkChanged() noexcept override {}
   void Invalidate() override {}
   void WriteXMLAttributes(XMLWriter &writer) const override {
      if (!markers.empty()) writer.WriteAttr("autopsyMarkers", wxString::FromUTF8(ClipMarkers::EncodeMarkers(markers)));
   }
   bool HandleXMLAttribute(const std::string_view &name, const XMLAttributeValueView &value) override {
      if (name != "autopsyMarkers" || !value.IsStringView()) return false;
      try {
         const auto valueText = wxString(value.ToWString());
         const auto text = valueText.ToUTF8();
         markers = ClipMarkers::DecodeMarkers({text.data(), text.length()}); return true;
      }
      catch (...) { return false; }
   }
};
const WaveClip::Attachments::RegisteredFactory sMarkerKey{
   [](WaveClip &) { return std::make_unique<MarkerAttachment>(); }
};
MarkerAttachment &For(WaveClip &clip) { return clip.Attachments::Get<MarkerAttachment>(sMarkerKey); }
struct ClipRef {
   Track *track{};
   WaveClip *audio{};
   AuTopsyVideoTrack *video{};
   uint64_t id{};
   size_t trackIndex{}, clipIndex{};
   double start{}, end{}, origin{}, speed{1};
   std::vector<ClipMarkers::Marker> markers;
};
std::vector<ClipRef> Clips(AudacityProject &project)
{
   std::vector<ClipRef> clips; size_t trackIndex{};
   for (auto *track : TrackList::Get(project)) {
      size_t index{};
      if (auto *audio = dynamic_cast<WaveTrack*>(track))
         for (const auto &clip : audio->SortedIntervalArray())
            clips.push_back({track, clip.get(), nullptr, 0, trackIndex, index++,
               clip->GetPlayStartTime(), clip->GetPlayEndTime(),
               clip->GetSequenceStartTime(), clip->GetStretchRatio(), ClipMarkers::AudioMarkers(*clip)});
      else if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track))
         for (const auto &clip : video->Timeline().clips)
            clips.push_back({track, nullptr, video, clip.id, trackIndex, index++,
               clip.start, clip.End(), clip.ProjectTimeAtContentTime(0), 1 / clip.playbackRate, clip.markers});
      ++trackIndex;
   }
   return clips;
}
bool Store(AudacityProject &project, ClipRef clip, std::vector<ClipMarkers::Marker> markers, bool remove)
{
   ClipMarkers::RequireUsableMarkers(markers);
   const auto old = clip.markers;
   VideoTimeline::Timeline original;
   if (clip.video) original = clip.video->Timeline();
   try {
      if (clip.audio) For(*clip.audio).markers = std::move(markers);
      else {
         auto changed = original;
         const auto found = std::find_if(changed.clips.begin(), changed.clips.end(),
            [&](const auto &value) { return value.id == clip.id; });
         if (found == changed.clips.end()) throw std::runtime_error("The clip was removed.");
         found->markers = std::move(markers); clip.video->SetTimeline(std::move(changed));
      }
      ProjectHistory::Get(project).PushState(remove ? XO("Removed clip flag") : XO("Added or edited clip flag"),
         remove ? XO("Remove Clip Flag") : XO("Clip Flag"));
   }
   catch (...) {
      if (clip.audio) For(*clip.audio).markers = old;
      else clip.video->SetTimeline(std::move(original));
      throw;
   }
   Viewport::Get(project).Redraw(); return true;
}
// These identities live only for a navigator invocation. Commands are held by
// the modal guard; actions still find the current native object before editing.
struct FlagTarget {
   Track *track{}; WaveClip *audio{}; uint64_t clipId{};
   std::string id; uint64_t key{};
   FlagTarget(const ClipRef &clip, std::string markerId, uint64_t occurrence)
      : track(clip.track), audio(clip.audio), clipId(clip.id), id(std::move(markerId)), key(occurrence) {}
};
ClipRef CurrentClip(AudacityProject &project, const FlagTarget &target)
{
   for (auto clip : Clips(project))
      if (clip.track == target.track && clip.audio == target.audio && clip.id == target.clipId) return clip;
   throw std::runtime_error("The flagged clip was removed. Reopen the flag navigator.");
}
ClipMarkers::Marker CurrentMarker(const ClipRef &clip, const std::string &id)
{
   for (const auto &marker : clip.markers) if (marker.id == id) return marker;
   throw std::runtime_error("The clip flag was removed.");
}
void JumpToFlag(AudacityProject &project, const FlagTarget &target)
{
   const auto clip = CurrentClip(project, target);
   const auto marker = CurrentMarker(clip, target.id);
   const auto time = ClipMarkers::ProjectMarkerTime(marker, clip.origin, clip.speed);
   if (time < clip.start || time >= clip.end)
      throw std::runtime_error("This flag is outside the clip's visible part. Untrim the clip to jump to it.");
   SelectUtilities::SelectNone(project); clip.track->SetSelected(true);
   TrackFocus::Get(project).Set(clip.track, false);
   ViewInfo::Get(project).selectedRegion.setTimes(time, time);
   Viewport::Get(project).ScrollIntoView(time); Viewport::Get(project).Redraw();
}
bool RenameFlag(AudacityProject &project, const FlagTarget &target, const wxString &text)
{
   auto clip = CurrentClip(project, target);
   auto marker = CurrentMarker(clip, target.id);
   const auto utf8 = text.ToUTF8(); marker.text.assign(utf8.data(), utf8.length());
   auto changed = clip.markers;
   if (!ClipMarkers::PutMarker(changed, std::move(marker))) return false;
   return Store(project, std::move(clip), std::move(changed), false);
}
void ShowFlagNavigator(AudacityProject &project)
{
   ClipActions::MediaDialogCommandGuard commands;
   wxDialog dialog(&ProjectWindow::Get(project), wxID_ANY, _("Clip Flags"), wxDefaultPosition,
      wxSize(760, 440), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
   auto *layout = new wxBoxSizer(wxVERTICAL);
   auto *search = new wxTextCtrl(&dialog, wxID_ANY);
   search->SetName(_("Search clip flags")); search->SetHint(_("Search flag, track or clip name"));
   layout->Add(search, 0, wxEXPAND | wxALL, 8);
   auto *hidden = new wxCheckBox(&dialog, wxID_ANY, _("Include flags outside trimmed clip edges"));
   layout->Add(hidden, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
   auto *list = new wxListCtrl(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize,
      wxLC_REPORT | wxLC_SINGLE_SEL);
   list->SetName(_("Clip flag list"));
   list->InsertColumn(0, _("Time"), wxLIST_FORMAT_LEFT, 100);
   list->InsertColumn(1, _("Track"), wxLIST_FORMAT_LEFT, 155);
   list->InsertColumn(2, _("Clip"), wxLIST_FORMAT_LEFT, 155);
   list->InsertColumn(3, _("Flag"), wxLIST_FORMAT_LEFT, 230);
   list->InsertColumn(4, _("State"), wxLIST_FORMAT_LEFT, 90);
   layout->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
   auto *status = new wxStaticText(&dialog, wxID_ANY, _("Flags follow their clips. Jump keeps the current zoom."));
   layout->Add(status, 0, wxALL, 8);
   auto *buttons = new wxBoxSizer(wxHORIZONTAL);
   const auto button = [&](const wxString &label) {
      auto *value = new wxButton(&dialog, wxID_ANY, label); buttons->Add(value, 0, wxRIGHT, 6); return value;
   };
   auto *previous = button(_("Previous")), *next = button(_("Next")), *jump = button(_("Jump"));
   auto *rename = button(_("Rename...")), *remove = button(_("Delete Flag"));
   buttons->AddStretchSpacer(); buttons->Add(new wxButton(&dialog, wxID_CANCEL, _("Close")));
   layout->Add(buttons, 0, wxEXPAND | wxALL, 8); dialog.SetSizer(layout);
   // Object-address tokens are adapter-local and never persisted or displayed.
   using Identity = std::tuple<uintptr_t, uintptr_t, uint64_t, std::string>;
   std::map<Identity, uint64_t> keys;
   std::map<uint64_t, FlagTarget> targets;
   std::vector<ClipMarkers::FlagLocation> locations;
   const auto selectedKey = [&]() -> uint64_t {
      const auto row = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
      return row < 0 ? 0 : list->GetItemData(row);
   };
   const auto updateButtons = [&]() {
      const auto key = selectedKey(); const auto found = targets.find(key);
      rename->Enable(found != targets.end()); remove->Enable(found != targets.end());
      jump->Enable(found != targets.end() && std::any_of(locations.begin(), locations.end(),
         [key](const auto &flag) { return flag.occurrence == key && flag.visible; }));
      const auto cursor = ViewInfo::Get(project).selectedRegion.t0();
      previous->Enable(ClipMarkers::AdjacentClipFlag(locations, cursor, false).has_value());
      next->Enable(ClipMarkers::AdjacentClipFlag(locations, cursor, true).has_value());
   };
   const auto refresh = [&]() {
      const auto selected = selectedKey();
      list->Freeze(); list->DeleteAllItems(); targets.clear(); locations.clear();
      const auto query = search->GetValue().Lower();
      struct Row { wxString track, clip, text; };
      std::map<uint64_t, Row> rows;
      for (const auto &clip : Clips(project)) for (const auto &marker : clip.markers) {
         const auto time = ClipMarkers::ProjectMarkerTime(marker, clip.origin, clip.speed);
         const bool visible = time >= clip.start && time < clip.end;
         if (!visible && !hidden->GetValue()) continue;
         const auto trackName = wxString::Format("%zu. %s", clip.trackIndex + 1, clip.track->GetName());
         const auto clipName = clip.audio ? clip.audio->GetName() :
            (clip.video->Timeline().clips.at(clip.clipIndex).appearance.name.empty() ? _("Video clip") :
               wxString::FromUTF8(clip.video->Timeline().clips.at(clip.clipIndex).appearance.name));
         const auto text = wxString::FromUTF8(marker.text);
         if (!(trackName + " " + clipName + " " + text).Lower().Contains(query)) continue;
         const Identity identity{reinterpret_cast<uintptr_t>(clip.track), reinterpret_cast<uintptr_t>(clip.audio), clip.id, marker.id};
         const auto key = keys.emplace(identity, keys.size() + 1).first->second;
         targets.emplace(key, FlagTarget{clip, marker.id, key});
         rows.emplace(key, Row{trackName, clipName, text}); locations.push_back({key, time, visible});
      }
      locations = ClipMarkers::OrderClipFlags(std::move(locations));
      for (const auto &location : locations) {
         const auto &row = rows.at(location.occurrence);
         const auto index = list->InsertItem(list->GetItemCount(), wxString::Format("%.3f s", location.time));
         list->SetItem(index, 1, row.track); list->SetItem(index, 2, row.clip); list->SetItem(index, 3, row.text);
         list->SetItem(index, 4, location.visible ? _("Visible") : _("Trimmed out"));
         list->SetItemData(index, static_cast<long>(location.occurrence));
         if (location.occurrence == selected) list->SetItemState(index, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
      }
      if (!selectedKey() && list->GetItemCount()) list->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
      list->Thaw(); updateButtons();
   };
   const auto act = [&](const auto &operation) {
      try { operation(); }
      catch (const std::exception &error) { wxMessageBox(wxString::FromUTF8(error.what()), _("Clip Flags"), wxOK | wxICON_ERROR, &dialog); }
   };
   const auto jumpSelected = [&]() { act([&] {
      const auto found = targets.find(selectedKey()); if (found == targets.end()) return;
      JumpToFlag(project, found->second); updateButtons();
   }); };
   search->Bind(wxEVT_TEXT, [&](wxCommandEvent &) { refresh(); });
   hidden->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent &) { refresh(); });
   list->Bind(wxEVT_LIST_ITEM_SELECTED, [&](wxListEvent &) { updateButtons(); });
   list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [&](wxListEvent &) { jumpSelected(); });
   jump->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { jumpSelected(); });
   rename->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { act([&] {
      const auto found = targets.find(selectedKey()); if (found == targets.end()) return;
      const auto target = found->second;
      const auto marker = CurrentMarker(CurrentClip(project, target), target.id);
      wxTextEntryDialog edit(&dialog, _("Flag label"), _("Rename Clip Flag"), wxString::FromUTF8(marker.text));
      if (edit.ShowModal() == wxID_OK) { RenameFlag(project, target, edit.GetValue()); refresh(); }
   }); });
   remove->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { act([&] {
      const auto found = targets.find(selectedKey()); if (found == targets.end()) return;
      auto clip = CurrentClip(project, found->second); auto markers = clip.markers;
      const auto id = found->second.id;
      markers.erase(std::remove_if(markers.begin(), markers.end(), [&](const auto &m) { return m.id == id; }), markers.end());
      if (markers.size() != clip.markers.size()) Store(project, std::move(clip), std::move(markers), true);
      refresh();
   }); });
   for (auto *control : {previous, next}) control->Bind(wxEVT_BUTTON, [&, control](wxCommandEvent &) { act([&] {
      const auto key = ClipMarkers::AdjacentClipFlag(locations, ViewInfo::Get(project).selectedRegion.t0(), control == next);
      if (!key) return;
      for (long i{}; i < list->GetItemCount(); ++i) if (list->GetItemData(i) == *key) {
         list->SetItemState(i, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED); list->EnsureVisible(i); break;
      }
      JumpToFlag(project, targets.at(*key)); updateButtons();
   }); });
   refresh(); dialog.ShowModal();
}
class ShowClipFlagsCommand final : public AudacityCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Find, jump to, rename and delete clip flags."); }
   bool Apply(const CommandContext &context) override { ShowFlagNavigator(context.project); return true; }
};
const ComponentInterfaceSymbol ShowClipFlagsCommand::Symbol{XO("ShowClipFlags")};
BuiltinCommandsModule::Registration<ShowClipFlagsCommand> sShowFlags;
void NavigateProjectFlag(AudacityProject &project, bool forward)
{
      std::map<uint64_t, FlagTarget> targets; std::vector<ClipMarkers::FlagLocation> flags;
      for (const auto &clip : Clips(project)) for (const auto &marker : clip.markers) {
         const auto key = targets.size() + 1;
         const auto time = ClipMarkers::ProjectMarkerTime(marker, clip.origin, clip.speed);
         targets.emplace(key, FlagTarget{clip, marker.id, key}); flags.push_back({key, time, time >= clip.start && time < clip.end});
      }
      const auto key = ClipMarkers::AdjacentClipFlag(flags, ViewInfo::Get(project).selectedRegion.t0(), forward);
      if (key) JumpToFlag(project, targets.at(*key));
}
class AddClipMarkerCommand final : public AudacityCommand {
   int mTrack{-1};
   double mAt{-1};
   wxString mText, mId;
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Attach a flag to an audio or video clip."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mTrack, wxT("Track"), -1, -1, 100000);
      v.Define(mAt, wxT("At"), -1.0, -1.0, 100000000.0);
      v.Define(mText, wxT("Text"), wxString{});
      v.Define(mId, wxT("Id"), wxString{}); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      mAt = ViewInfo::Get(project).selectedRegion.t0();
      std::vector<ClipRef> choices; wxArrayString labels;
      for (const auto &clip : Clips(project)) if (clip.start <= mAt && mAt < clip.end) {
         choices.push_back(clip);
         labels.Add(wxString::Format("%zu. %s / %s", clip.trackIndex + 1, clip.track->GetName(),
            clip.video ? _("Video") : _("Audio")));
      }
      if (choices.empty()) return false;
      wxSingleChoiceDialog choice{&ProjectWindow::Get(project), _("Choose the clip at the cursor."),
         _("Add Clip Flag"), labels};
      if (choice.ShowModal() != wxID_OK) return false;
      mTrack = int(choices.at(choice.GetSelection()).trackIndex);
      wxTextEntryDialog text{&ProjectWindow::Get(project), _("Flag label"), _("Add Clip Flag")};
      if (text.ShowModal() != wxID_OK) return false;
      mText = text.GetValue(); mId.clear(); return true;
   }
   bool Apply(const CommandContext &context) override {
      try {
         if (ProjectAudioIO::Get(context.project).IsAudioActive())
            throw std::runtime_error("Stop playback or recording before editing clip flags.");
         const auto at = mAt < 0 ? ViewInfo::Get(context.project).selectedRegion.t0() : mAt;
         const auto clips = Clips(context.project);
         std::vector<ClipRef> matches;
         for (const auto &clip : clips)
            if ((mTrack >= 0 ? clip.trackIndex == size_t(mTrack) : clip.track->GetSelected()) &&
                clip.start <= at && at < clip.end) matches.push_back(clip);
         if (matches.size() != 1) throw std::runtime_error("Choose one clip at this time, or supply Track.");
         auto clip = matches.front();
         const auto text = mText.ToUTF8(); const std::string label{text.data(), text.length()};
         const auto position = (at - clip.origin) / clip.speed;
         std::string id;
         if (mId.empty()) {
            const auto found = std::find_if(clip.markers.begin(), clip.markers.end(), [&](const auto &marker) {
               return std::abs(marker.position - position) < 1e-10 && marker.text == label;
            });
            if (found != clip.markers.end()) return true;
            id = ClipGroups::CreateClipGroupId();
         }
         else { const auto bytes = mId.ToUTF8(); id.assign(bytes.data(), bytes.length()); }
         auto changed = clip.markers;
         if (!ClipMarkers::PutMarker(changed, {id, position, label})) return true;
         return Store(context.project, std::move(clip), std::move(changed), false);
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol AddClipMarkerCommand::Symbol{XO("AddClipMarker")};
BuiltinCommandsModule::Registration<AddClipMarkerCommand> sAddMarker;
class RemoveClipMarkerCommand final : public AudacityCommand {
   int mTrack{}, mClip{};
   wxString mId;
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Remove a saved clip flag."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mTrack, wxT("Track"), 0, 0, 100000);
      v.Define(mClip, wxT("Clip"), 0, 0, 100000);
      v.Define(mId, wxT("Id"), wxString{}); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      struct Choice { size_t track, clip; std::string id; };
      std::vector<Choice> choices; wxArrayString labels;
      for (const auto &clip : Clips(project)) for (const auto &marker : clip.markers) {
         choices.push_back({clip.trackIndex, clip.clipIndex, marker.id});
         labels.Add(wxString::Format("%zu. %s / %.3f / %s", clip.trackIndex + 1,
            clip.track->GetName(), ClipMarkers::ProjectMarkerTime(marker, clip.origin, clip.speed),
            wxString::FromUTF8(marker.text)));
      }
      if (choices.empty()) return false;
      wxSingleChoiceDialog dialog{&ProjectWindow::Get(project), _("Choose the flag to remove."),
         _("Remove Clip Flag"), labels};
      if (dialog.ShowModal() != wxID_OK) return false;
      const auto choice = choices.at(dialog.GetSelection());
      mTrack = int(choice.track); mClip = int(choice.clip); mId = wxString::FromUTF8(choice.id); return true;
   }
   bool Apply(const CommandContext &context) override {
      try {
         if (ProjectAudioIO::Get(context.project).IsAudioActive())
            throw std::runtime_error("Stop playback or recording before editing clip flags.");
         if (mId.empty()) throw std::runtime_error("Supply the flag Id.");
         for (auto clip : Clips(context.project)) if (clip.trackIndex == size_t(mTrack) && clip.clipIndex == size_t(mClip)) {
            auto changed = clip.markers;
            changed.erase(std::remove_if(changed.begin(), changed.end(),
               [&](const auto &marker) { return wxString::FromUTF8(marker.id) == mId; }), changed.end());
            if (changed.size() == clip.markers.size()) return true;
            return Store(context.project, std::move(clip), std::move(changed), true);
         }
         throw std::runtime_error("Choose an existing clip.");
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol RemoveClipMarkerCommand::Symbol{XO("RemoveClipMarker")};
BuiltinCommandsModule::Registration<RemoveClipMarkerCommand> sRemoveMarker;
class GetClipMarkersCommand final : public AudacityCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("List flags and their current timeline positions."); }
   bool Apply(const CommandContext &context) override {
      rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out{buffer};
      out.StartArray();
      for (const auto &clip : Clips(context.project)) for (const auto &marker : clip.markers) {
         const auto time = ClipMarkers::ProjectMarkerTime(marker, clip.origin, clip.speed);
         out.StartObject(); out.Key("track"); out.Uint64(clip.trackIndex);
         out.Key("clip"); out.Uint64(clip.clipIndex); out.Key("id"); out.String(marker.id.c_str());
         out.Key("kind"); out.String(clip.video ? "video" : "audio"); out.Key("time"); out.Double(time);
         out.Key("visible"); out.Bool(clip.start <= time && time < clip.end);
         out.Key("text"); out.String(marker.text.c_str()); out.EndObject();
      }
      out.EndArray(); context.Status(wxString::FromUTF8(buffer.GetString())); return true;
   }
};
const ComponentInterfaceSymbol GetClipMarkersCommand::Symbol{XO("GetClipMarkers")};
BuiltinCommandsModule::Registration<GetClipMarkersCommand> sGetMarkers;
using namespace MenuRegistry;
AttachedItem sMarkerMenu{
   Section("ClipFlags",
      Command("ShowClipFlags", XXO("Clip Flags..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("PreviousClipFlag", XXO("Previous Clip Flag"),
         [](const CommandContext &context) { NavigateProjectFlag(context.project, false); }, AudioIONotBusyFlag()),
      Command("NextClipFlag", XXO("Next Clip Flag"),
         [](const CommandContext &context) { NavigateProjectFlag(context.project, true); }, AudioIONotBusyFlag()),
      Command("AddClipMarker", XXO("Add Clip Flag..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("RemoveClipMarker", XXO("Remove Clip Flag..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())),
   wxT("Edit/Other/Clip")
};
AttachedItem sMarkerInfo{
   Command("GetClipMarkers", XXO("Get Clip Flags"), CommandDispatch::OnAudacityCommand, AlwaysEnabledFlag),
   wxT("Optional/Extra/Part2/Scriptables2")
};
}
namespace ClipMarkers {
std::string EncodeMarkers(const std::vector<Marker> &markers)
{
   RequireUsableMarkers(markers);
   rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out{buffer};
   out.StartArray();
   for (const auto &marker : markers) {
      out.StartObject(); out.Key("id"); out.String(marker.id.c_str());
      out.Key("position"); out.Double(marker.position); out.Key("text"); out.String(marker.text.c_str()); out.EndObject();
   }
   out.EndArray(); return buffer.GetString();
}
std::vector<Marker> DecodeMarkers(const std::string &text)
{
   if (text.size() > 128 * 1024) throw std::runtime_error("Saved clip flags exceed their size limit.");
   rapidjson::Document data;
   if (data.Parse(text.c_str()).HasParseError() || !data.IsArray() || data.Size() > 128)
      throw std::runtime_error("Saved clip flags are malformed.");
   std::vector<Marker> markers;
   for (const auto &value : data.GetArray()) {
      if (!value.IsObject() || !value.HasMember("id") || !value["id"].IsString() ||
          !value.HasMember("position") || !value["position"].IsNumber() ||
          !value.HasMember("text") || !value["text"].IsString())
         throw std::runtime_error("Saved clip flag fields are malformed.");
      markers.push_back({{value["id"].GetString(), value["id"].GetStringLength()}, value["position"].GetDouble(),
         {value["text"].GetString(), value["text"].GetStringLength()}});
   }
   RequireUsableMarkers(markers); return markers;
}
const std::vector<Marker> &AudioMarkers(const WaveClip &clip) { return For(const_cast<WaveClip&>(clip)).markers; }
void DrawClipMarkerFlags(wxDC &dc, const wxRect &rect, const ZoomInfo &zoom,
   const std::vector<Marker> &markers, double origin, double speed, double from, double to)
{
   wxDCClipper clipping{dc, rect};
   for (const auto &marker : markers) {
      const auto time = ProjectMarkerTime(marker, origin, speed);
      if (time < from || time >= to) continue;
      const auto position = zoom.TimeToPosition(time, rect.x);
      if (position < rect.x || position >= rect.GetRight()) continue;
      const int x = int(position), y = rect.y + 2;
      dc.SetPen(wxPen(wxColour(255, 205, 64)));
      dc.SetBrush(wxBrush(wxColour(255, 205, 64)));
      dc.DrawLine(x, y, x, rect.GetBottom());
      const wxPoint points[]{{x,y},{x+9,y},{x,y+9}};
      dc.DrawPolygon(3, points);
      const auto text = wxString::FromUTF8(marker.text);
      const auto size = dc.GetTextExtent(text);
      dc.DrawRectangle(x+3, y, size.x+4, size.y);
      dc.SetTextForeground(*wxBLACK); dc.DrawText(text, x+5, y);
   }
}
void DrawAudioClipMarkers(const WaveClip &clip, wxDC &dc, const ZoomInfo &zoom, const wxRect &rect)
{
   DrawClipMarkerFlags(dc, rect, zoom, AudioMarkers(clip), clip.GetSequenceStartTime(),
      clip.GetStretchRatio(), clip.GetPlayStartTime(), clip.GetPlayEndTime());
}
}
