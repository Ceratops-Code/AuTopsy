/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Check owned message policy independently, then the host's translation adapter.
 * Core-only builds need only Catch2 and the standard library; no GUI is opened.
 */
#include "../ApplyProductBranding.h"
#include <catch2/catch.hpp>
#include <array>

TEST_CASE("Product words inherit AuTopsy branding", "[branding-core]")
{
   using AuTopsy::Branding::ApplyProductBranding;
   const std::array<std::pair<std::wstring, std::wstring>, 7> messages{{
      {L"Audacity", L"AuTopsy"},
      {L"About &Audacity...", L"About &AuTopsy..."},
      {L"Audacity: ready. Restart Audacity.", L"AuTopsy: ready. Restart AuTopsy."},
      {L"Audacity's settings", L"AuTopsy's settings"},
      {L"\u00d6ffnen mit Audacity", L"\u00d6ffnen mit AuTopsy"},
      {L"Audacity\u3067\u958b\u304f", L"AuTopsy\u3067\u958b\u304f"},
      {L"", L""}
   }};
   for (const auto &[message, expected] : messages) {
      CHECK(ApplyProductBranding(message) == expected);
      CHECK(ApplyProductBranding(expected) == expected);
      CHECK(ApplyProductBranding(message) == expected);
   }
}

TEST_CASE("Technical references keep their exact identity", "[branding-core]")
{
   using AuTopsy::Branding::ApplyProductBranding;
   for (const auto &message : {
      L"AudacityProject", L"myAudacity", L"Audacity3", L"Audacity::Project",
      L"Audacity.exe", L"Audacity.aup3", L"Audacity.cfg", L"Audacity.app",
      L"C:\\Program Files\\Audacity\\Audacity.exe", L"/usr/share/Audacity",
      L"https://example.org/Audacity/issues", L"Audacity@example.org",
      L"<img src=\"Audacity.png\" class=\"Audacity\">"})
      CHECK(ApplyProductBranding(message) == message);
   CHECK(ApplyProductBranding(L"<a href=\"https://example.org/Audacity\">Audacity</a>") ==
      L"<a href=\"https://example.org/Audacity\">AuTopsy</a>");
   CHECK(ApplyProductBranding(L"Audacity can open 'Audacity.aup3'.") ==
      L"AuTopsy can open 'Audacity.aup3'.");
}

#ifndef AUTOPSY_BRANDING_CORE_ONLY
#include "Internat.h"
#include "Identifier.h"
#include <wx/translation.h>
#include <wx/arrstr.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace {
// A real, in-memory gettext catalog exercises context, plural selection, and
// lookup order. The test owns the process's translation object and no files.
class BrandingCatalogLoader final : public wxTranslationsLoader {
public:
   wxArrayString GetAvailableTranslations(const wxString &) const override
   {
      wxArrayString languages;
      languages.Add(wxT("de"));
      return languages;
   }
   wxMsgCatalog *LoadCatalog(const wxString &domain, const wxString &) override
   {
      const std::map<std::string, std::string> messages{
         {"", "Content-Type: text/plain; charset=UTF-8\nLanguage: de\nPlural-Forms: nplurals=2; plural=(n != 1);\n"},
         {"Audacity ready", "Audacity bereit"},
         {"dialog\004Audacity settings", "Audacity Einstellungen"},
         {std::string{"Audacity selected %d clip"} + '\0' + "Audacity selected %d clips",
          std::string{"Audacity: %d Clip"} + '\0' + "Audacity: %d Clips"}
      };
      const auto count = static_cast<uint32_t>(messages.size());
      std::vector<uint32_t> words{0x950412de, 0, count, 28, 28+count*8, 0, 0};
      words.resize(7+count*4);
      std::string bytes(words.size()*4, '\0');
      size_t index = 0;
      for (const auto &[key, translation] : messages) {
         words[7+index*2] = static_cast<uint32_t>(key.size());
         words[8+index*2] = static_cast<uint32_t>(bytes.size());
         bytes.append(key).push_back('\0');
         words[7+count*2+index*2] = static_cast<uint32_t>(translation.size());
         words[8+count*2+index*2] = static_cast<uint32_t>(bytes.size());
         bytes.append(translation).push_back('\0');
         ++index;
      }
      // GNU MO header/table words are explicitly little endian, independent of
      // the host machine used by contributors or CI.
      for (size_t word = 0; word < words.size(); ++word)
         for (unsigned byte = 0; byte < 4; ++byte)
            bytes[word*4+byte] = static_cast<char>((words[word] >> (byte*8)) & 0xff);
      wxCharBuffer owned(bytes.size());
      std::memcpy(owned.data(), bytes.data(), bytes.size());
      return wxMsgCatalog::CreateFromData(owned, domain);
   }
};

