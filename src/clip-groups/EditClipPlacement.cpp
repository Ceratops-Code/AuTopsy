#include "EditClipPlacement.h"
#include "SyncLock.h"
#include "GuardMediaDialogs.h"
#include "GroupAudioClips.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectHistory.h"
#include "TrackPanel.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "WaveClip.h"
#include "WaveTrack.h"
#include "tracks/ui/TimeShiftHandle.h"
#include "CommandContext.h"
#include "SettingsVisitor.h"
#include "commands/AudacityCommand.h"
#include "commands/CommandDispatch.h"
#include "commands/LoadCommands.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
#include "LinkMediaClips.h"
#include "../video/CreateVideoTrack.h"
#endif
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <array>
#include <functional>
#include <optional>
#include <set>

namespace {
void ShowClipPropertiesDialog(wxWindow *parent, const ClipActions::ClipTimingProperties &original,
   const std::function<void(const ClipActions::ClipTimingProperties&)> &apply)
{
   wxDialog dialog{parent, wxID_ANY, _("Clip Properties")};
   auto *layout = new wxBoxSizer(wxVERTICAL);
   layout->Add(new wxStaticText(&dialog, wxID_ANY,
      _("Set exact placement. End and duration adjust speed; use clip edges for trimming.\nLinked picture and sound keep their relative timing.")),
      0, wxALL, 12);
   auto *grid = new wxFlexGridSizer(2, 8, 12);
   const wxString names[]{_("Start (seconds)"), _("End (seconds)"), _("Duration (seconds)"), _("Speed (%)")};
   const std::array<double, 4> initial{original.start, original.end, original.end - original.start, original.speed * 100};
   std::array<wxSpinCtrlDouble*, 4> fields{};
   auto current = original;
   for (size_t i{}; i < fields.size(); ++i) {
      grid->Add(new wxStaticText(&dialog, wxID_ANY, names[i]), 0, wxALIGN_CENTER_VERTICAL);
      fields[i] = new wxSpinCtrlDouble(&dialog, wxID_ANY, {}, wxDefaultPosition, wxSize(190, -1),
         wxSP_ARROW_KEYS, i == 3 ? 1 : 0, i == 3 ? 10000 : 100000000, initial[i], i == 3 ? 1 : .01);
      fields[i]->SetDigits(i == 3 ? 5 : 8);
      fields[i]->SetName(names[i]);
      fields[i]->SetValue(initial[i]);
      grid->Add(fields[i], 1, wxEXPAND);
   }
   layout->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);
   auto *locked = new wxCheckBox(&dialog, wxID_ANY, _("Lock timing (protect movement, trimming and deletion)"));
   locked->SetValue(original.locked);
   layout->Add(locked, 0, wxALL, 12);
   layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 12);
   dialog.SetSizerAndFit(layout);
   bool updating{};
   std::array<bool, 4> valid{true, true, true, true};
   const auto synchronize = [&](size_t changed, const wxString &text) {
      if (updating) return;
      double value{};
      valid[changed] = text.ToDouble(&value) && std::isfinite(value);
      if (!valid[changed]) return;
      if ((changed == 0 && value < 0) || (changed == 1 && value <= current.start) ||
          ((changed == 2 || changed == 3) && value <= 0)) {
         valid[changed] = false; return;
      }
      const auto sourceSpan = (original.end - original.start) * original.speed;
      if (changed == 0) { current.end += value - current.start; current.start = value; }
      else if (changed == 1 && value > current.start) {
         current.end = value; current.speed = sourceSpan / (value - current.start);
      }
      else if (changed == 2 && value > 0) {
         current.end = current.start + value; current.speed = sourceSpan / value;
      }
      else if (changed == 3 && value > 0) {
         current.speed = value / 100; current.end = current.start + sourceSpan / current.speed;
      }
      updating = true;
      const std::array<double, 4> values{current.start, current.end, current.end - current.start, current.speed * 100};
      for (size_t i{}; i < fields.size(); ++i) if (i != changed) {
         fields[i]->SetValue(values[i]); valid[i] = true;
      }
      updating = false;
   };
   for (size_t i{}; i < fields.size(); ++i) {
      fields[i]->Bind(wxEVT_SPINCTRLDOUBLE, [&, i](wxSpinDoubleEvent &event) {
         synchronize(i, wxString::Format("%.17g", event.GetValue()));
      });
      fields[i]->Bind(wxEVT_TEXT, [&, i](wxCommandEvent &event) { synchronize(i, event.GetString()); });
   }
   dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
      try {
         if (std::any_of(valid.begin(), valid.end(), [](bool good) { return !good; }))
            throw std::runtime_error("Enter valid positive timing values, with End after Start.");
         if (fields[1]->GetValue() <= fields[0]->GetValue() || fields[2]->GetValue() <= 0)
            throw std::runtime_error("End must be after Start and Duration must be positive.");
         current.locked = locked->GetValue();
         ClipActions::RequireClipTiming(current);
         if (original.locked && current.locked &&
             (current.start != original.start || current.end != original.end || current.speed != original.speed))
            throw std::runtime_error("Uncheck Lock timing to change this clip's timing.");
         apply(current); dialog.EndModal(wxID_OK);
      }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Clip Properties"), wxOK | wxICON_ERROR, &dialog);
      }
   }, wxID_OK);
   dialog.ShowModal();
}
void RequireIdle(AudacityProject &project, Track &track)
{
   if (ProjectAudioIO::Get(project).IsAudioActive()) throw std::runtime_error("Stop playback or recording before changing clip timing.");
   if (track.GetOwner().get() != &TrackList::Get(project)) throw std::runtime_error("The clip's track was removed.");
}
}

