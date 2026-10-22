// All native track/XML/history assumptions live here. Folder metadata never
// contains audio tracks: supported tracks remain ordinary siblings in AUP3.
#include "CreateTrackGroup.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "UndoManager.h"
#include "ProjectAudioIO.h"
#include "TrackAttachment.h"
#include "TrackFocus.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "Uuid.h"
#include "XMLWriter.h"
#include "SelectUtilities.h"
#include "tracks/ui/ChannelView.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {
std::string NewIdentity() { return audacity::Uuid::Generate().ToHexString(); }
struct GroupMemberAttachment;
GroupMemberAttachment &Member(Track &track);
struct GroupMemberAttachment final : TrackAttachment {
   std::weak_ptr<Track> track;
   std::string id{NewIdentity()}, group;
   bool hidden{};
   explicit GroupMemberAttachment(Track &owner) : track(owner.SharedPointer()) {}
   void Reparent(const std::shared_ptr<Track> &owner) override { track = owner; }
   void CopyTo(Track &destination) const override {
      auto &copy = Member(destination);
      copy.id = id; copy.group = group; copy.hidden = false;
   }
   void WriteXMLAttributes(XMLWriter &writer) const override {
      const auto parent = track.lock();
      const auto owner = parent ? parent->GetOwner() : nullptr;
      auto *project = owner ? owner->GetOwner() : nullptr;
      if (group.empty() || !project || !TrackGroups::FindTrackGroup(*project, group)) return;
      writer.WriteAttr("autopsyTrackIdentity", wxString::FromUTF8(id));
      writer.WriteAttr("autopsyTrackGroup", wxString::FromUTF8(group));
   }
   bool HandleXMLAttribute(const std::string_view &name, const XMLAttributeValueView &value) override {
      if (name != "autopsyTrackIdentity" && name != "autopsyTrackGroup") return false;
      if (!value.IsStringView()) return false;
      const auto bytes = wxString(value.ToWString()).ToUTF8();
      const std::string text{bytes.data(), bytes.length()};
      if (!TrackGroups::IsGroupIdentity(text)) return false;
      (name == "autopsyTrackIdentity" ? id : group) = text;
      if (auto parent = track.lock()) if (auto owner = parent->GetOwner())
         if (auto *project = owner->GetOwner()) TrackGroups::RefreshTrackGroupProjection(*project);
      return true;
   }
};
const AttachedTrackObjects::RegisteredFactory sMemberKey{
   [](Track &track) { return std::make_shared<GroupMemberAttachment>(track); }
};
GroupMemberAttachment &Member(Track &track)
{ return track.AttachedTrackObjects::Get<GroupMemberAttachment>(sMemberKey); }
std::vector<TrackGroups::GroupRow> ReadRows(AudacityProject &project)
{
   std::vector<TrackGroups::GroupRow> rows;
   for (auto *track : TrackList::Get(project).Any<Track>()) {
      if (auto *header = dynamic_cast<AuTopsyTrackGroup*>(track); header && header->Supported())
         rows.push_back({header->GroupId(), {}, true, header->Collapsed()});
      else rows.push_back({Member(*track).id, Member(*track).group, false, false});
   }
   return rows;
}
void SetTrackOrder(AudacityProject &project, const std::vector<std::string> &order)
{
   auto &tracks = TrackList::Get(project);
   std::unordered_map<std::string, Track*> lookup;
   std::vector<std::string> existing;
   for (auto *track : tracks.Any<Track>()) {
      const auto id = TrackGroups::TrackIdentity(*track);
      if (!lookup.emplace(id, track).second) throw std::runtime_error("Track identities are ambiguous.");
      existing.push_back(id);
   }
   if (existing == order) return;
   if (order.size() != lookup.size()) throw std::runtime_error("Track group layout changed before application.");
   std::vector<Track*> changed;
   std::unordered_set<std::string> seen;
   for (const auto &id : order) {
      const auto found = lookup.find(id);
      if (found == lookup.end() || !seen.insert(id).second) throw std::runtime_error("A group track is missing or duplicated.");
      changed.push_back(found->second);
   }
   tracks.Permute(changed);
}
class ProjectGroupPresentation final : public ClientData::Base {
public:
   AudacityProject &project;
   Observer::Subscription changes;
   Observer::Subscription history;
   uint64_t revision{};
   bool refreshing{};
   explicit ProjectGroupPresentation(AudacityProject &owner) : project(owner) {
      history = UndoManager::Get(project).Subscribe([this](const UndoRedoMessage &) { ++revision; Refresh(); });
      changes = TrackList::Get(project).Subscribe([this](const TrackListEvent &event) {
         if (event.mType == TrackListEvent::SELECTION_CHANGE || event.mType == TrackListEvent::RESIZING) return;
         ++revision;
         if (event.mType == TrackListEvent::ADDITION) if (auto track = event.mpTrack.lock()) {
            auto &member = Member(*track);
            // Fresh duplicates can copy a member identity, while Undo restores
            // the original identity. Distinguish them from the current owned
            // identities themselves, never a host-specific session-ID lifecycle.
            for (auto *other : TrackList::Get(project).Any<Track>())
               if (other != track.get() && Member(*other).id == member.id) {
                  member.id = NewIdentity(); break;
               }
         }
         Refresh();
      });
   }
   void Refresh() {
      if (refreshing) return;
      refreshing = true;
      struct Reset { bool &flag; ~Reset() { flag = false; } } reset{refreshing};
      auto &tracks = TrackList::Get(project);
      const auto rows = ReadRows(project);
      bool coherent = true;
      try { TrackGroups::CheckGroupRows(rows); }
      catch (const std::exception &) { coherent = false; }
      bool changed{};
      for (auto *track : tracks.Any<Track>()) {
         auto &member = Member(*track);
         const auto *header = dynamic_cast<AuTopsyTrackGroup*>(track);
         const bool hidden = header ? !header->Supported() : coherent &&
            TrackGroups::IsCollapsedGroupMember(rows, member.id);
         changed |= member.hidden != hidden; member.hidden = hidden;
         if (header && header->Supported()) {
            auto &view = ChannelView::Get(*track->GetChannel(0));
            const int height = header->Collapsed() ? 112 : 48;
            if (view.GetExpandedHeight() != height) view.SetExpandedHeight(height);
         }
      }
      // Failed/unknown optional metadata must leave the audio accessible.
      // Ordinary Undo can temporarily expose an incomplete list; do not reorder
      // until restoration has finished adding its saved tracks.
      if (coherent && !ProjectHistory::Get(project).IsRestoringState())
         SetTrackOrder(project, TrackGroups::ArrangeGroupRows(rows));
      if (changed && !tracks.empty()) (*tracks.begin())->AdjustPositions();
   }
};
const AudacityProject::AttachedObjects::RegisteredFactory sPresentationKey{
   [](AudacityProject &project) { return std::make_shared<ProjectGroupPresentation>(project); }
};
ProjectGroupPresentation &Presentation(AudacityProject &project)
{ return project.AttachedObjects::Get<ProjectGroupPresentation>(sPresentationKey); }
const ProjectFileIORegistry::ObjectReaderEntry sReadGroup{
   "autopsytrackgroup", [](AudacityProject &project) -> XMLTagHandler* {
      Presentation(project);
      auto track = std::make_shared<AuTopsyTrackGroup>();
      auto *result = TrackList::Get(project).Add(track);
      result->BuildTrackAdapters(); return result;
   }
};
void RequireStopped(AudacityProject &project)
{
   if (ProjectAudioIO::Get(project).IsAudioActive()) throw std::runtime_error("Stop playback before editing track groups.");
}
void RefreshPane(AudacityProject &project)
{
   TrackGroups::RefreshTrackGroupProjection(project);
   Viewport::Get(project).HandleResize(); Viewport::Get(project).Redraw();
}
}

