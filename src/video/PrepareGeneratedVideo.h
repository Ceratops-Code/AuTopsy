// Native media boundary for owned picture generators. Generated files belong
// exclusively to the leased preview cache and can always be recreated.
#pragma once
#include "EditVideoContent.h"
#include "PrepareVideoPreview.h"
class wxWindow;
class wxImage;
namespace VideoEditing {
struct VideoEffectRenderSettings;
struct VideoBlendSpan;
void RenderVideoBlendSpan(const VideoBlendSpan &span, int width, int height,
   double rate, bool lossless, wxWindow *parent, const wxString &output,
   const std::string &graph, bool preserveAlpha = false);
// Native font rasterization is shared by caption generation and its filmstrip.
wxImage RenderVideoTextPicture(const VideoTimeline::VideoClipContent &content, int width, int height);
struct PreparedGeneratedVideo {
   std::shared_ptr<const VideoPreview::PreparedMedia> media;
   double contentStart{}, frameRate{};
   int width{}, height{};
};
PreparedGeneratedVideo PrepareGeneratedVideo(
   const VideoTimeline::VideoClipContent &content,
   const VideoEffectRenderSettings &settings, wxWindow *parent, double from, double to);
}
