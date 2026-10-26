#include "GroupAudioClips.h"
#include "EditClipPlacement.h"
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
#include "LinkMediaClips.h"
#endif
#include "WaveClip.h"
#include "WaveTrack.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectWindow.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "XMLWriter.h"
#include "CommandContext.h"
#include "MenuRegistry.h"
#include "CommonCommandFlags.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include "SettingsVisitor.h"
#include "tracks/ui/TimeShiftHandle.h"
#include <wx/choicdlg.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <unordered_set>
namespace {
// The same attachment copy is used by Undo, split clips and clipboard copies.
// Group IDs have no process-local identity and survive ordinary project XML.
struct ClipGroupAttachment final : WaveClipListener {
   std::string groupId;
   bool timingLocked{};
   std::unique_ptr<WaveClipListener> Clone() const override
   { return std::make_unique<ClipGroupAttachment>(*this); }
   void MarkChanged() noexcept override {}
   void Invalidate() override {}
   void WriteXMLAttributes(XMLWriter &writer) const override
   {
      if (!groupId.empty())
         writer.WriteAttr(wxT("autopsyGroup"), wxString::FromUTF8(groupId));
      if (timingLocked) writer.WriteAttr("autopsyTimingLocked", 1);
   }
   bool HandleXMLAttribute(const std::string_view &name,
      const XMLAttributeValueView &value) override
   {
      if (name == "autopsyTimingLocked") {
         int locked{};
         if (!value.TryGet(locked) || locked < 0 || locked > 1) return false;
         timingLocked = locked != 0; return true;
      }
      if (name != "autopsyGroup" || !value.IsStringView()) return false;
      const auto valueText = wxString(value.ToWString());
      const auto text = valueText.ToUTF8();
      const std::string id{ text.data(), text.length() };
      if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
         return false;
      groupId = id;
      return true;
   }
};
const WaveClip::Attachments::RegisteredFactory sClipGroupKey{
   [](WaveClip &) { return std::make_unique<ClipGroupAttachment>(); }
};
ClipGroupAttachment &GroupFor(WaveClip &clip)
{ return clip.Attachments::Get<ClipGroupAttachment>(sClipGroupKey); }
}

namespace ClipGroups {
const std::string &GetClipGroupId(const WaveClip &clip)
{ return GroupFor(const_cast<WaveClip &>(clip)).groupId; }

void SetClipGroupId(WaveClip &clip, std::string groupId)
{ GroupFor(clip).groupId = std::move(groupId); }
bool IsAudioClipTimingLocked(const WaveClip &clip)
{ return GroupFor(const_cast<WaveClip &>(clip)).timingLocked; }
void SetAudioClipTimingLocked(WaveClip &clip, bool locked)
{ GroupFor(clip).timingLocked = locked; }

std::string CreateClipGroupId()
{
   std::random_device random;
   std::ostringstream result;
   for (int i = 0; i < 4; ++i)
      result << std::hex << std::setw(8) << std::setfill('0') << uint32_t(random());
   return result.str();
}

WaveClipPointers ExpandClipGroupMembers(
   TrackList &tracks, const WaveClipPointers &clips)
{
   std::unordered_set<const WaveClip *> chosen{ clips.begin(), clips.end() };
   std::unordered_set<std::string> groups;
   for (const auto *clip : clips)
      if (clip && !GetClipGroupId(*clip).empty()) groups.insert(GetClipGroupId(*clip));
   WaveClipPointers result;
   for (auto *track : tracks.Any<WaveTrack>())
      for (const auto &clip : track->Intervals())
         if (chosen.count(clip.get()) ||
            (!GetClipGroupId(*clip).empty() && groups.count(GetClipGroupId(*clip))))
            result.push_back(clip.get());
   return result;
}

bool ExpandMovingClipGroups(ClipMoveState &state, TrackList &trackList)
{
   std::unordered_set<std::string> groups;
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
   const auto families = BoundVideoTrackMoveFamilies(trackList);
#else
   (void)trackList;
#endif
   const auto groupsOf = [&](const ChannelGroupInterval &interval, const Track *track) -> std::vector<std::string> {
      std::vector<std::string> result;
      if (const auto *audio = dynamic_cast<const WaveClip*>(&interval)) result = {GetClipGroupId(*audio)};
      else if (const auto *media = dynamic_cast<const LinkedClipInterval*>(&interval)) result = media->MediaGroupIds();
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
      const auto family = families.find(track);
      if (family != families.end()) result.push_back(family->second);
#else
      (void)track;
#endif
      return result;
   };
   size_t before{};
   for (const auto &pair : state.shifters) for (const auto &interval : pair.second->MovingIntervals()) ++before;
   // Closing over relationships is needed when an audio piece selects a joined
   // picture whose other pieces refer to additional soundtrack groups.
   bool expanded{};
   do {
      expanded = false;
      for (const auto &pair : state.shifters) for (const auto &interval : pair.second->MovingIntervals())
         for (const auto &group : groupsOf(*interval, pair.first))
            if (!group.empty()) expanded |= groups.insert(group).second;
      for (auto &pair : state.shifters)
         pair.second->UnfixIntervals([&](const auto &interval) {
            for (const auto &group : groupsOf(interval, pair.first)) if (!group.empty() && groups.count(group)) return true;
            return false;
         });
   } while (expanded);
   size_t after{};
   for (const auto &pair : state.shifters) after += pair.second->MovingIntervals().size();
   return after != before;

}
}

