#include "RenderVideoComposition.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace VideoEditing {
VideoFrameCrop DragVideoCropHandle(const VideoFrameCrop &initial, unsigned handle,
   double horizontal, double vertical, double minimumWidth, double minimumHeight)
{
   RequireUsableVideoFrameCrop(initial);
   if (!std::isfinite(horizontal) || !std::isfinite(vertical) ||
       !std::isfinite(minimumWidth) || !std::isfinite(minimumHeight) ||
       minimumWidth <= 0 || minimumHeight <= 0 || minimumWidth > 1 || minimumHeight > 1 ||
       !handle || (handle & ~31u) || (handle != CropMove && (handle & CropMove)))
      throw std::runtime_error("Choose a usable picture crop handle and drag distance.");
   auto value = initial;
   if (handle == CropMove) {
      const auto dx = std::clamp(horizontal, -initial.left, initial.right);
      const auto dy = std::clamp(vertical, -initial.top, initial.bottom);
      value.left += dx; value.right -= dx; value.top += dy; value.bottom -= dy;
   }
   else {
      if (handle & CropLeft) value.left = std::clamp(initial.left + horizontal, 0.0, std::max(0.0, 1 - initial.right - minimumWidth));
      if (handle & CropRight) value.right = std::clamp(initial.right - horizontal, 0.0, std::max(0.0, 1 - value.left - minimumWidth));
      if (handle & CropTop) value.top = std::clamp(initial.top + vertical, 0.0, std::max(0.0, 1 - initial.bottom - minimumHeight));
      if (handle & CropBottom) value.bottom = std::clamp(initial.bottom - vertical, 0.0, std::max(0.0, 1 - value.top - minimumHeight));
   }
   RequireUsableVideoFrameCrop(value); return value;
}

VideoPixelRectangle CalculateVideoCropRectangle(const VideoFrameCrop &crop,
   int width, int height, int rotation)
{
   RequireUsableVideoFrameCrop(crop);
   if (width < 1 || height < 1 || width > 65536 || height > 65536)
      throw std::runtime_error("Picture crop requires usable decoded dimensions.");
   rotation = (rotation % 360 + 360) % 360;
   VideoFrameCrop raw;
   switch (rotation) {
   case 0: raw = crop; break;
   case 90: raw = {crop.top, crop.right, crop.bottom, crop.left}; break;
   case 180: raw = {crop.right, crop.bottom, crop.left, crop.top}; break;
   case 270: raw = {crop.bottom, crop.left, crop.top, crop.right}; break;
   default: throw std::runtime_error("Picture crop requires a display rotation in quarter turns.");
   }
   const int left = int(std::floor(width * raw.left)), right = int(std::floor(width * raw.right));
   const int top = int(std::floor(height * raw.top)), bottom = int(std::floor(height * raw.bottom));
   return {left, top, width - left - right, height - top - bottom};
}

std::string VideoCropFilterExpression(const VideoFrameCrop &crop)
{
   RequireUsableVideoFrameCrop(crop);
   if (crop == VideoFrameCrop{}) return {};
   std::ostringstream text;
   text.imbue(std::locale::classic()); text << std::setprecision(17);
   text << "crop=w=iw-floor(iw*" << crop.left << ")-floor(iw*" << crop.right
      << "):h=ih-floor(ih*" << crop.top << ")-floor(ih*" << crop.bottom
      << "):x=floor(iw*" << crop.left << "):y=floor(ih*" << crop.top << "):exact=1";
   return text.str();
}
}