struct TranslationCatalogScope {
   TranslationCatalogScope()
   {
      REQUIRE(wxTranslations::Get() == nullptr);
      auto translations = std::make_unique<wxTranslations>();
      translations->SetLoader(new BrandingCatalogLoader);
      translations->SetLanguage(wxT("de"));
      REQUIRE(translations->AddCatalog(wxT("branding-test")));
      wxTranslations::Set(translations.release());
   }
   ~TranslationCatalogScope() { wxTranslations::Set(nullptr); }
};
}

TEST_CASE("Translation adapters brand templates before arguments", "[branding-adapter]")
{
   TranslationCatalogScope scope;
   for (int repeat = 0; repeat < 2; ++repeat) {
      CHECK(GetCustomTranslation(wxT("Audacity ready")) == wxT("AuTopsy bereit"));
      CHECK(GetCustomSubstitution(wxT("Audacity: ready")) == wxT("AuTopsy: ready"));
      CHECK(XO("Audacity ready").Translation() == wxT("AuTopsy bereit"));
      CHECK(XC("Audacity settings", "dialog").Translation() == wxT("AuTopsy Einstellungen"));
      const auto message = XO("Audacity cannot open %s")
         .Format(wxString{wxT("C:\\Audacity\\Audacity.wav")});
      CHECK(message.Translation() == wxT("AuTopsy cannot open C:\\Audacity\\Audacity.wav"));
      CHECK(message.Debug() == wxT("Audacity cannot open C:\\Audacity\\Audacity.wav"));
      CHECK(message.MSGID() == Identifier{wxT("Audacity cannot open %s")});
      CHECK(XO("Audacity can open %s").Format(Verbatim(wxT("Audacity.aup3"))).Translation()
         == wxT("AuTopsy can open Audacity.aup3"));
      CHECK(XO("Audacity").Join(Verbatim(wxT("Audacity")), wxT(" / ")).Translation()
         == wxT("AuTopsy / Audacity"));
      CHECK(XO("&Audacity...").StrippedTranslation() == wxT("AuTopsy..."));
      CHECK(XO("&Audacity...").Stripped(TranslatableString::MenuCodes | TranslatableString::Ellipses)
         .Translation() == wxT("AuTopsy"));
      CHECK(Verbatim(wxT("Copyright Audacity Team")).Translation() == wxT("Copyright Audacity Team"));
      CHECK(XO("Based on %s").Format(Verbatim(wxT("Audacity"))).Translation() == wxT("Based on Audacity"));
      CHECK(XP("Audacity selected %d clip", "Audacity selected %d clips", 0)(1).Translation()
         == wxT("AuTopsy: 1 Clip"));
      CHECK(XP("Audacity selected %d clip", "Audacity selected %d clips", 0)(2).Translation()
         == wxT("AuTopsy: 2 Clips"));
      CHECK(XP("Audacity selected %d clip", "Audacity selected %d clips", 0)(2).Debug()
         == wxT("Audacity selected 2 clips"));
   }
}
#endif
