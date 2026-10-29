// Owned export choices; FFmpeg remains the decoder and encoder implementation.
#pragma once
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
class wxWindow;
namespace VideoExport {
struct ExportSettings {
   std::string preset{"auto"};
   int width{}, height{};
   double frameRate{};
   int quality{23};
};
inline void RequireUsableExportSettings(const ExportSettings &settings)
{
   const auto &p = settings.preset;
   if (p != "auto" && p != "h264" && p != "hevc" && p != "av1" &&
       p != "h264-nvenc" && p != "hevc-nvenc")
      throw std::runtime_error("Choose auto, h264, hevc, av1, h264-nvenc or hevc-nvenc.");
   if (settings.width < 0 || settings.height < 0 || settings.width > 16384 ||
       settings.height > 16384 || settings.width % 2 || settings.height % 2 ||
       ((settings.width == 0) != (settings.height == 0)))
      throw std::runtime_error("Use even output dimensions, or zero for both to use the cropped source size.");
   if (!std::isfinite(settings.frameRate) || settings.frameRate < 0 ||
       settings.frameRate > 1000 || (settings.frameRate > 0 && settings.frameRate < 1))
      throw std::runtime_error("Choose a frame rate from 1 to 1000, or zero to keep the source rate.");
   if (settings.quality < 0 || settings.quality > 51)
      throw std::runtime_error("Choose video quality from 0 (highest) to 51 (lowest).");
}
inline bool AllowsOriginalPictureCopy(const ExportSettings &settings)
{
   RequireUsableExportSettings(settings);
   return settings.preset == "auto" && !settings.width && !settings.height &&
      !settings.frameRate && settings.quality == 23;
}
inline std::vector<std::string> VideoEncoderArguments(const ExportSettings &settings)
{
   RequireUsableExportSettings(settings);
   const auto quality = std::to_string(settings.quality);
   if (settings.preset == "h264-nvenc" || settings.preset == "hevc-nvenc")
      return {"-c:v", settings.preset == "h264-nvenc" ? "h264_nvenc" : "hevc_nvenc",
         "-preset", "p4", "-rc", "vbr", "-cq", quality, "-b:v", "0", "-pix_fmt", "yuv420p"};
   if (settings.preset == "av1")
      return {"-c:v", "libsvtav1", "-preset", "8", "-crf", quality, "-pix_fmt", "yuv420p"};
   if (settings.preset == "hevc")
      return {"-c:v", "libx265", "-preset", "medium", "-crf", quality,
         "-tag:v", "hvc1", "-pix_fmt", "yuv420p"};
   return {"-c:v", "libx264", "-preset", "medium", "-crf", quality, "-pix_fmt", "yuv420p"};
}
// GUI boundary only: cancelling does not change the supplied settings or project.
// A null choice keeps clip exports scoped to that clip. Project exports offer
// the live time selection without changing which tracks participate in the mix.
struct ExportRangeChoice {
   double start{}, end{};
   bool selectedOnly{};
};
std::optional<ExportSettings> ChooseVideoExport(wxWindow *parent, ExportSettings settings,
   ExportRangeChoice *range = nullptr);
}
