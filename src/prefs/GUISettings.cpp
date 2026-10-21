/**********************************************************************

Audacity: A Digital Audio Editor

@file GUISettings.cpp

Paul Licameli split from GUIPrefs.cpp

**********************************************************************/

#include "GUISettings.h"

#include "FileNames.h"
#include "Languages.h"
#include "AudacityMessageBox.h"

#include <wx/app.h>

wxString GUISettings::SetLang( const wxString & lang )
{
   auto result = Languages::SetLang(FileNames::AudacityPathList(), lang);
   if (!(lang.empty() || lang == L"System") && result != lang)
      ::AudacityMessageBox(
         XO("Language \"%s\" is unknown").Format( lang ) );

#ifdef EXPERIMENTAL_CEE_NUMBERS_OPTION
   bool forceCeeNumbers;
   gPrefs->Read(wxT("/Locale/CeeNumberFormat"), &forceCeeNumbers, false);
   if( forceCeeNumbers )
      Internat::SetCeeNumberFormat();
#endif

#if defined(__WXMSW__) && defined(AUTOPSY_HAS_VIDEO_PREVIEW)
   // libmpv requires the C numeric locale for its entire lifetime. SetLang
   // already captured the regional separator in Internat for displayed numbers;
   // keep the C runtime locale stable, including after a language change.
   wxSetlocale(LC_NUMERIC, "C");
#endif

#ifdef __WXMAC__
      wxApp::s_macHelpMenuTitleName = _("&Help");
#endif

   return result;
}
