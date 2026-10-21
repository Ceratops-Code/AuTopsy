#include "LinkMediaClips.h"
#include "GroupAudioClips.h"
#include "../video/CreateVideoTrack.h"
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
      if (member.group == group) continue;
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
};
void ApplyMediaBounds(AudacityProject &project, const std::vector<MediaBounds> &bounds,
   bool history)
{
   std::map<AuTopsyVideoTrack*, VideoTimeline::Timeline> video;
   std::map<WaveClip*, std::pair<double, double>> audio;
   for (const auto &change : bounds) {
      const auto &member = change.member;
      if (member.audio) {
         if (change.end > change.start &&
             (change.start < member.audio->GetSequenceStartTime() - 1e-7 ||
              change.end > member.audio->GetSequenceEndTime() + 1e-7 ||
              change.end - change.start < 1.0 / member.audio->GetRate()))
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
      double end = -1;
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
         if (change.end <= change.start) {
            auto *track = static_cast<WaveTrack*>(change.member.track);
            const auto clips = track->SortedIntervalArray();
            const auto found = std::find_if(clips.begin(), clips.end(),
               [&](const auto &candidate) { return candidate.get() == &clip; });
            if (found == clips.end()) throw std::runtime_error("A linked audio clip was removed.");
            track->RemoveInterval(*found);
         }
         else {
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
            if (!found->group.empty()) groups.insert(found->group);
         }
         if (members.empty() || (!Unlink() && (!audio || !video)))
            throw std::runtime_error("Choose at least one video clip and one audio clip to link.");
         for (const auto &choice : choices)
            if (!choice.group.empty() && groups.count(choice.group) && !keys.count(choice.key))
               members.push_back(choice);
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
      for (auto &clip : timeline.clips) clip.linkGroup = copyGroup(clip.linkGroup);
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
ScopedVideoAudioImportLinks::ScopedVideoAudioImportLinks(AudacityProject &project, AuTopsyVideoTrack &picture)
   : mProject(project), mPicture(picture), mPrevious(sImportLinks)
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
   const auto rate = VideoTimeline::SourceFor(track.Timeline(), *found).frameRate;
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
bool SplitMediaAtSelection(AudacityProject &project, double from, double to, Track *only)
{
   bool changed{};
   for (const auto requested : std::vector<double>{from, to}) {
      if (requested == to && to == from && changed) break;
      const auto choices = MediaChoices(project);
      std::set<std::string> groupsWithVideo;
      for (const auto &choice : choices)
         if (choice.video && !choice.group.empty()) groupsWithVideo.insert(choice.group);
      std::map<std::string, double> cuts;
      for (const auto &choice : choices)
         if (groupsWithVideo.count(choice.group) && (only ? choice.track == only : choice.track->GetSelected()) &&
             requested > choice.start + 1e-7 && requested < choice.end - 1e-7)
            cuts.emplace(choice.group, requested);
      for (auto &entry : cuts) {
         // Imported soundtracks use the source video's exact frame boundary.
         for (const auto &choice : choices) if (choice.video && choice.group == entry.first &&
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
         const auto cut = cuts.find(choice.group);
         if (cut == cuts.end() && !(only ? choice.track == only : choice.track->GetSelected())) continue;
         const auto at = cut == cuts.end() ? requested : cut->second;
         if (choice.start + 1e-7 >= at || choice.end - 1e-7 <= at) continue;
         if (choice.audio) audioCuts.emplace(static_cast<WaveTrack*>(choice.track), at);
         else {
            auto inserted = video.emplace(choice.video, choice.video->Timeline());
            changed |= VideoTimeline::SplitClipAt(inserted.first->second, at);
         }
      }
      for (auto &entry : video) for (auto &clip : entry.second.clips) {
         const auto cut = cuts.find(clip.linkGroup);
         if (cut != cuts.end() && clip.start >= cut->second -
             0.51 / VideoTimeline::SourceFor(entry.second, clip).frameRate)
            clip.linkGroup = rightGroups.at(clip.linkGroup);
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
      for (const auto &choice : choices)
         if ((choice.video == &track && choice.id == original.id) ||
             (!original.linkGroup.empty() && choice.group == original.linkGroup))
            changes.push_back({choice, choice.start + left, choice.end + right});
   }
   if (!changes.empty()) ApplyMediaBounds(project, changes, true);
}

}