namespace ClipActions {
bool MovingMediaHasLockedClips(const ClipMoveState &state)
{
   for (const auto &entry : state.shifters) for (const auto &interval : entry.second->MovingIntervals()) {
      if (const auto *audio = dynamic_cast<const WaveClip*>(interval.get())) {
         if (ClipGroups::IsAudioClipTimingLocked(*audio)) return true;
      }
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
      else if (const auto *media = dynamic_cast<const ClipGroups::LinkedClipInterval*>(interval.get())) {
         if (const auto *track = dynamic_cast<const AuTopsyVideoTrack*>(&entry.second->GetTrack()))
            for (const auto &clip : track->Timeline().clips)
               if (clip.id == media->VideoClipId() && clip.timingLocked) return true;
      }
#endif
   }
   return false;
}
void RequireUnlockedTrackMedia(const Track &track, double from, double to, TimingEditScope scope)
{
   const auto touches = [&](double start, double end) {
      if (scope == TimingEditScope::Following) return end > from + 1e-9;
      if (scope == TimingEditScope::OutsideSelection) return start < from - 1e-9 || end > to + 1e-9;
      return from == to ? start <= from && from < end
         : start < to - 1e-9 && end > from + 1e-9;
   };
      if (const auto *audio = dynamic_cast<const WaveTrack*>(&track))
         for (const auto &clip : audio->Intervals())
            if (touches(clip->GetPlayStartTime(), clip->GetPlayEndTime()) && ClipGroups::IsAudioClipTimingLocked(*clip))
               throw std::runtime_error("Unlock the audio clip in Clip Properties before changing its timing.");
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
      if (const auto *video = dynamic_cast<const AuTopsyVideoTrack*>(&track))
         for (const auto &clip : video->Timeline().clips)
            if (clip.timingLocked && touches(clip.start, clip.End()))
               throw std::runtime_error("Unlock the video clip in Clip Properties before changing its timing.");
#endif
}
void RequireUnlockedSelectedMedia(AudacityProject &project, TimingEditScope scope, bool syncLock)
{
   const auto &range = ViewInfo::Get(project).selectedRegion;
   for (const auto *track : TrackList::Get(project))
      if (syncLock ? SyncLock::IsSelectedOrSyncLockSelected(*track) : track->GetSelected())
         RequireUnlockedTrackMedia(*track, range.t0(), range.t1(), scope);
}

bool ApplyAudioClipProperties(AudacityProject &project, WaveTrack &track, WaveClip &clip, const ClipTimingProperties &value)
{
   RequireIdle(project, track); RequireClipTiming(value);
   const bool oldLocked = ClipGroups::IsAudioClipTimingLocked(clip);
   const auto oldStart = clip.GetPlayStartTime(), oldSpeed = 1 / clip.GetStretchRatio();
   RequireConsistentClipRetiming(clip.GetPlayEndTime() - oldStart, oldSpeed, value);
   const bool timing = std::abs(value.start - oldStart) > 1e-10 || std::abs(value.speed - oldSpeed) > 1e-12;
   if (oldLocked && value.locked && timing) throw std::runtime_error("Unlock the clip before changing its timing.");
   if (!timing && oldLocked == value.locked) return false;
   try {
      if (oldLocked && !value.locked) ClipGroups::SetAudioClipTimingLocked(clip, false);
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
      if (timing) ClipGroups::ChangeLinkedMediaClipTiming(project, track, &clip, 0, value.start, value.speed, false);
#else
      if (timing) {
         std::vector<ClipPlacementInterval> others;
         for (const auto &other : track.Intervals()) if (other.get() != &clip)
            others.push_back({other->GetPlayStartTime(), other->GetPlayEndTime()});
         RequireFreeClipPlacement(value, others);
         clip.StretchBy(oldSpeed / value.speed); clip.SetPlayStartTime(value.start);
      }
#endif
      ClipGroups::SetAudioClipTimingLocked(clip, value.locked);
      ProjectHistory::Get(project).PushState(XO("Changed audio clip properties"), XO("Clip Properties"));
   }
   catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
   Viewport::Get(project).Redraw(); return true;
}
void ShowAudioClipProperties(AudacityProject &project, WaveTrack &track, WaveClip &clip, wxWindow *parent)
{
   MediaDialogCommandGuard commands;
   RequireIdle(project, track);
   const auto trackId = track.GetId();
   const ClipTimingProperties value{clip.GetPlayStartTime(), clip.GetPlayEndTime(),
      1 / clip.GetStretchRatio(), ClipGroups::IsAudioClipTimingLocked(clip)};
   ShowClipPropertiesDialog(parent, value, [&](const ClipTimingProperties &requested) {
      // A failed native transaction can replace tracks and clips during
      // rollback. The modal guard excludes unrelated edits; reacquire the
      // original interval in the stable track identity for each Apply attempt.
      for (auto *live : TrackList::Get(project).Any<WaveTrack>()) if (live->GetId() == trackId)
         for (const auto &interval : live->Intervals())
            if (std::abs(interval->GetPlayStartTime() - value.start) < 1e-7 &&
                std::abs(interval->GetPlayEndTime() - value.end) < 1e-7) {
               ApplyAudioClipProperties(project, *live, *interval, requested); return;
            }
      throw std::runtime_error("The audio clip changed. Reopen its properties before editing it.");
   });
}
void CloseGapBeforeAudioClip(AudacityProject &project, WaveTrack &track, WaveClip &clip)
{
   std::vector<ClipPlacementInterval> others;
   for (const auto &other : track.Intervals()) if (other.get() != &clip)
      others.push_back({other->GetPlayStartTime(), other->GetPlayEndTime()});
   const auto start = FindGapBeforeClip(clip.GetPlayStartTime(), others);
   ApplyAudioClipProperties(project, track, clip, {start, start + clip.GetPlayEndTime() - clip.GetPlayStartTime(),
      1 / clip.GetStretchRatio(), ClipGroups::IsAudioClipTimingLocked(clip)});
}
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
bool ApplyVideoClipProperties(AudacityProject &project, AuTopsyVideoTrack &track, uint64_t id, const ClipTimingProperties &value)
{
   RequireIdle(project, track); RequireClipTiming(value);
   auto candidate = track.Timeline();
   const auto found = std::find_if(candidate.clips.begin(), candidate.clips.end(), [id](const auto &clip) { return clip.id == id; });
   if (found == candidate.clips.end()) throw std::runtime_error("The video clip was removed.");
   RequireConsistentClipRetiming(found->duration, found->playbackRate, value);
   const bool oldLocked = found->timingLocked;
   const bool timing = std::abs(value.start - found->start) > 1e-10 || std::abs(value.speed - found->playbackRate) > 1e-12;
   if (oldLocked && value.locked && timing) throw std::runtime_error("Unlock the clip before changing its timing.");
   if (!timing && oldLocked == value.locked) return false;
   try {
      if (oldLocked && !value.locked) { found->timingLocked = false; track.SetTimeline(std::move(candidate)); }
      if (timing) ClipGroups::ChangeLinkedMediaClipTiming(project, track, nullptr, id, value.start, value.speed, false);
      auto final = track.Timeline();
      for (auto &clip : final.clips) if (clip.id == id) clip.timingLocked = value.locked;
      track.SetTimeline(std::move(final));
      ProjectHistory::Get(project).PushState(XO("Changed video clip properties"), XO("Clip Properties"));
   }
   catch (...) { ProjectHistory::Get(project).RollbackState(); throw; }
   Viewport::Get(project).Redraw(); return true;
}
void ShowVideoClipProperties(AudacityProject &project, AuTopsyVideoTrack &track, uint64_t id, wxWindow *parent)
{
   MediaDialogCommandGuard commands;
   RequireIdle(project, track);
   const auto trackId = track.GetId();
   const auto &clips = track.Timeline().clips;
   const auto found = std::find_if(clips.begin(), clips.end(), [id](const auto &clip) { return clip.id == id; });
   if (found == clips.end()) throw std::runtime_error("The video clip was removed.");
   ShowClipPropertiesDialog(parent, {found->start, found->End(), found->playbackRate, found->timingLocked},
      [&](const ClipTimingProperties &value) {
         for (auto *live : VideoTimeline::ProjectVideoTracks(project)) if (live->GetId() == trackId) {
            ApplyVideoClipProperties(project, *live, id, value); return;
         }
         throw std::runtime_error("The video track was removed. Reopen clip properties before editing it.");
      });
}
void CloseGapBeforeVideoClip(AudacityProject &project, AuTopsyVideoTrack &track, uint64_t id)
{
   std::vector<ClipPlacementInterval> others;
   const VideoTimeline::Clip *target{};
   for (const auto &clip : track.Timeline().clips)
      if (clip.id == id) target = &clip; else others.push_back({clip.start, clip.End()});
   if (!target) throw std::runtime_error("The video clip was removed.");
   const auto start = FindGapBeforeClip(target->start, others);
   ApplyVideoClipProperties(project, track, id, {start, start + target->duration, target->playbackRate, target->timingLocked});
}
#endif
}

