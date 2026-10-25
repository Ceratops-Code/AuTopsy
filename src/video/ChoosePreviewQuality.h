// Owned playback-quality choices. Export and original media never use these values.
#pragma once
#include <cmath>
#include <stdexcept>
#include <string>

namespace VideoPreview {
enum class PreviewQuality { Automatic, Low360, Medium720, High1080, Original };
struct PreviewQualityProfile {
   bool lightweight{};
   int maximumHeight{540};
};
inline PreviewQualityProfile ChoosePreviewQualityProfile(PreviewQuality quality,
   int width, int height, double frameRate)
{
   if (width < 0 || height < 0 || width > 65536 || height > 65536 ||
       !std::isfinite(frameRate) || frameRate <= 0 || frameRate > 1000)
      throw std::runtime_error("Preview quality requires usable source dimensions and frame rate.");
   switch (quality) {
   case PreviewQuality::Automatic:
      return {double(width) * height > 1280.0 * 720.0 || frameRate > 60, 540};
   case PreviewQuality::Low360: return {true, 360};
   case PreviewQuality::Medium720: return {true, 720};
   case PreviewQuality::High1080: return {true, 1080};
   case PreviewQuality::Original: return {false, 540};
   default: throw std::runtime_error("Choose a supported preview quality.");
   }
}
inline std::string PreviewQualityScaleFilter(int maximumHeight)
{
   // The existing automatic 540p profile keeps its recipe and cache identity.
   // Explicit profiles never upscale smaller footage; all proxies cap motion at 30 fps.
   if (maximumHeight == 540)
      return "scale=960:540:force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1,fps=30";
   if (maximumHeight != 360 && maximumHeight != 720 && maximumHeight != 1080)
      throw std::runtime_error("Choose a supported preview resolution.");
   return "scale=w='min(iw," + std::to_string(maximumHeight * 16 / 9) +
      ")':h='min(ih," + std::to_string(maximumHeight) +
      ")':force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1,fps=30";
}
}
