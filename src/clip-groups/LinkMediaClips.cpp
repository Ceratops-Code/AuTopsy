#include "LinkMediaClips.h"
#include "GroupAudioClips.h"
#include "EditClipPlacement.h"
#include "../video/CreateVideoTrack.h"
#include "../video/PlanVideoEdits.h"
#include "TimeStretching.h"
#include "WaveClip.h"
#include "WaveTrack.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectAudioIO.h"
#include "ProjectWindow.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "CommandContext.h"
#include "MenuRegistry.h"
#include "SettingsVisitor.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include "tracks/ui/TimeShiftHandle.h"
#include <wx/choicdlg.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <limits>
#include <map>
#include <set>
namespace {
thread_local ClipGroups::ScopedVideoAudioImportLinks *sImportLinks{};
struct MediaChoice {
   Track *track{};
   WaveClip *audio{};
   AuTopsyVideoTrack *video{};
   uint64_t id{};
   size_t trackIndex{}, clipIndex{};
   double start{}, end{};
   std::string group;
   wxString key, label;
};
std::vector<MediaChoice> MediaChoices(AudacityProject &project)
{
   std::vector<MediaChoice> result;
   size_t trackIndex{};
   for (auto *track : TrackList::Get(project)) {
      size_t index{};
      if (auto *audio = dynamic_cast<WaveTrack*>(track))
         for (const auto &clip : audio->SortedIntervalArray())
            result.push_back({track, clip.get(), nullptr, 0, trackIndex, index++,
               clip->GetPlayStartTime(), clip->GetPlayEndTime(), ClipGroups::GetClipGroupId(*clip)});
      else if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track))
         for (const auto &clip : video->Timeline().clips)
            result.push_back({track, nullptr, video, clip.id, trackIndex, index++,
               clip.start, clip.End(), clip.linkGroup});
      ++trackIndex;
   }
   for (auto &choice : result) {
      choice.key = wxString::Format("%zu:%zu", choice.trackIndex, choice.clipIndex);
      choice.label = wxString::Format("%zu. %s / %s %.3f .. %.3f%s",
         choice.trackIndex + 1, choice.track->GetName(), choice.video ? _("Video") : _("Audio"),
         choice.start, choice.end, choice.group.empty() ? wxString{} : _(" (linked/grouped)"));
   }
   return result;
}
std::vector<std::string> ChoiceGroups(const MediaChoice &choice)
{
   if (!choice.video) return choice.group.empty() ? std::vector<std::string>{} : std::vector<std::string>{choice.group};
   for (const auto &clip : choice.video->Timeline().clips)
      if (clip.id == choice.id) return VideoTimeline::VideoContentLinkGroups(clip);
   throw std::runtime_error("A linked video clip was removed.");
}
bool HasGroup(const MediaChoice &choice, const std::string &group)
{
   const auto groups = ChoiceGroups(choice);
   return std::find(groups.begin(), groups.end(), group) != groups.end();
}
bool Selected(const MediaChoice &choice, const SelectedRegion &region)
{
   return choice.track->GetSelected() && (region.isPoint()
      ? choice.start <= region.t0() && region.t0() < choice.end
      : choice.start < region.t1() && choice.end > region.t0());
}
// Allocate candidates and strings before either media type changes.
bool ApplyGroups(AudacityProject &project, const std::vector<MediaChoice> &members,
   const std::string &group, bool history)
{
   std::map<AuTopsyVideoTrack*, VideoTimeline::Timeline> candidates, originals;
   std::vector<std::pair<WaveClip*, std::string>> audio, oldAudio;
   for (const auto &member : members) {
      const auto previousGroups = ChoiceGroups(member);
      if (member.group == group && std::all_of(previousGroups.begin(), previousGroups.end(),
            [&](const auto &old) { return old == group; })) continue;
      if (member.audio) {
         audio.emplace_back(member.audio, group); oldAudio.emplace_back(member.audio, member.group);
      }
      else {
         if (!candidates.count(member.video)) {
            originals[member.video] = member.video->Timeline();
            candidates[member.video] = member.video->Timeline();
         }
         auto &clips = candidates[member.video].clips;
         const auto found = std::find_if(clips.begin(), clips.end(),
            [&](const auto &clip) { return clip.id == member.id; });
         if (found == clips.end()) throw std::runtime_error("A media clip was removed.");
         std::vector<std::pair<std::string, std::string>> mapping;
         for (const auto &old : VideoTimeline::VideoContentLinkGroups(*found)) mapping.emplace_back(old, group);
         VideoTimeline::RemapVideoContentLinks(*found, mapping);
         found->linkGroup = group;
      }
   }
   if (audio.empty() && candidates.empty()) return false;
   for (const auto &entry : candidates) VideoTimeline::RequireWellFormedTimeline(entry.second);
   try {
      for (auto &entry : candidates) entry.first->SetTimeline(std::move(entry.second));
      for (auto &entry : audio) ClipGroups::SetClipGroupId(*entry.first, std::move(entry.second));
      if (history) ProjectHistory::Get(project).PushState(
         group.empty() ? XO("Unlinked media clips") : XO("Linked media clips"),
         group.empty() ? XO("Unlink Clips") : XO("Link Clips"));
   }
   catch (...) {
      for (auto &entry : originals) entry.first->SetTimeline(std::move(entry.second));
      for (auto &entry : oldAudio) ClipGroups::SetClipGroupId(*entry.first, std::move(entry.second));
      throw;
   }
   return true;
}

