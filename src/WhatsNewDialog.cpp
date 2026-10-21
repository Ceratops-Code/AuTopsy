/*  SPDX-License-Identifier: GPL-2.0-or-later */
/*!********************************************************************

  Audacity: A Digital Audio Editor

  WhatsNewDialog.cpp

  Vitaly Sverchinsky

**********************************************************************/

#include "WhatsNewDialog.h"

#include <wx/checkbox.h>
#include <wx/statbmp.h>
#include "HelpSystem.h"
#include "HyperLink.h"
#include "Prefs.h"
#include "ProjectWindows.h"
#include "ShuttleGui.h"
#include "../images/Audacity-splash.xpm"

namespace
{
AttachedWindows::RegisteredFactory sWhatsNewWindow{
   [](AudacityProject &project) -> wxWeakRef<wxWindow> {
      return safenew WhatsNewDialog(&GetProjectFrame(project), wxID_ANY);
   }
};
}

BEGIN_EVENT_TABLE(WhatsNewDialog, wxDialogWrapper)
   EVT_BUTTON(wxID_OK, WhatsNewDialog::OnOK)
END_EVENT_TABLE()

WhatsNewDialog::WhatsNewDialog(wxWindow* parent, wxWindowID id)
   : wxDialogWrapper(parent, id, XO("Welcome to AuTopsy!"))
{
   SetName();
   ShuttleGui S(this, eIsCreating);
   Populate(S);
   Fit();
   Centre();
}

WhatsNewDialog::~WhatsNewDialog() = default;

void WhatsNewDialog::Show(AudacityProject& project)
{
   auto dialog = &GetAttachedWindows(project).Get<WhatsNewDialog>(sWhatsNewWindow);
   dialog->CenterOnParent();
   dialog->wxDialogWrapper::Show();
}

void WhatsNewDialog::Populate(ShuttleGui& S)
{
   bool showSplashScreen;
   gPrefs->Read("/GUI/ShowSplashScreen", &showSplashScreen, true);
   S.SetBorder(12);
   S.StartVerticalLay(wxALIGN_CENTER_HORIZONTAL);
   {
      // Reuse the product's loading artwork; upstream promotions do not belong
      // to this fork's welcome screen.
      auto banner = safenew wxStaticBitmap(
         S.GetParent(), wxID_ANY, wxBitmap(Audacity_splash_xpm));
      banner->SetName(XO("AuTopsy: audio and video in sync").Translation());
      S.AddWindow(banner);
      S.AddFixedText(XO("Picture and sound. One timeline."));
      S.AddFixedText(XO(
         "Import Video adds picture and a separate editable soundtrack.\n"
         "Split, trim and move video clips alongside your audio tracks.\n"
         "Export the video with your finished audio mix."), false, 620);
      S.AddSpace(10);
      S.StartHorizontalLay(wxALIGN_CENTER_HORIZONTAL);
      {
         S.AddWindow(safenew HyperLink(S.GetParent(), wxID_ANY,
            XO("AuTopsy guide").Translation(),
            "https://github.com/Ceratops-Code/AuTopsy#readme"));
         S.AddSpace(24);
         S.AddWindow(safenew HyperLink(S.GetParent(), wxID_ANY,
            XO("Help and feedback").Translation(),
            "https://github.com/Ceratops-Code/AuTopsy/issues"));
      }
      S.EndHorizontalLay();
      S.AddSpace(10);
      mDontShowAgain = S.AddCheckBox(
         XXO("&Don't show this again at start up"), !showSplashScreen);
      S.Id(wxID_OK).AddButton(XXO("Start editing"), wxALL, true);
   }
   S.EndVerticalLay();
}

void WhatsNewDialog::OnOK(wxCommandEvent&)
{
   gPrefs->Write("/GUI/ShowSplashScreen", !mDontShowAgain->IsChecked());
   gPrefs->Flush();
   wxDialogWrapper::Show(false);
}
