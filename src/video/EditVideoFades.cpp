#include "EditVideoFades.h"
#include "PrepareVideoEffects.h"
#include "CreateVideoTrack.h"
#include "EvaluateVideoTimeline.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include "commands/AudacityCommand.h"
#include "commands/LoadCommands.h"
#include "CommandContext.h"
#include "SettingsVisitor.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectWindow.h"
#include "ProjectHistory.h"
#include "Viewport.h"
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

namespace VideoEditing {
namespace {
const VideoTimeline::Clip &FindClip(const AuTopsyVideoTrack &track, uint64_t id) {
   for (const auto &clip : track.Timeline().clips) if (clip.id == id) return clip;
   throw std::runtime_error("The video clip was removed.");
}
double CrossfadeLimit(AudacityProject &project, const AuTopsyVideoTrack &track,
   const VideoTimeline::Clip &clip) {
   const auto layers = VideoTimeline::GetProjectVisibleVideoLayers(project);
   const auto current = std::find(layers.begin(), layers.end(), &track.Timeline());
   if (current == layers.end()) throw std::runtime_error("Unmute this video track before crossfading.");
   std::vector<const VideoTimeline::Timeline*> lower(current + 1, layers.end());
   std::vector<double> cuts{clip.start, clip.End()};
   for (const auto *layer : lower) for (const auto &part : layer->clips)
      for (auto edge : VideoTimeline::VideoContentEdges(part))
         if (edge > clip.start && edge < clip.End()) cuts.push_back(edge);
   std::sort(cuts.begin(), cuts.end());
   double end = clip.start;
   for (size_t i = 1; i < cuts.size(); ++i) {
      if (!EvaluateVideoLayers(lower, (cuts[i - 1] + cuts[i]) / 2).clip) break;
      end = cuts[i];
   }
   if (end <= clip.start + 1e-7)
      throw std::runtime_error("Place this clip over an overlapping video on a lower track, then choose Crossfade. Clip positions and audio will stay unchanged.");
   return end - clip.start;
}
}
bool ChangeVideoClipFades(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, double fadeIn, double fadeOut)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before changing picture fades.");
   for (const auto value : {fadeIn, fadeOut}) if (!std::isfinite(value) || (value < 0 && value != -1))
      throw std::runtime_error("Picture fade durations must be finite and non-negative.");
   FindClip(track, clipId);
   const auto original = track.Timeline(); auto candidate = original;
   auto &clip = *std::find_if(candidate.clips.begin(), candidate.clips.end(),
      [clipId](const auto &value) { return value.id == clipId; });
   if (fadeIn > clip.duration + 1e-7 || fadeOut > clip.duration + 1e-7)
      throw std::runtime_error("A fade must fit inside the visible clip. Shorten the fade or untrim the clip.");
   if (fadeIn >= 0) { clip.opacity.inStart = fadeIn ? clip.sourceStart : 0; clip.opacity.inDuration = fadeIn * clip.playbackRate; }
   if (fadeOut >= 0) { clip.opacity.outEnd = fadeOut ? clip.ContentTimeAtProjectTime(clip.End()) : 0; clip.opacity.outDuration = fadeOut * clip.playbackRate; }
   if (fadeIn == 0 && fadeOut == 0) clip.opacityModifiers.clear();
   if (clip.opacity == FindClip(track, clipId).opacity && clip.opacityModifiers == FindClip(track, clipId).opacityModifiers) return false;
   VideoTimeline::RequireWellFormedTimeline(candidate);
   auto layers = VideoTimeline::GetProjectVisibleVideoLayers(project);
   for (auto &layer : layers) if (layer == &track.Timeline()) layer = &candidate;
   auto prepared = PrepareCandidateVideoEffects(project, layers, &ProjectWindow::Get(project));
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(XO("Changed picture fades"), XO("Picture Fades")); }
   catch (...) { track.SetTimeline(original); throw; }
   AdoptPreparedVideoEffects(project, std::move(prepared)); Viewport::Get(project).Redraw(); return true;
}