// Bound changes are checked together before publishing either media type.
// Native WaveClip trims are measured in project seconds, including stretched
// clips; source coordinates remain owned by each media implementation.
struct MediaBounds {
   MediaChoice member;
   double start{}, end{};
   bool retainEmpty{};
};
void ApplyMediaBounds(AudacityProject &project, const std::vector<MediaBounds> &bounds,
   bool history)
{
   std::map<AuTopsyVideoTrack*, VideoTimeline::Timeline> video;
   std::map<WaveClip*, std::pair<double, double>> audio;
   for (const auto &change : bounds) {
      const auto &member = change.member;
      if (member.audio ? ClipGroups::IsAudioClipTimingLocked(*member.audio)
                       : std::any_of(member.video->Timeline().clips.begin(), member.video->Timeline().clips.end(),
                          [&](const auto &clip) { return clip.id == member.id && clip.timingLocked; }))
         throw std::runtime_error("Unlock the linked clip in Clip Properties before trimming it.");
      if (member.audio) {
         if ((change.end > change.start || change.retainEmpty) &&
             (change.start < member.audio->GetSequenceStartTime() - 1e-7 ||
              change.end > member.audio->GetSequenceEndTime() + 1e-7 ||
              (change.end < change.start || (!change.retainEmpty && change.end - change.start < 1.0 / member.audio->GetRate()))))
            throw std::runtime_error("A linked audio clip cannot be trimmed to that boundary.");
         audio[member.audio] = {change.start, change.end};
      }
      else {
         auto inserted = video.emplace(member.video, member.video->Timeline());
         auto &timeline = inserted.first->second;
         if (change.end <= change.start) VideoTimeline::RemoveClip(timeline, member.id);
         else VideoTimeline::TrimVideoClip(timeline, member.id, change.start, change.end);
      }
   }
   // Extending a linked trim must also leave every other interval intact.
   for (auto *track : TrackList::Get(project).Any<WaveTrack>()) {
      double end = -std::numeric_limits<double>::infinity();
      for (const auto &clip : track->SortedIntervalArray()) {
         const auto changed = audio.find(clip.get());
         const auto start = changed == audio.end() ? clip->GetPlayStartTime() : changed->second.first;
         const auto stop = changed == audio.end() ? clip->GetPlayEndTime() : changed->second.second;
         if (stop <= start) continue;
         if (start < end - 1e-7)
            throw std::runtime_error("A linked trim would overlap another audio clip.");
         end = stop;
      }
   }
   for (const auto &entry : video) VideoTimeline::RequireWellFormedTimeline(entry.second);
   try {
      for (auto &entry : video) entry.first->SetTimeline(std::move(entry.second));
      for (const auto &change : bounds) if (change.member.audio) {
         auto &clip = *change.member.audio;
         if (change.end <= change.start && !change.retainEmpty) {
            auto *track = static_cast<WaveTrack*>(change.member.track);
            const auto clips = track->SortedIntervalArray();
            const auto found = std::find_if(clips.begin(), clips.end(),
               [&](const auto &candidate) { return candidate.get() == &clip; });
            if (found == clips.end()) throw std::runtime_error("A linked audio clip was removed.");
            track->RemoveInterval(*found);
         }
         else {
            // Expand first, then cut the requested window. This also restores a
            // piece whose previous visible interval was empty.
            clip.TrimRightTo(clip.GetSequenceEndTime());
            clip.TrimLeftTo(change.start);
            clip.TrimRightTo(change.end);
         }
      }
      if (history) ProjectHistory::Get(project).PushState(XO("Trimmed linked media clips"), XO("Trim Clips"));
   }
   catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
}

