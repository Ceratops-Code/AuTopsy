#include "RenderVideoComposition.h"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace VideoEditing {
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
