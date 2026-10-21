#include "ChooseVideoExport.h"
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <algorithm>
namespace VideoExport {
std::optional<ExportSettings> ChooseVideoExport(wxWindow *parent, ExportSettings settings)
{
   wxDialog dialog{parent, wxID_ANY, _("Video Export Settings")};
   auto *layout = new wxBoxSizer{wxVERTICAL};
   layout->Add(new wxStaticText{&dialog, wxID_ANY,
      _("Zero dimensions or frame rate keep the source values.\nHardware presets require a supported NVIDIA GPU and driver.")},
      0, wxALL, 10);
   auto *rows = new wxFlexGridSizer{2, 8, 8};
   rows->AddGrowableCol(1);
   const auto row = [&](const wxString &label, wxWindow *control) {
      rows->Add(new wxStaticText{&dialog, wxID_ANY, label}, 0, wxALIGN_CENTER_VERTICAL);
      rows->Add(control, 1, wxEXPAND);
   };
   const std::vector<std::string> names{"auto","h264","hevc","av1","h264-nvenc","hevc-nvenc"};
   auto *preset = new wxChoice{&dialog, wxID_ANY};
   for (const auto *name : {"Automatic (copy unchanged picture; H.264 for edits)",
         "H.264 (widely compatible)", "HEVC / H.265 (smaller files)", "AV1 (software)",
         "H.264 NVIDIA hardware", "HEVC NVIDIA hardware"})
      preset->Append(wxGetTranslation(name));
   const auto found = std::find(names.begin(), names.end(), settings.preset);
   preset->SetSelection(found == names.end() ? 0 : int(found - names.begin()));
   row(_("Preset"), preset);
   auto *width = new wxSpinCtrl{&dialog, wxID_ANY};
   width->SetRange(0, 16384); width->SetValue(settings.width); row(_("Width"), width);
   auto *height = new wxSpinCtrl{&dialog, wxID_ANY};
   height->SetRange(0, 16384); height->SetValue(settings.height); row(_("Height"), height);
   auto *rate = new wxSpinCtrlDouble{&dialog, wxID_ANY};
   rate->SetRange(0, 1000); rate->SetDigits(6); rate->SetIncrement(1);
   rate->SetValue(settings.frameRate); row(_("Frames per second"), rate);
   auto *quality = new wxSpinCtrl{&dialog, wxID_ANY};
   quality->SetRange(0, 51); quality->SetValue(settings.quality);
   row(_("Quality (lower is better)"), quality);
   layout->Add(rows, 1, wxEXPAND | wxALL, 10);
   layout->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
   dialog.SetSizerAndFit(layout);
   while (dialog.ShowModal() == wxID_OK) {
      settings = {names.at(preset->GetSelection()), width->GetValue(), height->GetValue(),
         rate->GetValue(), quality->GetValue()};
      try { RequireUsableExportSettings(settings); return settings; }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Video Export Settings"),
            wxOK | wxICON_ERROR, &dialog);
      }
   }
   return {};
}
}