void ShowVideoClipFades(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent, bool crossfade)
{
   ClipActions::MediaDialogCommandGuard commands;
   const auto original = FindClip(track, clipId);
   const auto maximum = crossfade ? CrossfadeLimit(project, track, original) : original.duration;
   const auto initialIn = crossfade ? std::min(1.0, maximum) :
      std::clamp(original.opacity.inDuration / original.playbackRate, 0.0, maximum);
   const auto initialOut = std::clamp(original.opacity.outDuration / original.playbackRate, 0.0, maximum);
   wxDialog dialog(parent, wxID_ANY, crossfade ? _("Crossfade from Video Below") : _("Fade Picture"));
   auto *layout = new wxBoxSizer(wxVERTICAL);
   auto *explanation = new wxStaticText(&dialog, wxID_ANY, crossfade
      ? _("The lower video shows through while this clip fades in. Clip positions and audio stay unchanged.")
      : _("Picture fades reveal a lower video, or black. Audio stays unchanged. Zero removes a fade."));
   explanation->Wrap(420); layout->Add(explanation, 0, wxALL, 10);
   auto *fields = new wxFlexGridSizer(2, 8, 10);
   const auto add = [&](const wxString &name, double value) {
      fields->Add(new wxStaticText(&dialog, wxID_ANY, name), 0, wxALIGN_CENTER_VERTICAL);
      auto *control = new wxSpinCtrlDouble(&dialog, wxID_ANY, {}, wxDefaultPosition, wxSize(160, -1),
         wxSP_ARROW_KEYS, 0, maximum, value, .1);
      control->SetDigits(4); control->SetName(name); fields->Add(control); return control;
   };
   auto *in = add(_("Fade in (seconds)"), initialIn);
   auto *out = crossfade ? nullptr : add(_("Fade out (seconds)"), initialOut);
   layout->Add(fields, 0, wxALL, 10);
   auto *reset = new wxButton(&dialog, wxID_ANY, _("Remove Fades"));
   bool removeAll{};
   layout->Add(reset, 0, wxLEFT | wxBOTTOM, 10);
   reset->Bind(wxEVT_BUTTON, [=, &removeAll](wxCommandEvent &) { removeAll = true; in->SetValue(0); if (out) out->SetValue(0); });
   layout->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, 10);
   dialog.SetSizerAndFit(layout);
   dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
      try {
         // Untouched controls preserve retained anchors after split/trim.
         const auto incoming = removeAll || crossfade || in->GetValue() != initialIn ? in->GetValue() : -1;
         const auto outgoing = removeAll || (out && out->GetValue() != initialOut) ? (out ? out->GetValue() : 0) : -1;
         ChangeVideoClipFades(project, track, clipId, incoming, outgoing); dialog.EndModal(wxID_OK);
      }
      catch (const VideoPreview::PreviewPreparationCancelled &) {}
      catch (const std::exception &error) { wxMessageBox(wxString::FromUTF8(error.what()), _("Picture Fades"), wxOK | wxICON_ERROR, &dialog); }
   }, wxID_OK);
   dialog.ShowModal();
}
}

namespace {
class SetVideoClipFadesCommand final : public AudacityCommand {
   int mTrack{-1}; wxString mId; double mIn{-1}, mOut{-1};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Set picture fade seconds without changing clip timing or audio."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &visitor) {
      visitor.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      visitor.Define(mId, wxT("Id"), wxString{});
      visitor.Define(mIn, wxT("In"), -1.0, -1.0, 100000000.0);
      visitor.Define(mOut, wxT("Out"), -1.0, -1.0, 100000000.0); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         unsigned long long id{}; if (!mId.ToULongLong(&id) || !id) throw std::runtime_error("Supply a video clip Id and Track.");
         int index{}; AuTopsyVideoTrack *target{};
         for (auto *track : TrackList::Get(context.project)) {
            if ((mTrack < 0 || index == mTrack)) if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track))
               for (const auto &clip : video->Timeline().clips) if (clip.id == id) {
                  if (target) throw std::runtime_error("Clip Id exists on several tracks. Supply Track too."); target = video;
               }
            ++index;
         }
         if (!target) throw std::runtime_error("The video clip was removed.");
         VideoEditing::ChangeVideoClipFades(context.project, *target, id, mIn, mOut); return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetVideoClipFadesCommand::Symbol{XO("SetVideoClipFades")};
BuiltinCommandsModule::Registration<SetVideoClipFadesCommand> sSetVideoClipFades;
}
