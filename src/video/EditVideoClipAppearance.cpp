#include "EditVideoClipAppearance.h"
#include "CreateVideoTrack.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include "ProjectAudioIO.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "Viewport.h"
#include "CommandContext.h"
#include "SettingsVisitor.h"
#include "commands/AudacityCommand.h"
#include "commands/LoadCommands.h"
#include <wx/checkbox.h>
#include <wx/clrpicker.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <limits>

namespace VideoEditing {
bool ChangeVideoClipAppearance(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t id, const VideoClipAppearance &appearance)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback before changing clip appearance.");
   if (track.GetOwner().get() != &TrackList::Get(project))
      throw std::runtime_error("The video track was removed.");
   const auto &clips = track.Timeline().clips;
   const auto found = std::find_if(clips.begin(), clips.end(), [id](const auto &clip) { return clip.id == id; });
   if (found == clips.end()) throw std::runtime_error("The video clip was removed.");
   const auto previous = found->appearance;
   if (!track.SetClipAppearance(id, appearance)) return false;
   try { ProjectHistory::Get(project).PushState(XO("Changed video clip name and colour"), XO("Clip Appearance")); }
   catch (...) { track.SetClipAppearance(id, previous); throw; }
   Viewport::Get(project).Redraw(); return true;
}
void ShowVideoClipAppearanceDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t id, wxWindow *parent)
{
   ClipActions::MediaDialogCommandGuard commands;
   const auto found = std::find_if(track.Timeline().clips.begin(), track.Timeline().clips.end(),
      [id](const auto &clip) { return clip.id == id; });
   if (found == track.Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
   const auto current = found->appearance;
   wxDialog dialog{parent, wxID_ANY, _("Video Clip Name and Colour")};
   auto *layout = new wxBoxSizer{wxVERTICAL};
   layout->Add(new wxStaticText{&dialog, wxID_ANY, _("Clip name (empty uses the source name):")}, 0, wxALL, 10);
   auto *name = new wxTextCtrl{&dialog, wxID_ANY, wxString::FromUTF8(current.name), wxDefaultPosition, {360, -1}};
   name->SetName(_("Video clip name")); layout->Add(name, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
   auto *automatic = new wxCheckBox{&dialog, wxID_ANY, _("Use default clip colour")};
   automatic->SetValue(current.rgb < 0); layout->Add(automatic, 0, wxALL, 10);
   const auto rgb = current.rgb < 0 ? 0x39698f : current.rgb;
   auto *colour = new wxColourPickerCtrl{&dialog, wxID_ANY, wxColour{(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255}};
   colour->SetName(_("Video clip colour")); colour->Enable(!automatic->GetValue());
   automatic->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent &) { colour->Enable(!automatic->GetValue()); });
   layout->Add(colour, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
   layout->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
   dialog.SetSizerAndFit(layout); name->SetFocus(); name->SelectAll();
   while (dialog.ShowModal() == wxID_OK) {
      const auto selected = colour->GetColour();
      const auto utf8 = name->GetValue().ToUTF8();
      VideoClipAppearance value{std::string(utf8.data(), utf8.length()), automatic->GetValue() ? -1 :
         (int(selected.Red()) << 16) | (int(selected.Green()) << 8) | int(selected.Blue())};
      try { ChangeVideoClipAppearance(project, track, id, value); return; }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Clip Appearance"), wxOK | wxICON_ERROR, &dialog);
      }
   }
}
}

namespace {
class SetVideoClipAppearanceCommand final : public AudacityCommand {
   wxString mId, mName;
   int mTrack{-1}, mColour{-1};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Set a video clip's display name and RGB colour; -1 uses the default."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) {
      v.Define(mId, wxT("Id"), wxString{}); v.Define(mName, wxT("Name"), wxString{});
      v.Define(mTrack, wxT("Track"), -1, -1, 1000000);
      v.Define(mColour, wxT("Colour"), -1, std::numeric_limits<int>::lowest(), std::numeric_limits<int>::max()); return true;
   }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      try {
         unsigned long long id{};
         if (!mId.ToULongLong(&id) || !id) throw std::runtime_error("Supply a video clip Id from GetVideoClips.");
         AuTopsyVideoTrack *target{}; int index{};
         for (auto *track : TrackList::Get(context.project)) {
            if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track); video && (mTrack < 0 || mTrack == index))
               if (std::any_of(video->Timeline().clips.begin(), video->Timeline().clips.end(),
                     [id](const auto &clip) { return clip.id == id; })) {
                  if (target) throw std::runtime_error("The clip Id is ambiguous. Supply its Track.");
                  target = video;
               }
            ++index;
         }
         if (!target) throw std::runtime_error("The video clip was removed.");
         const auto utf8 = mName.ToUTF8();
         VideoEditing::ChangeVideoClipAppearance(context.project, *target, id,
            {std::string(utf8.data(), utf8.length()), mColour}); return true;
      }
      catch (const std::exception &error) { context.Error(wxString::FromUTF8(error.what())); return false; }
   }
};
const ComponentInterfaceSymbol SetVideoClipAppearanceCommand::Symbol{XO("SetVideoClipAppearance")};
BuiltinCommandsModule::Registration<SetVideoClipAppearanceCommand> sSetVideoClipAppearance;
}