namespace {
using namespace MenuRegistry;
struct AudioClipChoice {
   WaveTrack *track;
   WaveClip *clip;
   size_t trackIndex, clipIndex;
   wxString key, label;
};

std::vector<AudioClipChoice> AudioClipChoices(AudacityProject &project)
{
   std::vector<AudioClipChoice> result;
   size_t trackIndex{};
   for (auto *track : TrackList::Get(project)) {
      if (auto *wave = dynamic_cast<WaveTrack *>(track)) {
         size_t clipIndex{};
         for (const auto &clip : wave->SortedIntervalArray()) {
            const auto key = wxString::Format("%zu:%zu", trackIndex, clipIndex);
            const auto label = wxString::Format("%zu. %s / %s [%.3f .. %.3f]%s",
               trackIndex + 1, track->GetName(), clip->GetName(),
               clip->GetPlayStartTime(), clip->GetPlayEndTime(),
               ClipGroups::GetClipGroupId(*clip).empty() ? "" : " (grouped)");
            result.push_back({ wave, clip.get(), trackIndex, clipIndex++, key, label });
         }
      }
      ++trackIndex;
   }
   return result;
}

bool SelectedClip(const AudioClipChoice &choice, const SelectedRegion &region)
{
   if (!choice.track->GetSelected()) return false;
   const auto start = choice.clip->GetPlayStartTime(), end = choice.clip->GetPlayEndTime();
   return region.isPoint() ? start <= region.t0() && region.t0() < end
      : start < region.t1() && end > region.t0();
}

class EditAudioClipGroupCommand : public AudacityCommand {
   wxString mClips;
protected:
   virtual bool Ungroup() const { return false; }
public:
   TranslatableString GetDescription() const override
   { return XO("Create or remove a saved group of audio clips."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   { visitor.Define(mClips, wxT("Clips"), wxString{}); return true; }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool PromptUser(AudacityProject &project) override
   {
      const auto clips = AudioClipChoices(project);
      wxArrayString labels;
      wxArrayInt selected;
      for (size_t i = 0; i < clips.size(); ++i) {
         labels.Add(clips[i].label);
         if (SelectedClip(clips[i], ViewInfo::Get(project).selectedRegion)) selected.Add(int(i));
      }
      wxMultiChoiceDialog dialog{ &ProjectWindow::Get(project),
         Ungroup() ? _("Choose clips whose groups should be removed.")
                   : _("Choose clips to group. Existing groups are kept together."),
         Ungroup() ? _("Ungroup Audio Clips") : _("Group Audio Clips"), labels };
      dialog.SetSelections(selected);
      if (dialog.ShowModal() != wxID_OK) return false;
      mClips.clear();
      for (const auto index : dialog.GetSelections()) {
         if (!mClips.empty()) mClips += ",";
         mClips += clips.at(index).key;
      }
      return !mClips.empty();
   }
   bool Apply(const CommandContext &context) override
   {
      auto &project = context.project;
      const auto choices = AudioClipChoices(project);
      WaveClipPointers chosen;
      if (mClips.empty()) {
         for (const auto &choice : choices)
            if (SelectedClip(choice, ViewInfo::Get(project).selectedRegion)) chosen.push_back(choice.clip);
      }
      else for (const auto &key : wxSplit(mClips, ',')) {
         const auto found = std::find_if(choices.begin(), choices.end(),
            [&](const auto &choice) { return choice.key == key; });
         if (found == choices.end()) {
            context.Error(_("Unknown audio clip. Use zero-based track:clip pairs, separated by commas."));
            return false;
         }
         if (std::find(chosen.begin(), chosen.end(), found->clip) == chosen.end()) chosen.push_back(found->clip);
      }
      auto members = ClipGroups::ExpandClipGroupMembers(TrackList::Get(project), chosen);
      if (members.size() < (Ungroup() ? 1u : 2u)) {
         context.Error(_("Choose at least two audio clips to group, or one grouped clip to ungroup."));
         return false;
      }
      std::set<std::string> groups;
      for (const auto *clip : members)
         if (!ClipGroups::GetClipGroupId(*clip).empty()) groups.insert(ClipGroups::GetClipGroupId(*clip));
      const std::string group = Ungroup() ? std::string{}
         : groups.size() == 1 ? *groups.begin() : ClipGroups::CreateClipGroupId();
      if (std::all_of(members.begin(), members.end(), [&](const auto *clip) {
         return ClipGroups::GetClipGroupId(*clip) == group; })) return true;
      // Allocate before the first change; applying the prepared IDs only moves strings.
      std::vector<std::string> prepared(members.size(), group);
      for (size_t i = 0; i < members.size(); ++i)
         ClipGroups::SetClipGroupId(*members[i], std::move(prepared[i]));
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
      ClipGroups::UpdateVideoGroupIds(project, {groups.begin(), groups.end()}, group);
#endif
      ProjectHistory::Get(project).PushState(
         Ungroup() ? XO("Ungrouped audio clips") : XO("Grouped audio clips"),
         Ungroup() ? XO("Ungroup Clips") : XO("Group Clips"));
      Viewport::Get(project).Redraw();
      return true;
   }
};

class GroupAudioClipsCommand final : public EditAudioClipGroupCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol GroupAudioClipsCommand::Symbol{ XO("GroupAudioClips") };
BuiltinCommandsModule::Registration<GroupAudioClipsCommand> sGroupAudioClips;
class UngroupAudioClipsCommand final : public EditAudioClipGroupCommand {
   bool Ungroup() const override { return true; }
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol UngroupAudioClipsCommand::Symbol{ XO("UngroupAudioClips") };
BuiltinCommandsModule::Registration<UngroupAudioClipsCommand> sUngroupAudioClips;

class GetAudioClipGroupsCommand final : public AudacityCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("List audio clips and their saved group IDs."); }
   bool Apply(const CommandContext &context) override
   {
      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> out{ buffer };
      out.StartArray();
      for (const auto &choice : AudioClipChoices(context.project)) {
         out.StartObject();
         out.Key("track"); out.Uint64(choice.trackIndex);
         out.Key("clip"); out.Uint64(choice.clipIndex);
         out.Key("start"); out.Double(choice.clip->GetPlayStartTime());
         out.Key("end"); out.Double(choice.clip->GetPlayEndTime());
         out.Key("group"); out.String(ClipGroups::GetClipGroupId(*choice.clip).c_str());
         out.EndObject();
      }
      out.EndArray();
      context.Status(wxString::FromUTF8(buffer.GetString()));
      return true;
   }
};
const ComponentInterfaceSymbol GetAudioClipGroupsCommand::Symbol{ XO("GetAudioClipGroups") };
BuiltinCommandsModule::Registration<GetAudioClipGroupsCommand> sGetAudioClipGroups;

class MoveAudioClipGroupCommand final : public AudacityCommand {
   int mTrack{};
   double mAt{}, mStart{};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Move an audio clip and its group to an exact start time."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor)
   {
      visitor.Define(mTrack, wxT("Track"), 0, 0, 100000);
      visitor.Define(mAt, wxT("At"), 0.0, 0.0, 1000000.0);
      visitor.Define(mStart, wxT("Start"), 0.0, 0.0, 1000000.0);
      return true;
   }
   bool VisitSettings(SettingsVisitor &visitor) override { return Settings(visitor); }
   bool VisitSettings(ConstSettingsVisitor &visitor) override { return Settings(visitor); }
   bool Apply(const CommandContext &context) override
   {
      auto &project = context.project;
      auto &tracks = TrackList::Get(project);
      const auto choices = AudioClipChoices(project);
      const auto found = std::find_if(choices.begin(), choices.end(), [&](const auto &choice) {
         return choice.trackIndex == size_t(mTrack) && choice.clip->GetPlayStartTime() <= mAt && mAt < choice.clip->GetPlayEndTime();
      });
      if (found == choices.end()) { context.Error(_("No audio clip at that track and time.")); return false; }
      const auto desired = mStart - found->clip->GetPlayStartTime();
      if (std::abs(desired) < 1e-10) return true;
      auto shifter = MakeTrackShifter::Call(*found->track, project);
      shifter->UnfixIntervals([&](const auto &interval) { return &interval == found->clip; });
      ClipMoveState state;
      state.Init(project, *found->track, TrackShifter::HitTestResult::Intervals,
         std::move(shifter), mAt, ViewInfo::Get(project), tracks, false);
      if (ClipActions::MovingMediaHasLockedClips(state)) {
         context.Error(_("Unlock the clip in Clip Properties before moving its group.")); return false;
      }
      for (const auto &pair : state.shifters) {
         if (std::abs(pair.second->AdjustOffsetSmaller(desired) - desired) > 1e-10) {
            context.Error(_("There is not enough free space to move the entire clip group."));
            return false;
         }
         for (const auto &clip : pair.second->MovingIntervals())
            if (clip->Start() + desired < -1e-10) {
               context.Error(_("The clip group would move before time zero.")); return false;
            }
      }
      state.DoSlideHorizontal(desired);
      ProjectHistory::Get(project).PushState(XO("Moved audio clip group"), XO("Move Clip Group"));
      Viewport::Get(project).Redraw();
      return true;
   }
};
const ComponentInterfaceSymbol MoveAudioClipGroupCommand::Symbol{ XO("MoveAudioClipGroup") };
BuiltinCommandsModule::Registration<MoveAudioClipGroupCommand> sMoveAudioClipGroup;

AttachedItem sAudioClipGroupMenu{
   Section("PersistentGroups",
      Command("GroupAudioClips", XXO("&Group Clips..."), CommandDispatch::OnAudacityCommand,
         AudioIONotBusyFlag() | WaveTracksExistFlag()),
      Command("UngroupAudioClips", XXO("&Ungroup Clips..."), CommandDispatch::OnAudacityCommand,
         AudioIONotBusyFlag() | WaveTracksExistFlag())),
   wxT("Edit/Other/Clip")
};

AttachedItem sAudioClipGroupScripting{
   Items("",
      Command("GetAudioClipGroups", XXO("Get Audio Clip Groups"), CommandDispatch::OnAudacityCommand, AlwaysEnabledFlag),
      Command("MoveAudioClipGroup", XXO("Move Audio Clip Group..."), CommandDispatch::OnAudacityCommand,
         AudioIONotBusyFlag() | WaveTracksExistFlag())),
   wxT("Optional/Extra/Part2/Scriptables2")
};

// Register menu items


}
