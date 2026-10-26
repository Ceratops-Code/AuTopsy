// Owned release policy; no editor, network, filesystem or GUI dependency.
#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace AuTopsy::Updates {
inline constexpr std::string_view GitHubRepository{"Ceratops-Code/AuTopsy"};
inline std::string ProductReleasesPage() { return "https://github.com/" + std::string(GitHubRepository) + "/releases"; }
struct ReleaseVersion {
   std::array<unsigned, 3> core{};
   std::optional<unsigned> preview;
   bool operator<(const ReleaseVersion &other) const {
      if (core != other.core) return core < other.core;
      if (preview.has_value() != other.preview.has_value()) return preview.has_value();
      return preview.value_or(0) < other.preview.value_or(0);
   }
};
inline std::optional<ReleaseVersion> ParseReleaseVersion(std::string_view text) {
   if (!text.empty() && text.front() == 'v') text.remove_prefix(1);
   if (text.empty() || text.size() > 64) return {};
   ReleaseVersion result;
   const auto number = [&](unsigned &value) {
      const auto begin = text.data(); const auto parsed = std::from_chars(begin, begin + text.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr == begin || (parsed.ptr - begin > 1 && *begin == '0')) return false;
      text.remove_prefix(static_cast<size_t>(parsed.ptr - begin)); return true;
   };
   for (size_t i{}; i < result.core.size(); ++i) {
      if (!number(result.core[i])) return {};
      if (i < 2) { if (text.empty() || text.front() != '.') return {}; text.remove_prefix(1); }
   }
   if (text.empty()) return result;
   constexpr std::string_view suffix{"-autopsy."};
   if (text.substr(0, suffix.size()) != suffix) return {};
   text.remove_prefix(suffix.size()); unsigned preview{};
   if (!number(preview) || !text.empty() || !preview) return {};
   result.preview = preview; return result;
}
struct ReleaseOffer {
   std::string tag, notes, page, download;
   bool draft{}, prerelease{};
};
inline bool IsOwnedReleaseLink(const std::string &url, bool download = false) {
   const std::string prefix = ProductReleasesPage() + (download ? "/download/" : "/tag/");
   return url.size() > prefix.size() && url.compare(0, prefix.size(), prefix) == 0 &&
      url.find_first_of("\r\n\\?#") == std::string::npos && url.find("/../") == std::string::npos;
}
inline bool IsWindowsX64Installer(const std::string &name) {
   return name.size() > 20 && name.compare(0, 12, "autopsy-win-") == 0 &&
      name.compare(name.size()-8, 8, "-x64.exe") == 0 &&
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
}
inline std::optional<ReleaseOffer> ChooseProductUpdate(const std::vector<ReleaseOffer> &offers,
   const ReleaseVersion &current, bool includePreviews) {
   std::optional<ReleaseOffer> result; auto newest = current;
   for (const auto &offer : offers) {
      const auto version = ParseReleaseVersion(offer.tag);
      const auto page = ProductReleasesPage() + "/tag/" + offer.tag;
      const auto prefix = ProductReleasesPage() + "/download/" + offer.tag + '/';
      if (offer.draft || !version || (!includePreviews && (offer.prerelease || version->preview)) ||
          offer.page != page || offer.download.compare(0, prefix.size(), prefix) != 0 ||
          !IsWindowsX64Installer(offer.download.substr(prefix.size())) || !(newest < *version)) continue;
      newest = *version; result = offer;
   }
   return result;
}
inline bool ShouldCheckProductUpdates(bool enabled, std::int64_t now, std::int64_t last,
   std::int64_t frequency, bool requestActive) {
   return enabled && !requestActive && now >= 0 && frequency > 0 &&
      (last <= 0 || last > now || now - last >= frequency);
}
}