class EditMediaLinksCommand : public AudacityCommand {
   wxString mClips;
protected:
   virtual bool Unlink() const { return false; }
public:
   TranslatableString GetDescription() const override { return XO("Link or unlink video and audio clips."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v)
   { v.Define(mClips, wxT("Clips"), wxString{}); return true; }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool PromptUser(AudacityProject &project) override {
      const auto choices = MediaChoices(project);
      wxArrayString labels; wxArrayInt selected;
      for (size_t i{}; i < choices.size(); ++i) {
         labels.Add(choices[i].label);
         if (Selected(choices[i], ViewInfo::Get(project).selectedRegion)) selected.Add(int(i));
      }
      wxMultiChoiceDialog dialog{&ProjectWindow::Get(project),
         Unlink() ? _("Choose clips to unlink.") : _("Choose video and audio clips to keep together."),
         Unlink() ? _("Unlink Media Clips") : _("Link Media Clips"), labels};
      dialog.SetSelections(selected);
      if (dialog.ShowModal() != wxID_OK) return false;
      mClips.clear();
      for (const auto index : dialog.GetSelections()) {
         if (!mClips.empty()) mClips += ",";
         mClips += choices.at(index).key;
      }
      return !mClips.empty();
   }
   bool Apply(const CommandContext &context) override {
      try {
         if (ProjectAudioIO::Get(context.project).IsAudioActive())
            throw std::runtime_error("Stop playback or recording before editing media links.");
         const auto choices = MediaChoices(context.project);
         std::set<wxString> keys;
         if (mClips.empty()) {
            for (const auto &choice : choices)
               if (Selected(choice, ViewInfo::Get(context.project).selectedRegion)) keys.insert(choice.key);
         }
         else for (const auto &key : wxSplit(mClips, ',')) keys.insert(key);
         std::vector<MediaChoice> members;
         std::set<std::string> groups;
         bool audio{}, video{};
         for (const auto &key : keys) {
            const auto found = std::find_if(choices.begin(), choices.end(),
               [&](const auto &choice) { return choice.key == key; });
            if (found == choices.end()) throw std::runtime_error("Unknown media clip; use zero-based track:clip pairs.");
            members.push_back(*found); audio |= found->audio != nullptr; video |= found->video != nullptr;
            for (const auto &group : ChoiceGroups(*found)) groups.insert(group);
         }
         if (members.empty() || (!Unlink() && (!audio || !video)))
            throw std::runtime_error("Choose at least one video clip and one audio clip to link.");
         for (const auto &choice : choices)
            if (!keys.count(choice.key)) {
               const auto related = ChoiceGroups(choice);
               if (std::any_of(related.begin(), related.end(), [&](const auto &group) { return groups.count(group); }))
                  members.push_back(choice);
            }
         const auto group = Unlink() ? std::string{}
            : groups.size() == 1 ? *groups.begin() : ClipGroups::CreateClipGroupId();
         ApplyGroups(context.project, members, group, true);
         Viewport::Get(context.project).Redraw(); return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
class LinkMediaClipsCommand final : public EditMediaLinksCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
class UnlinkMediaClipsCommand final : public EditMediaLinksCommand {
   bool Unlink() const override { return true; }
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol LinkMediaClipsCommand::Symbol{XO("LinkMediaClips")};
const ComponentInterfaceSymbol UnlinkMediaClipsCommand::Symbol{XO("UnlinkMediaClips")};
BuiltinCommandsModule::Registration<LinkMediaClipsCommand> sLink;
BuiltinCommandsModule::Registration<UnlinkMediaClipsCommand> sUnlink;
class GetLinkedMediaClipsCommand final : public AudacityCommand {
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("List saved video/audio links."); }
   bool Apply(const CommandContext &context) override {
      rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out{buffer};
      out.StartArray();
      for (const auto &choice : MediaChoices(context.project)) {
         out.StartObject(); out.Key("track"); out.Uint64(choice.trackIndex);
         out.Key("clip"); out.Uint64(choice.clipIndex); out.Key("kind"); out.String(choice.video ? "video" : "audio");
         out.Key("id"); out.Uint64(choice.id); out.Key("start"); out.Double(choice.start);
         out.Key("end"); out.Double(choice.end); out.Key("group"); out.String(choice.group.c_str()); out.EndObject();
      }
      out.EndArray(); context.Status(wxString::FromUTF8(buffer.GetString())); return true;
   }
};
const ComponentInterfaceSymbol GetLinkedMediaClipsCommand::Symbol{XO("GetLinkedMediaClips")};
BuiltinCommandsModule::Registration<GetLinkedMediaClipsCommand> sGetLinks;
using namespace MenuRegistry;
AttachedItem sLinksMenu{
   Section("MediaLinks",
      Command("LinkMediaClips", XXO("Link Video / Audio Clips..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag()),
      Command("UnlinkMediaClips", XXO("Unlink Video / Audio Clips..."), CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag())),
   wxT("Edit/Other/Clip")
};
AttachedItem sLinksInfo{
   Command("GetLinkedMediaClips", XXO("Get Linked Media Clips"), CommandDispatch::OnAudacityCommand, AlwaysEnabledFlag),
   wxT("Optional/Extra/Part2/Scriptables2")
};
}
namespace ClipGroups {
void MediaCopyGroups::AssignToCopiedTrack(Track &track)
{
   const auto copyGroup = [&](const std::string &id) {
      if (id.empty()) return id;
      auto found = mGroups.find(id);
      if (found == mGroups.end())
         found = mGroups.emplace(id, CreateClipGroupId()).first;
      return found->second;
   };
   if (auto *wave = dynamic_cast<WaveTrack*>(&track)) {
      for (const auto &clip : wave->Intervals())
         SetClipGroupId(*clip, copyGroup(GetClipGroupId(*clip)));
   }
   else if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(&track)) {
      auto timeline = video->Timeline();
      for (auto &clip : timeline.clips) {
         std::vector<std::pair<std::string, std::string>> mapping;
         for (const auto &group : VideoTimeline::VideoContentLinkGroups(clip)) mapping.emplace_back(group, copyGroup(group));
         VideoTimeline::RemapVideoContentLinks(clip, mapping);
      }
      video->SetTimeline(std::move(timeline));
   }
}
std::shared_ptr<const TrackList> CopyMediaForPaste(const TrackList &source)
{
   // Prepare independent copies before native paste can change the project.
   // Sample blocks remain shared through the host's ordinary Duplicate contract.
   auto copies = TrackList::Create(nullptr);
   MediaCopyGroups groups;
   for (const auto *track : source) {
      auto copy = track->Duplicate();
      groups.AssignToCopiedTrack(*copy);
      copies->Add(std::move(copy));
   }
   return copies;
}
ScopedVideoAudioImportLinks::ScopedVideoAudioImportLinks(AudacityProject &project, AuTopsyVideoTrack &picture, double projectStart)
   : mProject(project), mPicture(picture), mProjectStart(projectStart), mPrevious(sImportLinks)
{
   for (auto *track : TrackList::Get(project).Any<WaveTrack>()) mExistingAudio.push_back(track);
   sImportLinks = this;
}
ScopedVideoAudioImportLinks::~ScopedVideoAudioImportLinks()
{ sImportLinks = mPrevious; }
void LinkNewlyImportedVideoAudio(AudacityProject &project)
{
   for (auto *scope = sImportLinks; scope; scope = scope->mPrevious) if (&scope->mProject == &project) {
      std::vector<WaveTrack*> imported;
      for (auto *track : TrackList::Get(project).Any<WaveTrack>())
         if (std::find(scope->mExistingAudio.begin(), scope->mExistingAudio.end(), track) ==
             scope->mExistingAudio.end()) imported.push_back(track);
       // Placement occurs before the importer's one Undo snapshot, never as a
       // second edit that could leave picture and sound at different positions.
       if (scope->mProjectStart != 0)
          for (auto *track : imported) track->ShiftBy(0, scope->mProjectStart);
       LinkImportedMedia(project, scope->mPicture, imported);
      return;
   }
}
void LinkImportedMedia(AudacityProject &project, AuTopsyVideoTrack &picture, const std::vector<WaveTrack*> &audio)
{
   std::vector<MediaChoice> members;
   for (const auto &choice : MediaChoices(project))
      if (choice.video == &picture || std::find(audio.begin(), audio.end(), choice.track) != audio.end())
         members.push_back(choice);
   if (members.size() > picture.Timeline().clips.size()) ApplyGroups(project, members, CreateClipGroupId(), false);
}

bool JoinSelectedMediaClips(AudacityProject &project, double from, double to)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before joining clips.");
   VideoEditing::VideoProjectSnapshot snapshot;
   std::vector<AuTopsyVideoTrack*> pictures;
   for (auto *track : VideoTimeline::ProjectVideoTracks(project)) if (track->GetSelected()) {
      snapshot.tracks.push_back({std::to_string(pictures.size()), 0, track->Timeline()});
      pictures.push_back(track);
   }
   std::vector<VideoEditing::VideoEditRequest> requests;
   for (const auto &track : snapshot.tracks)
      requests.push_back({VideoEditing::VideoEditKind::JoinClips, track.id, 0, from, to});
   const auto plan = VideoEditing::PlanVideoEdits(snapshot, requests);
   auto candidate = plan.After();
   struct SoundJoin { WaveTrack *track; std::set<std::string> groups; double start, end; };
   std::vector<SoundJoin> sound;
   std::vector<std::set<std::string>> components;
   const auto mergeLinkedGroups = [&](std::set<std::string> merged) {
      for (auto it = components.begin(); it != components.end();) {
         if (std::any_of(it->begin(), it->end(), [&](const auto &id) { return merged.count(id) != 0; })) {
            merged.insert(it->begin(), it->end()); it = components.erase(it);
            it = components.begin();
         }
         else ++it;
      }
      components.push_back(std::move(merged));
   };
   for (auto *track : TrackList::Get(project).Selected<WaveTrack>()) {
      SoundJoin join{track}; size_t count{};
      for (const auto &clip : track->SortedIntervalArray())
         if (clip->GetPlayStartTime() < to && clip->GetPlayEndTime() > from) {
            if (!count++) join.start = clip->GetPlayStartTime();
            join.end = clip->GetPlayEndTime();
            const auto group = GetClipGroupId(*clip);
            if (!group.empty()) join.groups.insert(group);
         }
      if (count < 2) continue;
      if (!join.groups.empty()) mergeLinkedGroups(join.groups);
      sound.push_back(std::move(join));
   }
   if (sound.empty() && plan.ChangedTrackIds().empty()) return false;
   // One joined picture can span links to several selected soundtrack tracks.
   // Connect their new intervals through that parent instead of rejecting the
   // otherwise ordinary join or arbitrarily choosing only one soundtrack.
   for (size_t i{}; i < pictures.size(); ++i)
      for (const auto &clip : candidate.tracks[i].timeline.clips) {
         const auto old = std::find_if(snapshot.tracks[i].timeline.clips.begin(), snapshot.tracks[i].timeline.clips.end(),
            [&](const auto &value) { return value.id == clip.id; });
         if (!clip.content || (old != snapshot.tracks[i].timeline.clips.end() && old->content == clip.content)) continue;
         const auto groups = VideoTimeline::VideoContentLinkGroups(clip);
         if (std::any_of(components.begin(), components.end(), [&](const auto &component) {
               return std::any_of(groups.begin(), groups.end(), [&](const auto &group) { return component.count(group) != 0; });
            })) mergeLinkedGroups({groups.begin(), groups.end()});
      }
   std::map<std::string, std::string> joinedGroups;
   for (const auto &component : components) {
      const auto group = CreateClipGroupId();
      for (const auto &old : component) joinedGroups.emplace(old, group);
   }
   for (size_t i{}; i < pictures.size(); ++i)
      for (auto &clip : candidate.tracks[i].timeline.clips) {
         const auto old = std::find_if(snapshot.tracks[i].timeline.clips.begin(), snapshot.tracks[i].timeline.clips.end(),
            [&](const auto &value) { return value.id == clip.id; });
         if (!clip.content || (old != snapshot.tracks[i].timeline.clips.end() && old->content == clip.content)) continue;
         const auto groups = VideoTimeline::VideoContentLinkGroups(clip);
         std::set<std::string> matches;
         for (const auto &group : groups) if (const auto found = joinedGroups.find(group); found != joinedGroups.end())
            matches.insert(found->second);
         if (matches.size() > 1) throw std::runtime_error("Join these independent soundtrack groups separately.");
         // A fresh parent link represents the whole joined soundtrack. Retained
         // child links still describe the original pieces for Separate/Undo.
         if (!matches.empty()) clip.linkGroup = *matches.begin();
      }
   try {
      for (auto &join : sound) {
         TimeStretching::WithClipRenderingProgress([&](const ProgressReporter &progress) {
            join.track->Join(from, to, progress);
         });
         if (!join.groups.empty())
            for (const auto &clip : join.track->SortedIntervalArray())
               if (clip->GetPlayStartTime() < join.end && clip->GetPlayEndTime() > join.start)
                  SetClipGroupId(*clip, joinedGroups.at(*join.groups.begin()));
      }
      for (size_t i{}; i < pictures.size(); ++i) pictures[i]->SetTimeline(candidate.tracks[i].timeline);
      ProjectHistory::Get(project).PushState(XO("Joined selected media clips"), XO("Join"));
   }
   catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
   Viewport::Get(project).HandleResize();
   return true;
}
void UpdateVideoGroupIds(AudacityProject &project, const std::vector<std::string> &oldGroups, const std::string &newGroup)
{
   std::vector<MediaChoice> members;
   for (const auto &choice : MediaChoices(project))
      if (choice.video && !choice.group.empty() &&
          std::find(oldGroups.begin(), oldGroups.end(), choice.group) != oldGroups.end())
         members.push_back(choice);
   ApplyGroups(project, members, newGroup, false);
}
bool MoveLinkedVideoClip(AudacityProject &project, AuTopsyVideoTrack &track,
   unsigned long long id, double start, bool recordHistory)
{
   const auto &clips = track.Timeline().clips;
   const auto found = std::find_if(clips.begin(), clips.end(), [id](const auto &clip) { return clip.id == id; });
   if (found == clips.end()) throw std::runtime_error("Unknown video clip.");
   const auto rate = VideoTimeline::VideoContentFrameRate(*found, VideoTimeline::SourceFor(track.Timeline(), *found)) * found->playbackRate;
   const auto desired = std::round(start * rate) / rate - found->start;
   if (std::abs(desired) < 1e-10) return false;
   auto shifter = MakeTrackShifter::Call(track, project);
   shifter->UnfixIntervals([id](const auto &interval) {
      const auto *media = dynamic_cast<const LinkedClipInterval*>(&interval);
      return media && media->VideoClipId() == id;
   });
   ClipMoveState state;
   state.Init(project, track, TrackShifter::HitTestResult::Intervals, std::move(shifter),
      (found->start + found->End()) / 2, ViewInfo::Get(project), TrackList::Get(project), false);
   if (ClipActions::MovingMediaHasLockedClips(state))
      throw std::runtime_error("Unlock the linked clip in Clip Properties before moving it.");
   for (const auto &pair : state.shifters)
      if (std::abs(pair.second->AdjustOffsetSmaller(desired) - desired) > 1e-10)
         throw std::runtime_error("There is not enough free space for the linked clips.");
   try {
      state.DoSlideHorizontal(desired);
      if (recordHistory) ProjectHistory::Get(project).PushState(XO("Moved linked media clips"), XO("Move Linked Clips"));
   }
   catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
   return true;
}
bool ChangeLinkedVideoClipSpeed(AudacityProject &project, AuTopsyVideoTrack &track,
   unsigned long long id, double playbackRate)
{
   const auto found = std::find_if(track.Timeline().clips.begin(), track.Timeline().clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == track.Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
   return ChangeLinkedMediaClipTiming(project, track, nullptr, id, found->start, playbackRate);
}
bool ChangeLinkedMediaClipTiming(AudacityProject &project, Track &track,
   WaveClip *audioAnchor, unsigned long long id, double start, double playbackRate, bool recordHistory)
{
   if (track.GetOwner().get() != &TrackList::Get(project))
      throw std::runtime_error("The clip's track was removed while its properties were open.");
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before changing clip timing.");
   VideoTimeline::Clip anchor;
   auto *videoAnchor = dynamic_cast<AuTopsyVideoTrack*>(&track);
   if (videoAnchor) {
      const auto &clips = videoAnchor->Timeline().clips;
      const auto found = std::find_if(clips.begin(), clips.end(), [id](const auto &clip) { return clip.id == id; });
      if (found == clips.end()) throw std::runtime_error("The video clip was removed.");
      anchor = *found;
   }
   else {
      auto *wave = dynamic_cast<WaveTrack*>(&track);
      if (!wave || !audioAnchor) throw std::runtime_error("The audio clip was removed.");
      const auto clips = wave->Intervals();
      if (std::none_of(clips.begin(), clips.end(), [&](const auto &clip) { return clip.get() == audioAnchor; }))
         throw std::runtime_error("The audio clip was removed.");
      anchor.start = audioAnchor->GetPlayStartTime();
      anchor.duration = audioAnchor->GetPlayEndTime() - anchor.start;
      anchor.playbackRate = 1 / audioAnchor->GetStretchRatio();
   }
   auto timing = VideoTimeline::PrepareVideoClipRetiming(anchor, playbackRate);
   if (!std::isfinite(start) || start < 0) throw std::runtime_error("Enter a non-negative clip start.");
   timing.shift = start - anchor.start;
   if (std::abs(timing.timeScale - 1) < 1e-12 && std::abs(timing.shift) < 1e-10) return false;
   const auto selectedStart = start, selectedEnd = start + anchor.duration * timing.timeScale;
   const auto choices = MediaChoices(project);
   std::vector<bool> included(choices.size());
   std::vector<std::vector<std::string>> ownGroups;
   std::map<std::string, std::vector<size_t>> linked;
   std::vector<size_t> pending;
   for (size_t i{}; i < choices.size(); ++i) {
      ownGroups.push_back(ChoiceGroups(choices[i]));
      for (const auto &group : ownGroups.back()) linked[group].push_back(i);
      if ((videoAnchor && choices[i].video == videoAnchor && choices[i].id == id) ||
          (audioAnchor && choices[i].audio == audioAnchor)) {
         included[i] = true; pending.push_back(i);
      }
   }
   // A joined clip may connect several soundtrack groups. Resolve that closure
   // before making candidates so no linked member is left at its old speed.
   std::set<std::string> expandedGroups;
   for (size_t cursor{}; cursor < pending.size(); ++cursor)
      for (const auto &group : ownGroups[pending[cursor]]) if (expandedGroups.insert(group).second)
         for (const auto member : linked.at(group)) if (!included[member]) {
            included[member] = true; pending.push_back(member);
         }
   std::map<AuTopsyVideoTrack*, VideoTimeline::Timeline> video;
   std::map<WaveClip*, std::pair<double, double>> audio;
   for (size_t i{}; i < choices.size(); ++i) if (included[i]) {
      const auto &choice = choices[i];
      if (choice.audio) {
         if (IsAudioClipTimingLocked(*choice.audio)) throw std::runtime_error("Unlock the linked audio clip before changing its timing.");
         const auto start = timing.ProjectTimeAfterRetiming(choice.start);
         const auto end = timing.ProjectTimeAfterRetiming(choice.end);
         const auto ratio = choice.audio->GetStretchRatio() * timing.timeScale;
         if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end > 100000000 ||
             !std::isfinite(ratio) || ratio < 0.01 || ratio > 100)
            throw std::runtime_error("A linked audio clip cannot fit the requested speed.");
         audio.emplace(choice.audio, std::make_pair(start, end));
      }
      else {
         auto entry = video.find(choice.video);
         if (entry == video.end()) entry = video.emplace(choice.video, choice.video->Timeline()).first;
         auto &candidate = entry->second;
         const auto part = std::find_if(candidate.clips.begin(), candidate.clips.end(),
            [&](const auto &clip) { return clip.id == choice.id; });
         if (part == candidate.clips.end()) throw std::runtime_error("A linked video clip was removed.");
         if (part->timingLocked) throw std::runtime_error("Unlock the linked video clip before changing its timing.");
         *part = VideoTimeline::RetimeVideoClip(*part, VideoTimeline::SourceFor(candidate, *part), timing);
      }
   }
   for (auto &entry : video) {
      std::sort(entry.second.clips.begin(), entry.second.clips.end(),
         [](const auto &a, const auto &b) { return a.start < b.start; });
      VideoTimeline::RequireWellFormedTimeline(entry.second);
   }
   for (auto *audioTrack : TrackList::Get(project).Any<WaveTrack>()) {
      std::vector<std::pair<double, double>> ranges;
      for (const auto &clip : audioTrack->SortedIntervalArray()) {
         const auto changed = audio.find(clip.get());
         ranges.push_back(changed == audio.end()
            ? std::make_pair(clip->GetPlayStartTime(), clip->GetPlayEndTime()) : changed->second);
      }
      std::sort(ranges.begin(), ranges.end());
      double end = -1;
      for (const auto &range : ranges) if (range.second > range.first) {
         if (range.first < end - 1e-7)
            throw std::runtime_error("The speed change would overlap another audio clip.");
         end = range.second;
      }
   }
   // All allocations, link closure and overlap checks precede the effects.
   // The host owns stretching, pitch handling and rollback; media bytes and
   // retained source windows are never copied into a second audio engine.
   try {
      for (auto &entry : video) entry.first->SetTimeline(std::move(entry.second));
      for (const auto &entry : audio) {
         entry.first->StretchBy(timing.timeScale);
         entry.first->SetPlayStartTime(entry.second.first);
      }
      ViewInfo::Get(project).selectedRegion.setTimes(selectedStart, selectedEnd);
      Viewport::Get(project).HandleResize();
      if (recordHistory) ProjectHistory::Get(project).PushState(XO("Changed linked clip timing"), XO("Clip Properties"));
   }
   catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
   return true;
}
bool SplitMediaAtSelection(AudacityProject &project, double from, double to, Track *only)
{
   bool changed{};
   for (const auto requested : std::vector<double>{from, to}) {
      if (requested == to && to == from && changed) break;
      const auto choices = MediaChoices(project);
      std::set<std::string> groupsWithVideo;
      for (const auto &choice : choices) if (choice.video)
         for (const auto &group : ChoiceGroups(choice)) groupsWithVideo.insert(group);
      std::map<std::string, double> cuts;
      for (const auto &choice : choices)
         if ((only ? choice.track == only : choice.track->GetSelected()) &&
             requested > choice.start + 1e-7 && requested < choice.end - 1e-7)
            for (const auto &group : ChoiceGroups(choice))
               if (groupsWithVideo.count(group)) cuts.emplace(group, requested);
      for (auto &entry : cuts) {
         // Imported soundtracks use the source video's exact frame boundary.
         for (const auto &choice : choices) if (choice.video &&
             HasGroup(choice, entry.first) &&
             requested > choice.start + 1e-7 && requested < choice.end - 1e-7) {
            auto trial = choice.video->Timeline();
            const auto next = trial.nextId;
            if (VideoTimeline::SplitClipAt(trial, requested)) {
               const auto right = std::find_if(trial.clips.begin(), trial.clips.end(),
                  [next](const auto &clip) { return clip.id == next; });
               entry.second = right->start;
            }
            break;
         }
      }
      std::map<AuTopsyVideoTrack*, VideoTimeline::Timeline> video;
      std::map<std::string, std::string> rightGroups;
      for (const auto &entry : cuts) rightGroups.emplace(entry.first, CreateClipGroupId());
      std::set<std::pair<WaveTrack*, double>> audioCuts;
      for (const auto &choice : choices) {
         auto cut = cuts.end();
         for (const auto &group : ChoiceGroups(choice)) {
            cut = cuts.find(group); if (cut != cuts.end()) break;
         }
         if (cut == cuts.end() && !(only ? choice.track == only : choice.track->GetSelected())) continue;
         const auto at = cut == cuts.end() ? requested : cut->second;
         // Later members may live on tracks that do not cross this cut. They
         // still need the same right-hand group as their linked soundtrack.
         if (choice.video && cut != cuts.end()) video.emplace(choice.video, choice.video->Timeline());
         if (choice.start + 1e-7 >= at || choice.end - 1e-7 <= at) continue;
         if (choice.audio) audioCuts.emplace(static_cast<WaveTrack*>(choice.track), at);
         else {
            auto inserted = video.emplace(choice.video, choice.video->Timeline());
            changed |= VideoTimeline::SplitClipAt(inserted.first->second, at);
         }
      }
      for (auto &entry : video) for (auto &clip : entry.second.clips) {
         std::vector<std::pair<std::string, std::string>> mapping;
         for (const auto &group : VideoTimeline::VideoContentLinkGroups(clip)) {
            const auto cut = cuts.find(group);
            if (cut != cuts.end() && clip.start >= cut->second -
                0.51 / VideoTimeline::VideoContentFrameRate(clip, VideoTimeline::SourceFor(entry.second, clip)))
               mapping.emplace_back(group, rightGroups.at(group));
         }
         VideoTimeline::RemapVideoContentLinks(clip, mapping);
      }
      try {
         for (const auto &entry : audioCuts) entry.first->Split(entry.second, entry.second);
         for (const auto &choice : MediaChoices(project)) if (choice.audio) {
            const auto cut = cuts.find(choice.group);
            if (cut != cuts.end() && choice.start >= cut->second - 1.0 / choice.audio->GetRate())
               SetClipGroupId(*choice.audio, rightGroups.at(choice.group));
         }
         for (auto &entry : video) entry.first->SetTimeline(std::move(entry.second));
      }
      catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
      changed |= !audioCuts.empty();
      if (to == from) break;
   }
   return changed;
}
void TrimLinkedMediaToSelection(AudacityProject &project, double from, double to)
{
   const auto choices = MediaChoices(project);
   std::set<std::string> groups;
   for (const auto &choice : choices)
      if (choice.track->GetSelected() && !choice.group.empty()) groups.insert(choice.group);
   std::vector<MediaBounds> changes;
   for (const auto &choice : choices)
      if (!choice.track->GetSelected() && groups.count(choice.group))
         changes.push_back({choice, std::max(choice.start, from), std::min(choice.end, to)});
   ApplyMediaBounds(project, changes, false);
}
bool CommitLinkedAudioTrim(AudacityProject &project, WaveClip &clip, bool left, double previous)
{
   const auto group = GetClipGroupId(clip);
   if (group.empty()) return false;
   const auto choices = MediaChoices(project);
   if (std::none_of(choices.begin(), choices.end(), [&](const auto &choice) {
         return choice.video && choice.group == group; })) return false;
   const auto delta = (left ? clip.GetPlayStartTime() : clip.GetPlayEndTime()) - previous;
   std::vector<MediaBounds> changes;
   for (const auto &choice : choices) if (choice.group == group)
      changes.push_back({choice, choice.start + (left && choice.audio != &clip ? delta : 0),
         choice.end + (!left && choice.audio != &clip ? delta : 0)});
   try { ApplyMediaBounds(project, changes, true); }
   catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
   return true;
}
void CommitVideoClipTrim(AudacityProject &project, AuTopsyVideoTrack &track, VideoTimeline::Timeline candidate)
{
   VideoTimeline::RequireWellFormedTimeline(candidate);
   const auto choices = MediaChoices(project);
   std::vector<MediaBounds> changes;
   for (const auto &original : track.Timeline().clips) {
      const auto found = std::find_if(candidate.clips.begin(), candidate.clips.end(),
         [&](const auto &clip) { return clip.id == original.id; });
      if (found == candidate.clips.end()) throw std::runtime_error("A video trim cannot remove a clip.");
      const auto left = found->start - original.start, right = found->End() - original.End();
      if (std::abs(left) < 1e-7 && std::abs(right) < 1e-7) continue;
      if (original.content) {
         const auto windows = VideoTimeline::VideoContentLinkWindows(*found);
         const auto previousWindows = VideoTimeline::VideoContentLinkWindows(original);
         std::map<std::string, VideoTimeline::VideoLinkWindow> ranges;
         for (const auto &window : windows) {
            auto item = ranges.emplace(window.group, window);
            if (!item.second) {
               // Multiple occurrences of one linked group cannot identify a
               // unique soundtrack window. Reject before either medium changes.
               throw std::runtime_error("Separate or unlink repeated soundtrack occurrences before trimming this joined clip.");
            }
         }
         for (const auto &choice : choices) {
            if (choice.video == &track && choice.id == original.id)
               changes.push_back({choice, found->start, found->End()});
            else if (choice.audio && ranges.count(choice.group)) {
               const auto range = ranges.at(choice.group);
                if (range.wholeContent) {
                   // A manually linked soundtrack keeps its own source and
                   // placement. Follow this parent's edge deltas, including
                   // when it is nested inside another joined clip.
                   const auto previous = std::find_if(previousWindows.begin(), previousWindows.end(),
                      [&](const auto &window) { return window.group == range.group; });
                   if (previous == previousWindows.end())
                      throw std::runtime_error("A linked parent soundtrack window was removed.");
                   changes.push_back({choice, choice.start + range.start - previous->start,
                      choice.end + range.end - previous->end, true});
                   continue;
                }
               const auto sourceStart = choice.audio->GetSequenceStartTime();
               const auto sourceEnd = choice.audio->GetSequenceEndTime();
               // Encoded soundtracks can contain a fraction of a frame of
               // padding. Retain it at a physical source edge, never at an
               // internal cut or a fully hidden piece.
               const auto begin = std::clamp(range.start, sourceStart, sourceEnd);
               const auto padding = range.end > range.start && std::abs(range.end - range.sourceEnd) < 1e-7
                  ? sourceEnd - range.sourceEnd : 0.0;
               const auto end = std::clamp(range.end + padding, begin, sourceEnd);
               changes.push_back({choice, begin, end, true});
            }
         }
      }
      else for (const auto &choice : choices)
         if ((choice.video == &track && choice.id == original.id) ||
             (!original.linkGroup.empty() && HasGroup(choice, original.linkGroup)))
            changes.push_back({choice, choice.start + left, choice.end + right});
   }
   if (!changes.empty()) ApplyMediaBounds(project, changes, true);
}

}
