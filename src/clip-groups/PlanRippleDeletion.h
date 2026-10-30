// Track-scope policy only. Native tracks remain responsible for sample edits,
// labels, retained footage and history. Keys are opaque host-provided identities.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ClipGroups {
struct RippleClipInput {
   double start{}, end{};
   bool locked{};
   std::vector<std::string> links;
};
template<typename Key> struct RippleTrackInput {
   Key id;
   bool selected{};
   std::string family;
   std::vector<RippleClipInput> clips;
};

// A whole-track time removal must include linked partners of every affected
// clip, including later clips shifted by the cut. Expansion reaches a fixed
// point before any lock can be accepted. Earlier, unaffected links do not widen
// scope. Parent/embedded families share the same time edit even through gaps.
template<typename Key> std::vector<Key> PlanRippleDeletion(
   const std::vector<RippleTrackInput<Key>> &tracks, double from, double to, bool wholeProject)
{
   if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to <= from)
      throw std::runtime_error("Select a nonempty time range for ripple deletion.");
   std::set<Key> identities;
   std::map<std::string, std::vector<size_t>> links, families;
   std::vector<size_t> pending;
   std::vector<bool> included(tracks.size());
   for (size_t i{}; i < tracks.size(); ++i) {
      const auto &track = tracks[i];
      if (!identities.insert(track.id).second) throw std::runtime_error("Ripple tracks have ambiguous identities.");
      if (!track.family.empty()) families[track.family].push_back(i);
      for (const auto &clip : track.clips) {
         if (!std::isfinite(clip.start) || !std::isfinite(clip.end) || clip.end < clip.start)
            throw std::runtime_error("A ripple clip has invalid timing.");
         for (const auto &link : clip.links) if (!link.empty()) links[link].push_back(i);
      }
      if (wholeProject || track.selected) { included[i] = true; pending.push_back(i); }
   }
   const auto include = [&](const std::vector<size_t> &members) {
      for (const auto i : members) if (!included[i]) { included[i] = true; pending.push_back(i); }
   };
   std::set<std::string> expandedLinks, expandedFamilies;
   for (size_t cursor{}; cursor < pending.size(); ++cursor) {
      const auto &track = tracks[pending[cursor]];
      if (!track.family.empty() && expandedFamilies.insert(track.family).second) include(families.at(track.family));
      for (const auto &clip : track.clips) if (clip.end > from)
         for (const auto &link : clip.links)
            if (!link.empty() && expandedLinks.insert(link).second) include(links.at(link));
   }
   std::vector<Key> result;
   for (size_t i{}; i < tracks.size(); ++i) if (included[i]) {
      for (const auto &clip : tracks[i].clips) if (clip.locked && clip.end > from)
         throw std::runtime_error("Unlock affected clips in Clip Properties before ripple deletion. Linked tracks are included.");
      result.push_back(tracks[i].id);
   }
   return result;
}
}
