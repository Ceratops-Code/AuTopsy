#include "EditVideoComposition.h"
#include "CreateVideoTrack.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include <wx/button.h>
#include "ProjectHistory.h"
#include "Viewport.h"
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <algorithm>
#include <stdexcept>

namespace VideoEditing {
bool ChangeProjectVideoClipMute(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, bool muted)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before changing clip picture mute.");
   auto candidate = track.Timeline();
   if (!VideoTimeline::ChangeVideoClipMute(candidate, clipId, muted)) return false;
   const auto previous = track.Timeline();
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(
      muted ? XO("Muted video clip picture") : XO("Unmuted video clip picture"), XO("Video Clip Mute")); }
   catch (...) { track.SetTimeline(previous); throw; }
   Viewport::Get(project).HandleResize();
   return true;
}

bool ChangeProjectVideoClipCrop(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, const VideoFrameCrop &crop)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing picture crop.");
   auto candidate = track.Timeline();
   if (!VideoTimeline::ChangeVideoClipCrop(candidate, clipId, crop)) return false;
   const auto previous = track.Timeline();
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(XO("Cropped video picture"), XO("Video Crop")); }
   catch (...) { track.SetTimeline(previous); throw; }
   Viewport::Get(project).HandleResize();
   return true;
}

void ShowVideoClipCropDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent)
{
   const auto found = std::find_if(track.Timeline().clips.begin(), track.Timeline().clips.end(),
      [clipId](const auto &clip) { return clip.id == clipId; });
   if (found == track.Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
   auto crop = found->crop;
   wxDialog dialog{parent, wxID_ANY, _("Crop Video Picture")};
   auto *layout = new wxBoxSizer(wxVERTICAL);
   layout->Add(new wxStaticText(&dialog, wxID_ANY,
      _("Remove picture margins (%). The remaining picture keeps its proportions.")),
      0, wxALL, 10);
   auto *grid = new wxFlexGridSizer(2, 8, 12);
   const wxString labels[]{_("Left"), _("Top"), _("Right"), _("Bottom")};
   const double initial[]{crop.left, crop.top, crop.right, crop.bottom};
   wxSpinCtrlDouble *fields[4]{};
   double shown[4]{};
   for (size_t i{}; i < 4; ++i) {
      grid->Add(new wxStaticText(&dialog, wxID_ANY, labels[i]), 0, wxALIGN_CENTER_VERTICAL);
      fields[i] = new wxSpinCtrlDouble(&dialog, wxID_ANY, {}, wxDefaultPosition,
         wxDefaultSize, wxSP_ARROW_KEYS, 0, 99.99, initial[i] * 100, 0.1);
      fields[i]->SetDigits(2);
      shown[i] = fields[i]->GetValue();
      fields[i]->SetName(labels[i]);
      grid->Add(fields[i], 0, wxEXPAND);
   }
   layout->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
   auto *reset = new wxButton(&dialog, wxID_ANY, _("Reset Crop"));
   reset->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
      crop = {};
      for (size_t i{}; i < 4; ++i) { fields[i]->SetValue(0); shown[i] = 0; }
   });
   layout->Add(reset, 0, wxLEFT | wxRIGHT, 10);
   layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
   dialog.SetSizerAndFit(layout);
   VideoFrameCrop selected;
   dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
      // Untouched controls retain saved precision beyond the displayed digits.
      selected = crop;
      double *values[]{&selected.left, &selected.top, &selected.right, &selected.bottom};
      for (size_t i{}; i < 4; ++i)
         if (fields[i]->GetValue() != shown[i]) *values[i] = fields[i]->GetValue() / 100;
      try { RequireUsableVideoFrameCrop(selected); dialog.EndModal(wxID_OK); }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Picture Crop"), wxOK | wxICON_ERROR, &dialog);
      }
   }, wxID_OK);
   if (dialog.ShowModal() == wxID_OK) ChangeProjectVideoClipCrop(project, track, clipId, selected);
}
}
