/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Owned product-name policy. This header has no editor or wxWidgets dependency;
 * adapters apply it to translated message templates before inserting arguments.
 */
#pragma once

#include <string>
#include <string_view>

namespace AuTopsy::Branding {
namespace Detail {
inline bool IsIdentifierCharacter(wchar_t character)
{
   return (character >= L'a' && character <= L'z') ||
      (character >= L'A' && character <= L'Z') ||
      (character >= L'0' && character <= L'9') || character == L'_';
}

inline bool IsTokenDelimiter(wchar_t character)
{
   return character == L' ' || character == L'\t' || character == L'\n' ||
      character == L'\r' || character == L'"' || character == L'\'' ||
      character == L'<' || character == L'>';
}

inline bool IsTechnicalReference(std::wstring_view text, size_t position, size_t length)
{
   const auto openTag = text.rfind(L'<', position);
   const auto closeTag = text.rfind(L'>', position);
   if (openTag != text.npos && (closeTag == text.npos || openTag > closeTag))
      return true;

   auto start = position;
   auto end = position + length;
   while (start && !IsTokenDelimiter(text[start-1])) --start;
   while (end < text.size() && !IsTokenDelimiter(text[end])) ++end;
   const auto token = text.substr(start, end-start);
   if (token.find_first_of(L"/\\@_#") != token.npos || token.find(L"::") != token.npos)
      return true;
   // A sentence-ending period is punctuation; a file extension or domain is
   // part of a technical name. Keep both its spelling and any link target.
   for (size_t index = 0; index+1 < token.size(); ++index)
      if (token[index] == L'.' && IsIdentifierCharacter(token[index+1]))
         return true;
   return false;
}
}

// Change only standalone product words. Identifiers, paths, URLs, and markup
// attributes retain their spelling. Verbatim text and user-supplied arguments
// bypass this operation in the translation adapter, preserving attribution and
// filenames. The pure transformation is repeatable and keeps no global state.
inline std::wstring ApplyProductBranding(std::wstring_view message)
{
   constexpr std::wstring_view upstreamName{L"Audacity"};
   constexpr std::wstring_view productName{L"AuTopsy"};
   std::wstring result;
   result.reserve(message.size());
   size_t offset = 0;
   while (true) {
      const auto position = message.find(upstreamName, offset);
      if (position == message.npos) {
         result.append(message.substr(offset));
         return result;
      }
      result.append(message.substr(offset, position-offset));
      const auto end = position + upstreamName.size();
      const bool wholeWord =
         (!position || !Detail::IsIdentifierCharacter(message[position-1])) &&
         (end == message.size() || !Detail::IsIdentifierCharacter(message[end]));
      result.append(wholeWord && !Detail::IsTechnicalReference(message, position, upstreamName.size())
         ? productName : upstreamName);
      offset = end;
   }
}
}
