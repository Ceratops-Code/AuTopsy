// Clip-relative flags share a value model; host adapters own XML and drawing.
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
class WaveClip;
class wxDC;
class wxRect;
class ZoomInfo;
namespace ClipMarkers {
struct Marker {
   std::string id;
   double position{}; // Source seconds before clip trimming and speed.
   std::string text;
};
inline void RequireUsableMarkers(const std::vector<Marker> &markers)
{
   if (markers.size() > 128) throw std::runtime_error("A clip can hold at most 128 flags.");
   std::unordered_set<std::string> ids;
   for (const auto &marker : markers)
      if (marker.id.size() != 32 || marker.id.find_first_not_of("0123456789abcdef") != std::string::npos ||
          !ids.insert(marker.id).second || !std::isfinite(marker.position) || marker.position < 0 ||
          marker.position > 100000000 || marker.text.empty() || marker.text.size() > 256 ||
          std::any_of(marker.text.begin(), marker.text.end(), [](unsigned char c) { return c < 32; }))
         throw std::runtime_error("Clip flags require unique IDs, a usable position and a short label.");
}
inline double ProjectMarkerTime(const Marker &marker, double origin, double speed)
{
   return origin + marker.position * speed;
}
inline bool PutMarker(std::vector<Marker> &markers, Marker marker)
{
   auto changed = markers;
   const auto found = std::find_if(changed.begin(), changed.end(),
      [&](const auto &item) { return item.id == marker.id; });
   if (found != changed.end()) {
      if (found->position == marker.position && found->text == marker.text) return false;
      *found = std::move(marker);
   }
   else changed.push_back(std::move(marker));
   RequireUsableMarkers(changed); markers = std::move(changed); return true;
}
std::string EncodeMarkers(const std::vector<Marker> &markers);
std::vector<Marker> DecodeMarkers(const std::string &text);
const std::vector<Marker> &AudioMarkers(const WaveClip &clip);
void DrawClipMarkerFlags(wxDC &dc, const wxRect &rect, const ZoomInfo &zoom,
   const std::vector<Marker> &markers, double origin, double speed, double from, double to);
void DrawAudioClipMarkers(const WaveClip &clip, wxDC &dc, const ZoomInfo &zoom, const wxRect &rect);
}