AuTopsyTrackGroup::AuTopsyTrackGroup() : mGroupId(NewIdentity()) { SetName(_("Track Group")); }
AuTopsyTrackGroup::AuTopsyTrackGroup(const AuTopsyTrackGroup &other, ProtectedCreationArg &&arg)
   : UniqueChannelTrack<>(other, std::move(arg)), mGroupId(other.mGroupId),
     mCollapsed(other.mCollapsed), mSupported(other.mSupported) {}
AuTopsyTrackGroup::~AuTopsyTrackGroup() = default;
const Track::TypeInfo &AuTopsyTrackGroup::ClassTypeInfo()
{
   static const TypeInfo info{{"trackgroup", "trackgroup", XO("Track Group")}, true, &Track::ClassTypeInfo()};
   return info;
}
const Track::TypeInfo &AuTopsyTrackGroup::GetTypeInfo() const { return ClassTypeInfo(); }
Track::Holder AuTopsyTrackGroup::Clone(bool) const
{
   auto result = std::make_shared<AuTopsyTrackGroup>(*this, ProtectedCreationArg{});
   result->Init(*this); return result;
}
Track::Holder AuTopsyTrackGroup::Copy(double, double, bool) const { return Clone(false); }
Track::Holder AuTopsyTrackGroup::Cut(double from, double to) { return Copy(from, to); }
Track::Holder AuTopsyTrackGroup::PasteInto(AudacityProject &, TrackList &) const
{ throw std::runtime_error("Copy the member tracks instead of a track group header."); }
bool AuTopsyTrackGroup::HandleXMLTag(const std::string_view &tag, const AttributesList &attributes)
{
   if (tag != "autopsytrackgroup") return false;
   int schema{}, collapsed{};
   std::string id;
   bool fields = true;
   for (const auto &attribute : attributes) {
      if (attribute.first == "trackGroupSchema") fields &= attribute.second.TryGet(schema);
      else if (attribute.first == "collapsed") fields &= attribute.second.TryGet(collapsed);
      else if (attribute.first == "folderId") {
         if (attribute.second.IsStringView()) {
            const auto bytes = wxString(attribute.second.ToWString()).ToUTF8(); id.assign(bytes.data(), bytes.length());
         }
         else fields = false;
      }
      else HandleCommonXMLAttribute(attribute.first, attribute.second);
   }
   mSupported = fields && schema == 1 && TrackGroups::IsGroupIdentity(id) && (collapsed == 0 || collapsed == 1);
   if (mSupported) { mGroupId = std::move(id); mCollapsed = collapsed != 0; }
   if (auto owner = GetOwner()) if (auto *project = owner->GetOwner()) TrackGroups::RefreshTrackGroupProjection(*project);
   return true;
}
void AuTopsyTrackGroup::WriteXML(XMLWriter &writer) const
{
   if (!mSupported) return;
   writer.StartTag("autopsytrackgroup"); WriteCommonXMLAttributes(writer);
   writer.WriteAttr("trackGroupSchema", 1);
   writer.WriteAttr("folderId", wxString::FromUTF8(mGroupId)); writer.WriteAttr("collapsed", int(mCollapsed));
   writer.EndTag("autopsytrackgroup");
}