namespace {
struct TimingTarget {
   WaveTrack *audioTrack{}; WaveClip *audio{};
   AuTopsyVideoTrack *video{}; uint64_t id{};
   ClipActions::ClipTimingProperties value;
};
TimingTarget FindTimingTarget(AudacityProject &project, int trackIndex, double at)
{
   int index{};
   for (auto *track : TrackList::Get(project)) if (index++ == trackIndex) {
      if (auto *audio = dynamic_cast<WaveTrack*>(track))
         for (const auto &clip : audio->Intervals()) if (clip->GetPlayStartTime() <= at && at < clip->GetPlayEndTime())
            return {audio, clip.get(), nullptr, 0, {clip->GetPlayStartTime(), clip->GetPlayEndTime(),
               1 / clip->GetStretchRatio(), ClipGroups::IsAudioClipTimingLocked(*clip)}};
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
      if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track))
         if (const auto *clip = VideoTimeline::ClipAt(video->Timeline(), at))
            return {nullptr, nullptr, video, clip->id, {clip->start, clip->End(), clip->playbackRate, clip->timingLocked}};
#endif
      break;
   }
   throw std::runtime_error("Choose a clip using Track and a time At inside it.");
}
class ClipTimingCommand : public AudacityCommand {
   int mTrack{}, mLocked{-1};
   double mAt{}, mStart{-1}, mDuration{-1}, mEnd{-1}, mSpeed{-1};
protected:
   virtual bool Reading() const { return false; }
   virtual bool ClosingGap() const { return false; }
public:
   TranslatableString GetDescription() const override { return XO("Read or edit precise clip timing using the native linked-media engine."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mTrack, wxT("Track"), 0, 0, 1000000);
      visitor.Define(mAt, wxT("At"), 0.0, 0.0, 100000000.0);
      visitor.Define(mStart, wxT("Start"), -1.0, -1.0, 100000000.0);
      visitor.Define(mEnd, wxT("End"), -1.0, -1.0, 100000000.0);
      visitor.Define(mDuration, wxT("Duration"), -1.0, -1.0, 100000000.0);
      visitor.Define(mSpeed, wxT("Speed"), -1.0, -1.0, 100.0);
      visitor.Define(mLocked, wxT("Locked"), -1, -1, 1); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         auto target = FindTimingTarget(context.project, mTrack, mAt);
         if (Reading()) {
            rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
            out.StartObject(); out.Key("start"); out.Double(target.value.start);
            out.Key("end"); out.Double(target.value.end); out.Key("speed"); out.Double(target.value.speed);
            out.Key("locked"); out.Bool(target.value.locked); out.EndObject();
            context.Status(wxString::FromUTF8(buffer.GetString())); return true;
         }
         if (ClosingGap()) {
            if (target.audio) ClipActions::CloseGapBeforeAudioClip(context.project, *target.audioTrack, *target.audio);
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
            else ClipActions::CloseGapBeforeVideoClip(context.project, *target.video, target.id);
#endif
            return true;
         }
         auto value = target.value;
         const auto span = (value.end - value.start) * value.speed;
         if (mStart >= 0) value.start = mStart;
         if (mSpeed >= 0) value.speed = mSpeed;
         value.end = value.start + span / value.speed;
         if (mEnd >= 0 || mDuration >= 0) {
            const auto duration = mDuration >= 0 ? mDuration : mEnd - value.start;
            if (mDuration >= 0 && mEnd >= 0 && std::abs(value.start + mDuration - mEnd) > 1e-7)
               throw std::runtime_error("End and Duration disagree.");
            if (duration <= 0) throw std::runtime_error("Duration must be positive.");
            const auto rate = span / duration;
            if (mSpeed >= 0 && std::abs(rate - mSpeed) > 1e-7) throw std::runtime_error("Speed and Duration disagree.");
            value.end = value.start + duration; value.speed = rate;
         }
         if (mLocked >= 0) value.locked = mLocked != 0;
         if (target.audio) ClipActions::ApplyAudioClipProperties(context.project, *target.audioTrack, *target.audio, value);
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
         else ClipActions::ApplyVideoClipProperties(context.project, *target.video, target.id, value);
#endif
         return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
class SetClipPropertiesCommand final : public ClipTimingCommand {
public: static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
class GetClipPropertiesCommand final : public ClipTimingCommand {
   bool Reading() const override { return true; }
public: static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
class CloseClipGapCommand final : public ClipTimingCommand {
   bool ClosingGap() const override { return true; }
public: static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
};
const ComponentInterfaceSymbol SetClipPropertiesCommand::Symbol{XO("SetClipProperties")};
const ComponentInterfaceSymbol GetClipPropertiesCommand::Symbol{XO("GetClipProperties")};
const ComponentInterfaceSymbol CloseClipGapCommand::Symbol{XO("CloseClipGap")};
BuiltinCommandsModule::Registration<SetClipPropertiesCommand> sSetClipProperties;
BuiltinCommandsModule::Registration<GetClipPropertiesCommand> sGetClipProperties;
BuiltinCommandsModule::Registration<CloseClipGapCommand> sCloseClipGap;
}
