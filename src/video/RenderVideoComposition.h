// Pure geometry at the decoder/encoder boundary; no native project or GUI types.
#pragma once
#include "EditVideoContent.h"

namespace VideoEditing {
struct VideoPixelRectangle { int x{}, y{}, width{}, height{}; };
// Crop margins describe the oriented picture. mpv accepts a rectangle in raw
// decoded pixels, before its clockwise display rotation. Proxies use their own
// decoded dimensions with the same fractions, never original pixel coordinates.
VideoPixelRectangle CalculateVideoCropRectangle(const VideoFrameCrop &crop,
   int decodedWidth, int decodedHeight, int clockwiseRotation);
// FFmpeg applies source display rotation before this filter. The expression
// uses its actual oriented dimensions, including sources of different sizes.
std::string VideoCropFilterExpression(const VideoFrameCrop &crop);
VideoPixelRectangle CalculateVideoPlacementRectangle(const VideoFrameRectangle &rectangle,
   int canvasWidth, int canvasHeight);
// One logical glyph canvas per box aspect, independent of output resolution.
// The compositor scales its alpha picture; low preview quality cannot change
// font fitting. The raster stays within the existing 8192-pixel generator bound.
VideoPixelRectangle CalculateVideoTextRasterSize(const VideoFrameRectangle &rectangle,
   int canvasWidth, int canvasHeight);
enum VideoCropHandle : unsigned { CropLeft = 1, CropTop = 2, CropRight = 4, CropBottom = 8, CropMove = 16 };
// Drag distances use oriented-picture fractions; the opposite edges never
// cross. Moving the rectangle retains its dimensions and stays inside picture.
VideoFrameCrop DragVideoCropHandle(const VideoFrameCrop &initial, unsigned handle,
   double horizontal, double vertical, double minimumWidth, double minimumHeight);
}
