// Native folder presentation, commands and mix-snapshot adapter. Domain policy
// and streaming peak reduction have no dependency on this native pane layer.
#include "ShowTrackGroups.h"
#include "CreateTrackGroup.h"
#include "RenderGroupWaveform.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectAudioIO.h"
#include "ProjectWindow.h"
#include "TrackPanel.h"
#include "TrackPanelMouseEvent.h"
#include "TrackPanelDrawingContext.h"
#include "TrackArtist.h"
#include "TrackFocus.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "SelectUtilities.h"
#include "WaveTrack.h"
#include "Mix.h"
#include "HitTestResult.h"
#include "UIHandle.h"
#include "RefreshCode.h"
#include "CommandContext.h"
#include "CommonCommandFlags.h"
#include "MenuRegistry.h"
#include "SettingsVisitor.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include "tracks/ui/CommonChannelView.h"
#include "tracks/ui/ChannelVRulerControls.h"
#include "tracks/ui/TrackControls.h"
#include "tracks/ui/CommonTrackControls.h"
#include "widgets/PopupMenuTable.h"
#include <wx/cursor.h>
#include <wx/dc.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <wx/timer.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <optional>
#include <set>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {
using namespace TrackGroups;
enum class HeaderAction { Select, Toggle, Ungroup, Menu };
void ShowGroupMenu(AudacityProject &, AuTopsyTrackGroup &, wxWindow &);
std::vector<Track*> SelectedMembers(AudacityProject &project)
{
   std::vector<Track*> result;
   for (auto *track : TrackList::Get(project).Selected<Track>())
      if (!dynamic_cast<AuTopsyTrackGroup*>(track)) result.push_back(track);
   return result;
}
AuTopsyTrackGroup *SelectedGroup(AudacityProject &project)
{
   std::string id;
   if (auto *focused = TrackFocus::Get(project).Get()) {
      if (auto *header = dynamic_cast<AuTopsyTrackGroup*>(focused)) return header;
      if (auto *header = FindTrackGroup(project, MemberGroup(*focused))) return header;
   }
   for (auto *track : TrackList::Get(project).Selected<Track>()) {
      const auto *header = dynamic_cast<AuTopsyTrackGroup*>(track);
      const auto candidate = header ? header->GroupId() : MemberGroup(*track);
      if (candidate.empty()) continue;
      if (!id.empty() && id != candidate) return nullptr;
      id = candidate;
   }
   return FindTrackGroup(project, id);
}
void CreateSelectedGroup(AudacityProject &project)
{
   wxTextEntryDialog dialog(&ProjectWindow::Get(project), _("Name the group of selected tracks:"), _("Group Tracks"), _("Track Group"));
   if (dialog.ShowModal() != wxID_OK) return;
   GroupProjectTracks(project, SelectedMembers(project), dialog.GetValue());
}
std::vector<Track*> TrackOrder(AudacityProject &project)
{
   std::vector<Track*> result;
   for (auto *track : TrackList::Get(project).Any<Track>()) result.push_back(track);
   return result;
}
class GroupHeaderHandle final : public UIHandle {
   std::weak_ptr<AuTopsyTrackGroup> mTrack;
   HeaderAction mAction;
   wxRect mClickRect;
   int mOriginY{};
   std::vector<Track*> mOriginal;
   bool mMoved{};
public:
   GroupHeaderHandle(const std::shared_ptr<AuTopsyTrackGroup> &track, HeaderAction action, const wxRect &rect)
      : mTrack(track), mAction(action), mClickRect(rect) {}
   std::shared_ptr<const Track> FindTrack() const override { return mTrack.lock(); }
   bool HandlesRightClick() override { return true; }
   bool StopsOnKeystroke() override { return true; }
   HitTestPreview Preview(const TrackPanelMouseState &, AudacityProject *) override {
      static wxCursor cursor(wxCURSOR_HAND);
      return {XO("Select or drag this track group; use its arrow to collapse or expand"), &cursor};
   }
   Result Click(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      auto track = mTrack.lock();
      if (!project || !track || ProjectAudioIO::Get(*project).IsAudioActive()) return RefreshCode::Cancelled;
      if (event.event.RightDown()) mAction = HeaderAction::Menu;
      mOriginY = event.event.GetY(); mOriginal = TrackOrder(*project);
      if (mAction == HeaderAction::Select) SelectProjectTrackGroup(*project, track->GroupId(), event.event.ControlDown());
      return RefreshCode::RefreshAll;
   }
   Result Drag(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      if (!project || mAction != HeaderAction::Select || std::abs(event.event.GetY() - mOriginY) < 5) return 0;
      auto track = mTrack.lock();
      if (!track) return 0;
      const auto members = GroupMembers(*project, track->GroupId());
      Track *before{};
      for (auto *candidate : TrackList::Get(*project).Any<Track>()) {
         if (candidate == track.get() || std::find(members.begin(), members.end(), candidate) != members.end() || IsTrackHidden(*candidate)) continue;
         const auto rect = TrackPanel::Get(*project).FindTrackRect(candidate);
         if (!rect.IsEmpty() && event.event.GetY() < rect.y + rect.height / 2) { before = candidate; break; }
      }
      mMoved |= MoveProjectTrackGroup(*project, track->GroupId(), before ? TrackIdentity(*before) : std::string{}, false);
      return RefreshCode::RefreshAll;
   }
   Result Release(const TrackPanelMouseEvent &event, AudacityProject *project, wxWindow *parent) override {
      auto track = mTrack.lock();
      if (!project || !track) return 0;
      try {
         if (mMoved) {
            if (TrackOrder(*project) != mOriginal) ProjectHistory::Get(*project).PushState(XO("Moved track group"), XO("Move Track Group"));
         }
         else if (mClickRect.Contains(event.event.GetPosition())) {
            if (mAction == HeaderAction::Toggle) CollapseProjectTrackGroup(*project, track->GroupId(), !track->Collapsed());
            else if (mAction == HeaderAction::Ungroup) UngroupProjectTracks(*project, track->GroupId());
            else if (mAction == HeaderAction::Menu && parent) ShowGroupMenu(*project, *track, *parent);
         }
      }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Track Groups"), wxOK | wxICON_ERROR, parent);
      }
      return RefreshCode::RefreshAll;
   }
   Result Cancel(AudacityProject *project) override {
      if (project && mMoved) {
         const auto current = TrackOrder(*project);
         const std::set<Track*> expected(mOriginal.begin(), mOriginal.end()), actual(current.begin(), current.end());
         if (actual == expected) TrackList::Get(*project).Permute(mOriginal);
         RefreshTrackGroupProjection(*project);
      }
      return RefreshCode::RefreshAll;
   }
};
class GroupRuler final : public ChannelVRulerControls {
public:
   using ChannelVRulerControls::ChannelVRulerControls;
   void UpdateRuler(const wxRect &) override {}
};
struct SummaryKey {
   uint64_t revision{};
   double from{}, to{};
   int columns{};
   bool operator==(const SummaryKey &other) const {
      return revision == other.revision && from == other.from && to == other.to && columns == other.columns;
   }
   bool operator!=(const SummaryKey &other) const { return !(*this == other); }
};
struct SummaryReply {
   std::optional<std::vector<WaveformColumn>> columns;
   std::string error;
};
class GroupView final : public CommonChannelView, public wxEvtHandler {
   std::weak_ptr<GroupHeaderHandle> mHandle;
   std::optional<SummaryKey> mWanted, mRunning, mCompleted;
   std::future<SummaryReply> mJob;
   std::shared_ptr<std::atomic<bool>> mCancel;
   std::vector<WaveformColumn> mSummary;
   std::string mError;
   wxTimer mTimer{this};
   wxRect mBounds;
   void Poll() {
      if (!mJob.valid() || mJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
      auto reply = mJob.get();
      if (mWanted == mRunning && reply.columns) { mSummary = std::move(*reply.columns); mCompleted = mRunning; mError.clear(); }
      else if (mWanted == mRunning && !reply.error.empty()) { mError = std::move(reply.error); mCompleted = mRunning; }
      mRunning.reset(); mCancel.reset(); mTimer.Stop();
      if (auto track = FindTrack()) if (auto owner = track->GetOwner()) if (auto *project = owner->GetOwner()) Viewport::Get(*project).Redraw();
   }
   void RequestSummary(AudacityProject &project, AuTopsyTrackGroup &header, const wxRect &rect) {
      const auto &view = ViewInfo::Get(project);
      const SummaryKey key{TrackGroupRevision(project), view.PositionToTime(rect.x, rect.x),
         view.PositionToTime(rect.GetRight() + 1, rect.x), std::min(65536, rect.width)};
      mWanted = key;
      if (mCompleted == mWanted) return;
      if (mJob.valid()) { if (mRunning != mWanted) mCancel->store(true); return; }
      Mixer::Inputs inputs;
      double rate = 48000;
      const auto members = GroupMembers(project, header.GroupId());
      const bool solo = std::any_of(members.begin(), members.end(), [](auto *track) {
         const auto *audio = dynamic_cast<WaveTrack*>(track); return audio && audio->GetSolo();
      });
      for (auto *track : members) if (auto *audio = dynamic_cast<WaveTrack*>(track)) {
         if (audio->GetMute() || (solo && !audio->GetSolo())) continue;
         rate = std::max(rate, audio->GetRate());
         // Clone containers on the GUI thread. Sample blocks remain shared and
         // immutable; a worker never traverses a live editable track or view.
         auto copy = std::dynamic_pointer_cast<WaveTrack>(audio->Duplicate());
         inputs.emplace_back(std::move(copy));
      }
      mRunning = key; mCancel = std::make_shared<std::atomic<bool>>(false); mError.clear();
      const auto cancel = mCancel;
      mJob = std::async(std::launch::async, [key, inputs = std::move(inputs), rate, cancel]() mutable -> SummaryReply {
         try {
            if (!std::isfinite(key.from) || !std::isfinite(key.to) || key.to < key.from ||
                !std::isfinite(rate) || rate <= 0 || (key.to - key.from) * rate > double(uint64_t{1} << 48))
               throw std::runtime_error("The visible group waveform range is too large.");
            if (inputs.empty()) return {std::vector<WaveformColumn>(key.columns), {}};
            Mixer mixer(std::move(inputs), std::nullopt, true,
               Mixer::WarpOptions(static_cast<const BoundedEnvelope*>(nullptr)), key.from, key.to,
               1, 4096, false, rate, floatSample, true, nullptr, Mixer::ApplyVolume::Mixdown);
            const auto frames = uint64_t(std::max(0.0, std::floor((key.to - key.from) * rate)));
            uint64_t produced{};
            const auto read = [&](float *output, size_t count) {
               const auto got = mixer.Process(count);
               if (got) std::copy_n(reinterpret_cast<const float*>(mixer.GetBuffer(0)), got, output);
               produced += got;
               // Native absolute-time bounds and this view's sample grid can
               // differ by one endpoint sample. Only that finite tail is silence;
               // an earlier unexpected end remains an error, never a partial mix.
               if (!got && frames - produced <= 1 && mixer.MixGetCurrentTime() >= key.to - 2 / rate) {
                  std::fill_n(output, count, 0.0f); produced += count; return count;
               }
               return got;
            };
            return {RenderSummedWaveform(read, frames, key.columns, [cancel] { return cancel->load(); }), {}};
         }
         catch (const std::exception &error) { return {{}, error.what()}; }
      });
      mTimer.Start(30);
   }
public:
   explicit GroupView(const std::shared_ptr<Channel> &channel) : CommonChannelView(channel) {
      Bind(wxEVT_TIMER, [this](wxTimerEvent &) { Poll(); }, mTimer.GetId());
   }
   ~GroupView() override {
      mTimer.Stop();
      if (mCancel) mCancel->store(true);
      // Join before project storage closes. At most one bounded mixer block is
      // in progress; no detached worker can retain a closed project's database.
      if (mJob.valid()) mJob.wait();
   }
   bool SummaryReady() const { return mCompleted == mWanted && mCompleted && mError.empty(); }
   const auto &Summary() const { return mSummary; }
   const auto &SummaryError() const { return mError; }
   const wxRect &Bounds() const { return mBounds; }
   std::shared_ptr<ChannelVRulerControls> DoGetVRulerControls() override { return std::make_shared<GroupRuler>(shared_from_this()); }
   int GetMinimizedHeight() const override { return 48; }
   std::vector<UIHandlePtr> DetailedHitTest(const TrackPanelMouseState &state, const AudacityProject *, int, bool) override {
      auto header = std::dynamic_pointer_cast<AuTopsyTrackGroup>(FindTrack());
      if (!header) return {};
      const auto action = state.state.RightIsDown() ? HeaderAction::Menu : HeaderAction::Select;
      return {AssignUIHandlePtr(mHandle, std::make_shared<GroupHeaderHandle>(header, action, state.rect))};
   }
   void Draw(TrackPanelDrawingContext &context, const wxRect &rect, unsigned pass) override {
      if (pass != TrackArtist::PassTracks) { CommonChannelView::Draw(context, rect, pass); return; }
      mBounds = rect;
      auto header = std::dynamic_pointer_cast<AuTopsyTrackGroup>(FindTrack());
      if (!header || !header->GetOwner()) return;
      auto *project = header->GetOwner()->GetOwner();
      if (!project) return;
      auto &dc = context.dc;
      dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(wxColour(40, 62, 70))); dc.DrawRectangle(rect);
      dc.SetTextForeground(wxColour(225, 239, 235));
      if (!header->Collapsed()) {
         if (mCancel) mCancel->store(true);
         dc.DrawText(_("Track group — member tracks below"), rect.x + 8, rect.y + 8); return;
      }
      RequestSummary(*project, *header, rect);
      const int middle = rect.y + rect.height / 2;
      dc.SetPen(wxPen(wxColour(87, 111, 116))); dc.DrawLine(rect.x, middle, rect.GetRight(), middle);
      if (SummaryReady()) {
         dc.SetPen(wxPen(wxColour(113, 219, 179)));
         const auto scale = std::max(1, rect.height / 2 - 7);
         for (size_t i{}; i < mSummary.size(); ++i) {
            const int x = rect.x + int(i * rect.width / mSummary.size());
            dc.DrawLine(x, middle - int(std::clamp(mSummary[i].maximum, -1.0f, 1.0f) * scale),
               x, middle - int(std::clamp(mSummary[i].minimum, -1.0f, 1.0f) * scale));
         }
      }
      else dc.DrawText(mError.empty() ? _("Rendering group mix...") : _("Group waveform unavailable"), rect.x + 8, rect.y + 8);
   }
};
class GroupControls final : public TrackControls {
   std::weak_ptr<GroupHeaderHandle> mHandle;
   wxRect mBounds;
public:
   using TrackControls::TrackControls;
   const TrackInfo::TCPLines &GetTCPLines() const override { static const TrackInfo::TCPLines empty; return empty; }
   wxRect ToggleRect() const { return {mBounds.x + 4, mBounds.y + 4, 24, 25}; }
   wxRect CloseRect() const { return {mBounds.GetRight() - 24, mBounds.y + 4, 21, 25}; }
   std::vector<UIHandlePtr> HitTest(const TrackPanelMouseState &state, const AudacityProject *) override {
      auto header = std::dynamic_pointer_cast<AuTopsyTrackGroup>(FindTrack());
      if (!header) return {};
      auto action = HeaderAction::Select;
      if (state.state.RightIsDown()) action = HeaderAction::Menu;
      else if (ToggleRect().Contains(state.state.GetPosition())) action = HeaderAction::Toggle;
      else if (CloseRect().Contains(state.state.GetPosition())) action = HeaderAction::Ungroup;
      const auto rect = action == HeaderAction::Toggle ? ToggleRect() : action == HeaderAction::Ungroup ? CloseRect() : state.rect;
      return {AssignUIHandlePtr(mHandle, std::make_shared<GroupHeaderHandle>(header, action, rect))};
   }
   void Draw(TrackPanelDrawingContext &context, const wxRect &rect, unsigned pass) override {
      if (pass != TrackArtist::PassControls) return;
      mBounds = rect;
      auto header = std::dynamic_pointer_cast<AuTopsyTrackGroup>(FindTrack());
      if (!header) return;
      auto &dc = context.dc;
      dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(wxColour(40, 62, 70))); dc.DrawRectangle(rect);
      dc.SetTextForeground(wxColour(225, 239, 235));
      dc.DrawText(header->Collapsed() ? wxString{">"} : wxString{"v"}, rect.x + 9, rect.y + 8);
      dc.DrawText("x", CloseRect().x + 6, rect.y + 8);
      dc.SetClippingRegion(rect.x + 32, rect.y, std::max(1, rect.width - 59), rect.height);
      dc.DrawText(header->GetName(), rect.x + 33, rect.y + 8); dc.DestroyClippingRegion();
      if (header->Collapsed()) dc.DrawText(_("Summed audio"), rect.x + 8, rect.y + 38);
   }
};
void MoveGroupRelative(AudacityProject &project, AuTopsyTrackGroup &header, bool up)
{
   std::vector<Track*> visible;
   const auto members = GroupMembers(project, header.GroupId());
   for (auto *track : TrackList::Get(project).Any<Track>())
      if (std::find(members.begin(), members.end(), track) == members.end() && !IsTrackHidden(*track) &&
          (MemberGroup(*track).empty() || !FindTrackGroup(project, MemberGroup(*track)))) visible.push_back(track);
   auto position = std::find(visible.begin(), visible.end(), &header);
   if (up) {
      if (position != visible.begin()) MoveProjectTrackGroup(project, header.GroupId(), TrackIdentity(**std::prev(position)));
   }
   else if (position != visible.end() && ++position != visible.end()) {
      const auto next = ++position;
      MoveProjectTrackGroup(project, header.GroupId(), next == visible.end() ? std::string{} : TrackIdentity(**next));
   }
}
void ShowGroupMenu(AudacityProject &project, AuTopsyTrackGroup &header, wxWindow &parent)
{
   wxMenu menu;
   menu.Append(1, header.Collapsed() ? _("Expand group") : _("Collapse group"));
   menu.Append(2, _("Select member tracks")); menu.Append(3, _("Rename group..."));
   menu.AppendSeparator(); menu.Append(4, _("Move group up")); menu.Append(5, _("Move group down"));
   menu.AppendSeparator(); menu.Append(6, _("Ungroup tracks (keep audio)"));
   menu.Bind(wxEVT_MENU, [&](wxCommandEvent &event) {
      switch (event.GetId()) {
      case 1: CollapseProjectTrackGroup(project, header.GroupId(), !header.Collapsed()); break;
      case 2: SelectProjectTrackGroup(project, header.GroupId()); break;
      case 3: {
         wxTextEntryDialog dialog(&parent, _("Name the track group:"), _("Rename Group"), header.GetName());
         if (dialog.ShowModal() == wxID_OK && dialog.GetValue() != header.GetName()) {
            header.SetName(dialog.GetValue()); ProjectHistory::Get(project).PushState(XO("Renamed track group"), XO("Rename Group"));
         }
         break;
      }
      case 4: MoveGroupRelative(project, header, true); break;
      case 5: MoveGroupRelative(project, header, false); break;
      case 6: UngroupProjectTracks(project, header.GroupId()); break;
      }
   });
   parent.PopupMenu(&menu);
}
class GroupTracksCommand final : public AudacityCommand {
   wxString mName{wxT("Track Group")};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Group selected tracks without mixing their audio."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) { visitor.Define(mName, wxT("Name"), wxT("Track Group")); return true; }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool Apply(const CommandContext &context) override {
      try { GroupProjectTracks(context.project, SelectedMembers(context.project), mName); return true; }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol GroupTracksCommand::Symbol{XO("GroupTracks")};
BuiltinCommandsModule::Registration<GroupTracksCommand> sGroupTracks;
class SetTrackGroupCommand final : public AudacityCommand {
   wxString mId;
   bool mCollapsed{}, mRemove{};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Set track group visibility or ungroup its tracks."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mId, wxT("Id"), wxT(""));
      visitor.Define(mCollapsed, wxT("Collapsed"), false); visitor.Define(mRemove, wxT("Ungroup"), false); return true;
   }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool Apply(const CommandContext &context) override {
      try {
         auto *selected = SelectedGroup(context.project);
         const auto bytes = mId.ToUTF8(); const std::string id = mId.empty() && selected ? selected->GroupId() : std::string(bytes.data(), bytes.length());
         if (mRemove) UngroupProjectTracks(context.project, id);
         else CollapseProjectTrackGroup(context.project, id, mCollapsed);
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetTrackGroupCommand::Symbol{XO("SetTrackGroup")};
BuiltinCommandsModule::Registration<SetTrackGroupCommand> sSetTrackGroup;
class GetTrackGroupsCommand final : public AudacityCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Get track folder layout and summed waveform readiness."); }
   bool Apply(const CommandContext &context) override {
      RefreshTrackGroupProjection(context.project);
      rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
      const auto tracks = TrackOrder(context.project);
      auto &panel = TrackPanel::Get(context.project);
      const auto rect = [&](const char *name, const wxRect &value) {
         out.Key(name); out.StartObject(); out.Key("x"); out.Int(value.x); out.Key("y"); out.Int(value.y);
         out.Key("width"); out.Int(value.width); out.Key("height"); out.Int(value.height); out.EndObject();
      };
      out.StartObject(); out.Key("panelHandle"); out.Uint64(reinterpret_cast<uintptr_t>(panel.GetHandle()));
      out.Key("tracks"); out.StartArray();
      for (size_t index{}; index < tracks.size(); ++index) {
         auto *track = tracks[index];
         out.StartObject(); out.Key("index"); out.Uint64(index); out.Key("id"); out.String(TrackIdentity(*track).c_str());
         out.Key("group"); out.String(MemberGroup(*track).c_str()); out.Key("hidden"); out.Bool(IsTrackHidden(*track));
         out.Key("height"); out.Int(ChannelView::GetChannelGroupHeight(track)); rect("bounds", panel.FindTrackRect(track)); out.EndObject();
      }
      out.EndArray(); out.Key("groups"); out.StartArray();
      for (size_t index{}; index < tracks.size(); ++index) if (auto *header = dynamic_cast<AuTopsyTrackGroup*>(tracks[index])) {
         auto &view = static_cast<GroupView&>(ChannelView::Get(*header->GetChannel(0)));
         auto &controls = static_cast<GroupControls&>(TrackControls::Get(*header));
         out.StartObject(); out.Key("id"); out.String(header->GroupId().c_str()); out.Key("index"); out.Uint64(index);
         const auto name = header->GetName().ToUTF8(); out.Key("name"); out.String(name.data(), name.length());
         out.Key("collapsed"); out.Bool(header->Collapsed()); out.Key("supported"); out.Bool(header->Supported());
         rect("toggle", controls.ToggleRect()); rect("ungroup", controls.CloseRect());
         out.Key("members"); out.StartArray();
         for (size_t member{}; member < tracks.size(); ++member) if (MemberGroup(*tracks[member]) == header->GroupId()) out.Uint64(member);
         out.EndArray(); out.Key("summaryReady"); out.Bool(view.SummaryReady());
         float minimum{}, maximum{};
         for (const auto &column : view.Summary()) { minimum = std::min(minimum, column.minimum); maximum = std::max(maximum, column.maximum); }
         out.Key("minimum"); out.Double(minimum); out.Key("maximum"); out.Double(maximum);
         out.Key("summaryColumns"); out.Uint64(view.Summary().size()); out.Key("summaryError"); out.String(view.SummaryError().c_str()); out.EndObject();
      }
      out.EndArray(); out.EndObject(); context.Status(wxString::FromUTF8(buffer.GetString())); return true;
   }
};
const ComponentInterfaceSymbol GetTrackGroupsCommand::Symbol{XO("GetTrackGroups")};
BuiltinCommandsModule::Registration<GetTrackGroupsCommand> sGetTrackGroups;
class GroupPopupHandler final : public PopupMenuHandler {
public:
   CommonTrackControls::InitMenuData *data{};
   void InitUserData(void *value) override { data = static_cast<CommonTrackControls::InitMenuData*>(value); }
   void OnGroup(wxCommandEvent &) { if (data) CreateSelectedGroup(data->project); }
   void OnUngroup(wxCommandEvent &) {
      if (data) if (auto *header = FindTrackGroup(data->project, MemberGroup(data->track)))
         UngroupProjectTracks(data->project, header->GroupId());
   }
};
GroupPopupHandler sPopupHandler;
PopupMenuTable::AttachedItem sGroupContext{GetCommonTrackMenu(), Registry::Placement{},
   std::make_shared<PopupMenuTableEntry>("GroupSelectedTracks", PopupMenuTableEntry::Item,
      wxWindow::NewControlId(), XXO("Group Selected Tracks..."),
      static_cast<wxCommandEventFunction>(&GroupPopupHandler::OnGroup), sPopupHandler,
      [](PopupMenuHandler &handler, wxMenu &menu, int id) {
         const auto *data = static_cast<GroupPopupHandler&>(handler).data;
         menu.Enable(id, data && !ProjectAudioIO::Get(data->project).IsAudioActive() && SelectedMembers(data->project).size() >= 2);
      })};
PopupMenuTable::AttachedItem sUngroupContext{GetCommonTrackMenu(), Registry::Placement{},
   std::make_shared<PopupMenuTableEntry>("UngroupTracks", PopupMenuTableEntry::Item,
      wxWindow::NewControlId(), XXO("Ungroup Tracks"),
      static_cast<wxCommandEventFunction>(&GroupPopupHandler::OnUngroup), sPopupHandler,
      [](PopupMenuHandler &handler, wxMenu &menu, int id) {
         const auto *data = static_cast<GroupPopupHandler&>(handler).data;
         menu.Enable(id, data && !ProjectAudioIO::Get(data->project).IsAudioActive() &&
            FindTrackGroup(data->project, MemberGroup(data->track)));
      })};
using namespace MenuRegistry;
AttachedItem sGroupsMenu{Section("TrackFolders", Menu("TrackGroups", XXO("Track &Groups"),
   Command("GroupSelectedTracks", XXO("&Group Selected Tracks..."), [](const CommandContext &context) { CreateSelectedGroup(context.project); },
      AudioIONotBusyFlag() | AnyTracksSelectedFlag()),
   Command("CollapseTrackGroup", XXO("&Collapse Group"), [](const CommandContext &context) { if (auto *group = SelectedGroup(context.project)) CollapseProjectTrackGroup(context.project, group->GroupId(), true); }, AudioIONotBusyFlag()),
   Command("ExpandTrackGroup", XXO("&Expand Group"), [](const CommandContext &context) { if (auto *group = SelectedGroup(context.project)) CollapseProjectTrackGroup(context.project, group->GroupId(), false); }, AudioIONotBusyFlag()),
   Command("UngroupTracks", XXO("&Ungroup Tracks"), [](const CommandContext &context) { if (auto *group = SelectedGroup(context.project)) UngroupProjectTracks(context.project, group->GroupId()); }, AudioIONotBusyFlag())
)), Registry::Placement{wxT("Tracks")}};
}
using GetFolderControls = DoGetControls::Override<AuTopsyTrackGroup>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(GetFolderControls) {
   return [](AuTopsyTrackGroup &track) { return std::make_shared<GroupControls>(track.SharedPointer()); };
}
using GetFolderView = DoGetView::Override<AuTopsyTrackGroup>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(GetFolderView) {
   return [](AuTopsyTrackGroup &track, size_t channel) { return std::make_shared<GroupView>(track.GetChannel(channel)); };
}
using GetFolderHeight = GetDefaultTrackHeight::Override<AuTopsyTrackGroup>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(GetFolderHeight) { return [](AuTopsyTrackGroup &) { return 48; }; }
void TrackGroups::ShowTrackGroupMenu(AudacityProject &project, Track &track)
{
   if (auto *group = dynamic_cast<AuTopsyTrackGroup*>(&track)) ShowGroupMenu(project, *group, TrackPanel::Get(project));
   else CreateSelectedGroup(project);
}
bool TrackGroups::HandleTrackGroupMove(AudacityProject &project, Track &track, GroupMove choice)
{
   auto *header = dynamic_cast<AuTopsyTrackGroup*>(&track);
   if (!header) return false;
   if (choice == GroupMove::Up || choice == GroupMove::Down) MoveGroupRelative(project, *header, choice == GroupMove::Up);
   else if (choice == GroupMove::Bottom) MoveProjectTrackGroup(project, header->GroupId(), {});
   else {
      const auto order = TrackOrder(project);
      if (!order.empty()) MoveProjectTrackGroup(project, header->GroupId(), TrackIdentity(*order.front()));
   }
   return true;
}
