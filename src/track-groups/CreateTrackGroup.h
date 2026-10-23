// Native track, identity and optional AUP3 adapters for owned track folders.
#pragma once
#include "Track.h"
#include "EditTrackGroups.h"

class AuTopsyTrackGroup final : public UniqueChannelTrack<> {
   std::string mGroupId;
   bool mCollapsed{}, mSupported{true};
   Track::Holder Clone(bool backup) const override;
   std::shared_ptr<WideChannelGroupInterval> DoGetInterval(size_t) override { return {}; }
public:
   AuTopsyTrackGroup();
   AuTopsyTrackGroup(const AuTopsyTrackGroup &, ProtectedCreationArg &&);
   ~AuTopsyTrackGroup() override;
   void BuildTrackAdapters() { AttachedTrackObjects::BuildAll(); }
   const TypeInfo &GetTypeInfo() const override;
   static const TypeInfo &ClassTypeInfo();
   const std::string &GroupId() const { return mGroupId; }
   bool Collapsed() const { return mCollapsed; }
   bool Supported() const { return mSupported; }
   void SetCollapsed(bool collapsed) { mCollapsed = collapsed; Notify(false); }
   bool SupportsBasicEditing() const override { return false; }
   size_t NIntervals() const override { return 0; }
   Track::Holder Cut(double, double) override;
   Track::Holder Copy(double, double, bool = true) const override;
   void Clear(double, double) override {}
   void Paste(double, const Track &) override {}
   void Silence(double, double, ProgressReporter = {}) override {}
   void InsertSilence(double, double) override {}
   void MoveTo(double) override {}
   void ShiftBy(double, double) override {}
   Track::Holder PasteInto(AudacityProject &, TrackList &) const override;
   bool HandleXMLTag(const std::string_view &, const AttributesList &) override;
   XMLTagHandler *HandleXMLChild(const std::string_view &) override { return nullptr; }
   void WriteXML(XMLWriter &) const override;
};
ENUMERATE_TRACK_TYPE(AuTopsyTrackGroup);

namespace TrackGroups {
std::string TrackIdentity(const Track &track);
const std::string &MemberGroup(const Track &track);
bool IsTrackHidden(const Track &track);
void RefreshTrackGroupProjection(AudacityProject &project);
uint64_t TrackGroupRevision(AudacityProject &project);
std::vector<Track*> GroupMembers(AudacityProject &project, const std::string &group);
AuTopsyTrackGroup *FindTrackGroup(AudacityProject &project, const std::string &id);
std::shared_ptr<AuTopsyTrackGroup> GroupProjectTracks(AudacityProject &project,
   const std::vector<Track*> &members, const wxString &name);
bool CollapseProjectTrackGroup(AudacityProject &project, const std::string &id, bool collapsed);
bool UngroupProjectTracks(AudacityProject &project, const std::string &id);
bool MoveProjectTrackGroup(AudacityProject &project, const std::string &id,
   const std::string &before, bool history = true);
void SelectProjectTrackGroup(AudacityProject &project, const std::string &id, bool additive = false);
}