namespace TrackGroups {
std::string TrackIdentity(const Track &track)
{
   if (const auto *header = dynamic_cast<const AuTopsyTrackGroup*>(&track); header && header->Supported()) return header->GroupId();
   return Member(const_cast<Track&>(track)).id;
}
const std::string &MemberGroup(const Track &track) { return Member(const_cast<Track&>(track)).group; }
bool IsTrackHidden(const Track &track) { return Member(const_cast<Track&>(track)).hidden; }
void RefreshTrackGroupProjection(AudacityProject &project) { Presentation(project).Refresh(); }
uint64_t TrackGroupRevision(AudacityProject &project) { return Presentation(project).revision; }
AuTopsyTrackGroup *FindTrackGroup(AudacityProject &project, const std::string &id)
{
   AuTopsyTrackGroup *result{};
   for (auto *track : TrackList::Get(project).Any<Track>()) {
      auto *header = dynamic_cast<AuTopsyTrackGroup*>(track);
      if (header && header->Supported() && header->GroupId() == id) {
         if (result) return nullptr; // Ambiguity cannot hide supported audio.
         result = header;
      }
   }
   return result;
}
std::vector<Track*> GroupMembers(AudacityProject &project, const std::string &group)
{
   std::vector<Track*> result;
   if (FindTrackGroup(project, group)) for (auto *track : TrackList::Get(project).Any<Track>())
      if (MemberGroup(*track) == group) result.push_back(track);
   return result;
}
void SelectProjectTrackGroup(AudacityProject &project, const std::string &id, bool additive)
{
   auto *header = FindTrackGroup(project, id);
   if (!header) return;
   if (!additive) SelectUtilities::SelectNone(project);
   header->SetSelected(true);
   for (auto *track : GroupMembers(project, id)) track->SetSelected(true);
   TrackFocus::Get(project).Set(header, false); Viewport::Get(project).Redraw();
}
std::shared_ptr<AuTopsyTrackGroup> GroupProjectTracks(AudacityProject &project,
   const std::vector<Track*> &members, const wxString &name)
{
   RequireStopped(project); Presentation(project);
   auto &tracks = TrackList::Get(project);
   for (auto *track : members)
      if (!track || track->GetOwner().get() != &tracks) throw std::runtime_error("A selected track is no longer present.");
   if (members.size() >= 2) {
      const auto existing = MemberGroup(*members.front());
      auto *same = existing.empty() ? nullptr : FindTrackGroup(project, existing);
      if (same && std::all_of(members.begin(), members.end(), [&](auto *track) {
         return track && track->GetOwner().get() == &tracks && MemberGroup(*track) == existing;
      })) {
         const auto current = GroupMembers(project, existing);
         if (current.size() == members.size() && std::all_of(current.begin(), current.end(), [&](auto *track) {
            return std::find(members.begin(), members.end(), track) != members.end();
         })) return same->SharedPointer<AuTopsyTrackGroup>();
      }
   }
   auto header = std::make_shared<AuTopsyTrackGroup>();
   std::vector<std::string> selection;
   for (auto *track : members) {
      if (!track || track->GetOwner().get() != &tracks) throw std::runtime_error("A selected track is no longer present.");
      selection.push_back(TrackIdentity(*track));
   }
   const auto plan = PlanTrackGroup(ReadRows(project), selection, header->GroupId());
   std::vector<Track*> original;
   for (auto *track : tracks.Any<Track>()) original.push_back(track);
   auto first = std::find_if(original.begin(), original.end(), [&](auto *track) {
      return std::find(plan.members.begin(), plan.members.end(), TrackIdentity(*track)) != plan.members.end();
   });
   header->SetName(name.empty() ? tracks.MakeUniqueTrackName(_("Track Group")) : name);
   tracks.Insert(*first, header, true);
   try {
      header->BuildTrackAdapters();
      for (auto *track : members) Member(*track).group = header->GroupId();
      SetTrackOrder(project, plan.order); RefreshPane(project);
      SelectProjectTrackGroup(project, header->GroupId());
      ProjectHistory::Get(project).PushState(XO("Grouped tracks"), XO("Group Tracks"));
      Viewport::Get(project).ShowTrack(*header);
   }
   catch (...) {
      for (auto *track : members) Member(*track).group.clear();
      tracks.Remove(*header); tracks.Permute(original); RefreshPane(project); throw;
   }
   return header;
}
bool CollapseProjectTrackGroup(AudacityProject &project, const std::string &id, bool collapsed)
{
   RequireStopped(project);
   auto *header = FindTrackGroup(project, id);
   if (!header || header->Collapsed() == collapsed) return false;
   const auto previous = header->Collapsed();
   try {
      header->SetCollapsed(collapsed); RefreshPane(project);
      TrackFocus::Get(project).Set(header, false);
      ProjectHistory::Get(project).PushState(collapsed ? XO("Collapsed track group") : XO("Expanded track group"), XO("Track Group"));
   }
   catch (...) { header->SetCollapsed(previous); RefreshPane(project); throw; }
   return true;
}
bool UngroupProjectTracks(AudacityProject &project, const std::string &id)
{
   RequireStopped(project);
   auto *header = FindTrackGroup(project, id);
   if (!header) return false;
   auto saved = header->SharedPointer<AuTopsyTrackGroup>();
   auto members = GroupMembers(project, id);
   auto &tracks = TrackList::Get(project);
   std::vector<Track*> original;
   for (auto *track : tracks.Any<Track>()) original.push_back(track);
   for (auto *track : members) Member(*track).group.clear();
   tracks.Remove(*header);
   try { RefreshPane(project); ProjectHistory::Get(project).PushState(XO("Ungrouped tracks"), XO("Ungroup Tracks")); }
   catch (...) {
      tracks.Add(saved, false);
      for (auto *track : members) Member(*track).group = id;
      tracks.Permute(original); RefreshPane(project); throw;
   }
   return true;
}
bool MoveProjectTrackGroup(AudacityProject &project, const std::string &id,
   const std::string &before, bool history)
{
   RequireStopped(project);
   const auto rows = ReadRows(project);
   const auto desired = MoveGroupRows(rows, id, before);
   std::vector<std::string> original;
   for (const auto &row : rows) original.push_back(row.id);
   if (original == desired) return false;
   try {
      SetTrackOrder(project, desired); RefreshPane(project);
      if (history) ProjectHistory::Get(project).PushState(XO("Moved track group"), XO("Move Track Group"));
   }
   catch (...) { SetTrackOrder(project, original); RefreshPane(project); throw; }
   return true;
}
}
