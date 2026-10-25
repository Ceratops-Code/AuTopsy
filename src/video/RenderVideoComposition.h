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
enum VideoCropHandle : unsigned { CropLeft = 1, CropTop = 2, CropRight = 4, CropBottom = 8, CropMove = 16 };
// Drag distances use oriented-picture fractions; the opposite edges never
// cross. Moving the rectangle retains its dimensions and stays inside picture.
VideoFrameCrop DragVideoCropHandle(const VideoFrameCrop &initial, unsigned handle,
   double horizontal, double vertical, double minimumWidth, double minimumHeight);
}
