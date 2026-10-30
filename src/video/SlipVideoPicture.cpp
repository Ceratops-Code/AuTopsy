#include "SlipVideoPicture.h"
#include "CreateVideoTrack.h"
#include "PrepareVideoEffects.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include "CommandContext.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectHistory.h"
#include "ProjectWindow.h"
#include "SettingsVisitor.h"
#include "Viewport.h"
#include "commands/AudacityCommand.h"
#include "commands/LoadCommands.h"
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace VideoEditing {
bool ChangeVideoPictureSource(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, double sourceStart)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before slipping the picture.");
   const auto original = track.Timeline(); auto candidate = original;
   if (!VideoTimeline::SlipVideoClip(candidate, clipId, sourceStart)) return false;
   // Build cancellable derived pictures before publishing either state or history.
   auto prepared = PrepareCandidateVideoEffects(project, track, candidate, &ProjectWindow::Get(project));
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(XO("Slipped video picture; audio unchanged"), XO("Slip Picture")); }
   catch (...) { track.SetTimeline(original); throw; }
   AdoptPreparedVideoEffects(project, std::move(prepared));
   Viewport::Get(project).Redraw(); return true;
}
void ShowVideoPictureSlip(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent)
{
   ClipActions::MediaDialogCommandGuard commands;
   const auto &timeline = track.Timeline();
   const auto found = std::find_if(timeline.clips.begin(), timeline.clips.end(),
      [clipId](const auto &clip) { return clip.id == clipId; });
   if (found == timeline.clips.end()) throw std::runtime_error("The video clip was removed.");
   if (found->timingLocked) throw std::runtime_error("Unlock the clip before slipping its picture.");
   const auto source = VideoTimeline::SourceFor(timeline, *found);
   const auto fps = VideoTimeline::VideoContentFrameRate(*found, source);
   const auto maximum = std::max(0.0, VideoTimeline::VideoContentDuration(*found, source) - found->duration * found->playbackRate);
   wxDialog dialog(parent, wxID_ANY, _("Slip Picture"));
   auto *layout = new wxBoxSizer(wxVERTICAL);
   auto *explanation = new wxStaticText(&dialog, wxID_ANY,
      _("Choose where the picture starts in the retained footage. Clip position, length and audio stay fixed. Picture flags and fades follow the footage. Trim first to make room."));
   explanation->Wrap(430); layout->Add(explanation, 0, wxALL, 10);
   layout->Add(new wxStaticText(&dialog, wxID_ANY,
      wxString::Format(_("Picture source start (seconds): 0 to %.6f"), maximum)), 0, wxLEFT | wxRIGHT, 10);
   // Read the actual text on OK. A spin control can retain its previous cached
   // number until focus leaves the editor, including keyboard/default-button OK.
   auto *start = new wxTextCtrl(&dialog, wxID_ANY, wxString::Format("%.6f", found->sourceStart),
      wxDefaultPosition, wxSize(220, -1));
   start->SetName(_("Picture source start"));
   auto *fields = new wxBoxSizer(wxHORIZONTAL); fields->Add(start, 1, wxRIGHT, 8);
   for (const int direction : {-1, 1}) {
      auto *step = new wxButton(&dialog, wxID_ANY, direction < 0 ? _("-1 frame") : _("+1 frame"));
      fields->Add(step, 0, wxRIGHT, 4);
      step->Bind(wxEVT_BUTTON, [=](wxCommandEvent &) {
         double value{};
         if (start->GetValue().ToDouble(&value) && std::isfinite(value))
            start->SetValue(wxString::Format("%.6f", std::clamp(std::round(value * fps + direction) / fps, 0.0, maximum)));
      });
   }
   layout->Add(fields, 0, wxEXPAND | wxALL, 10);
   layout->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, 10);
   dialog.SetSizerAndFit(layout);
   dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
      try {
         double value{};
         if (!start->GetValue().ToDouble(&value) || !std::isfinite(value))
            throw std::runtime_error("Enter a finite picture source start in seconds.");
         ChangeVideoPictureSource(project, track, clipId, value); dialog.EndModal(wxID_OK);
      }
      catch (const VideoPreview::PreviewPreparationCancelled &) {}
      catch (const std::exception &error) { wxMessageBox(wxString::FromUTF8(error.what()), _("Slip Picture"), wxOK | wxICON_ERROR, &dialog); }
   }, wxID_OK);
   dialog.ShowModal();
}
}
namespace {
class SetVideoClipSlipCommand final : public AudacityCommand {
   int mTrack{-1}; wxString mId; double mSourceStart{};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Slip retained pictures without moving the clip or its soundtrack."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      v.Define(mId, wxT("Id"), wxString{});
      v.Define(mSourceStart, wxT("SourceStart"), 0.0, 0.0, 100000000.0); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id) throw std::runtime_error("Supply a video clip Id and Track.");
         int index{}; AuTopsyVideoTrack *target{};
         for (auto *track : TrackList::Get(context.project)) {
            if (mTrack < 0 || mTrack == index) if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track))
               for (const auto &clip : video->Timeline().clips) if (clip.id == id) {
                  if (target) throw std::runtime_error("Clip Id exists on several tracks. Supply Track too.");
                  target = video;
               }
            ++index;
         }
         if (!target) throw std::runtime_error("The video clip was removed.");
         VideoEditing::ChangeVideoPictureSource(context.project, *target, id, mSourceStart); return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetVideoClipSlipCommand::Symbol{XO("SetVideoClipSlip")};
BuiltinCommandsModule::Registration<SetVideoClipSlipCommand> sSetVideoClipSlip;
}
