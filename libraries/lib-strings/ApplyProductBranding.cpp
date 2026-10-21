/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapter for the upstream fork-name substitution hook. Return values own their
 * storage: no dangling references, mutable translation catalog, or growing cache.
 */
#include "ApplyProductBranding.h"
#include "Internat.h"
#include <wx/translation.h>

wxString GetCustomSubstitution(const wxString &message)
{
   if (!message.Contains(wxT("Audacity")))
      return message;
   return wxString{AuTopsy::Branding::ApplyProductBranding(message.ToStdWstring())};
}

wxString GetCustomTranslation(const wxString &message)
{
   // Keep the original gettext key and translate before applying product policy.
   // printf arguments are inserted by callers afterwards, never rewritten here.
   return GetCustomSubstitution(wxGetTranslation(message));
}
