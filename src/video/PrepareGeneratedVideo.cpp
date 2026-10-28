#include "PrepareGeneratedVideo.h"
#include "PrepareVideoEffects.h"
#include "PlanVideoTransitions.h"
#include "PlanVideoExport.h"
#include "ExportVideoMix.h"
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/font.h>
#include <wx/fontenum.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace VideoEditing {
namespace {
std::string Number(double value) {
   std::ostringstream s; s.imbue(std::locale::classic()); s << std::setprecision(17) << value; return s.str();
}
std::string Utf8(const wxString &value) { const auto s = value.ToUTF8(); return {s.data(), s.length()}; }
void Run(const std::vector<wxString> &arguments, wxWindow *parent) {
   if (!VideoExport::RunBundledVideoTool(arguments, parent, _("Prepare Video Picture"),
         _("Preparing the picture. Original media and audio stay unchanged...")))
      throw VideoPreview::PreviewPreparationCancelled{};
}
// Scratch files live inside one cache entry while its exclusive lease is held.
// Failure removes the entry; a killed owner is collected by normal cache cleanup.
struct ScratchFile {
   std::filesystem::path path;
   ~ScratchFile() { std::error_code error; std::filesystem::remove(path, error); }
};
void SaveTitlePicture(const VideoTimeline::VideoClipContent &content, int width, int height,
   const wxString &path) {
   wxBitmap bitmap(width, height, 24); wxMemoryDC dc(bitmap);
   dc.SetBackground(*wxBLACK_BRUSH); dc.Clear(); dc.SetTextForeground(*wxWHITE);
   const auto text = wxString::FromUTF8(content.title);
   int size = std::max(8, height / 9);
   for (;;) {
      dc.SetFont(wxFont{wxFontInfo(size).Family(wxFONTFAMILY_SWISS)});
      wxCoord w{}, h{}; dc.GetMultiLineTextExtent(text, &w, &h);
      if (w <= width * .85 && h <= height * .85) break;
      if (size <= 8) throw std::runtime_error("This title does not fit. Add line breaks or shorten the text.");
      --size;
   }
   dc.DrawLabel(text, wxRect{0, 0, width, height}, wxALIGN_CENTER);
   dc.SelectObject(wxNullBitmap);
   if (!bitmap.ConvertToImage().SaveFile(path, wxBITMAP_TYPE_PNG))
      throw std::runtime_error("Cannot prepare the title picture.");
}
void DescribeContent(std::ostream &out, const VideoTimeline::VideoClipContent &content);
void DescribeClip(std::ostream &out, const VideoTimeline::Clip &clip) {
   out << "clip " << clip.start << ' ' << clip.sourceStart << ' ' << clip.duration << ' ' << clip.playbackRate << ' ' <<
      clip.muted << ' ' << clip.crop.left << ' ' << clip.crop.top << ' ' << clip.crop.right << ' ' << clip.crop.bottom << ' ' <<
      clip.opacity.inStart << ' ' << clip.opacity.inDuration << ' ' << clip.opacity.outEnd << ' ' << clip.opacity.outDuration << ' ' <<
      std::quoted(VideoEditing::EncodeVideoOpacityRamps(clip.opacityModifiers)) << ' ' <<
      std::quoted(clip.media.path) << ' ' << clip.media.duration << ' ' << clip.media.frameRate << ' ' <<
      clip.media.width << ' ' << clip.media.height << '\n';
   if (clip.content) DescribeContent(out, *clip.content);
}
void DescribeContent(std::ostream &out, const VideoTimeline::VideoClipContent &content) {
   out << "content " << int(content.operation) << ' ' << content.duration << ' ' << content.frameRate << ' ' <<
      content.heldTime << ' ' << content.width << ' ' << content.height << ' ' <<
      std::quoted(content.imagePath) << ' ' << std::quoted(content.title) << ' ' << content.parts.size() << '\n';
   const auto &text = content.textAppearance;
   out << std::quoted(text.fontFace) << ' ' << text.heightFraction << ' ' << text.rgb << ' '
      << text.bold << ' ' << text.italic << ' ' << text.outline << '\n';
   for (const auto &part : content.parts) DescribeClip(out, part);
}
std::vector<std::string> SourcePaths(const VideoBlendSpan &span) {
   std::vector<std::string> result;
   for (const auto &layer : span.layers) result.push_back(layer.source.path);
   return result;
}
}

wxImage RenderVideoTextPicture(const VideoTimeline::VideoClipContent &content, int width, int height)
{
   RequireUsableVideoTextAppearance(content.textAppearance);
   if (width < 2 || height < 2 || width > 8192 || height > 8192)
      throw std::runtime_error("A text overlay needs usable picture dimensions.");
   const auto &appearance = content.textAppearance;
   const auto face = wxString::FromUTF8(appearance.fontFace);
   const auto text = wxString::FromUTF8(content.title);
   if (text.empty()) throw std::runtime_error("Enter text for the overlay.");
   if (!face.empty() && !wxFontEnumerator::IsValidFacename(face))
      throw std::runtime_error("The saved caption font is not installed. Choose another font in Edit Text Overlay.");
   auto info = wxFontInfo(wxSize{0, std::max(1, int(std::llround(height * appearance.heightFraction)))})
      .Family(wxFONTFAMILY_SWISS).Bold(appearance.bold).Italic(appearance.italic);
   if (!face.empty()) info.FaceName(face);
   const wxFont font(info);
   const auto draw = [&](bool stroke) {
      wxBitmap bitmap(width, height, 24); wxMemoryDC dc(bitmap);
      dc.SetBackground(*wxBLACK_BRUSH); dc.Clear(); dc.SetTextForeground(*wxWHITE); dc.SetFont(font);
      wxCoord w{}, h{}; dc.GetMultiLineTextExtent(text, &w, &h);
      const int radius = appearance.outline ? std::max(1, std::min(4, int(std::llround(height * appearance.heightFraction / 24)))) : 0;
      if (w + 2 * radius > width || h + 2 * radius > height)
         throw std::runtime_error("The text does not fit its box. Add line breaks, reduce text size or enlarge the box.");
      if (stroke && radius) for (const auto dx : {-radius, 0, radius})
         for (const auto dy : {-radius, 0, radius}) dc.DrawLabel(text, wxRect{dx, dy, width, height}, wxALIGN_CENTER);
      else dc.DrawLabel(text, wxRect{0, 0, width, height}, wxALIGN_CENTER);
      dc.SelectObject(wxNullBitmap); return bitmap.ConvertToImage();
   };
   const auto glyph = draw(false);
   const auto stroke = appearance.outline ? draw(true) : glyph;
   wxImage result(width, height); result.InitAlpha();
   const auto *foreground = glyph.GetData(); const auto *coverage = stroke.GetData();
   auto *pixels = result.GetData(); auto *alpha = result.GetAlpha();
   for (size_t i{}, count = size_t(width) * height; i < count; ++i) {
      const auto a = std::max(foreground[3*i], coverage[3*i]); alpha[i] = a;
      for (int channel{}; channel < 3; ++channel) {
         const auto colour = (appearance.rgb >> (16 - channel * 8)) & 255;
         pixels[3*i + channel] = a ? static_cast<unsigned char>((colour * foreground[3*i] + a/2) / a) : 0;
      }
   }
   return result;
}

void RenderVideoBlendSpan(const VideoBlendSpan &span, int, int, double,
   bool lossless, wxWindow *parent, const wxString &output, const std::string &graph, bool preserveAlpha)
{
   std::vector<wxString> arguments{"-v", "error", "-nostdin", "-n", "-copyts", "-start_at_zero",
      "-filter_complex_threads", "1"};
   for (const auto &layer : span.layers) {
      const auto seek = std::max(0.0, layer.sourceStart - 2.0 / layer.source.frameRate);
      arguments.insert(arguments.end(), {"-ss", wxString::FromUTF8(Number(seek)),
         "-threads", "2", "-i", wxString::FromUTF8(layer.source.path)});
   }
   arguments.insert(arguments.end(), {"-filter_complex", wxString::FromUTF8(graph), "-map", "[vout]",
      "-an", "-sn", "-dn", "-map_metadata", "-1", "-frames:v", wxString::Format("%lld", span.frameCount)});
   if (preserveAlpha) arguments.insert(arguments.end(), {"-c:v", "qtrle", "-pix_fmt", "argb"});
   else arguments.insert(arguments.end(), {"-c:v", "libx264", "-preset", "veryfast", "-crf", lossless ? "0" : "20",
      "-g", "15", "-bf", "0", "-pix_fmt", "yuv420p"});
   arguments.insert(arguments.end(), {"-threads", "2", output});
   Run(arguments, parent);
}

PreparedGeneratedVideo PrepareGeneratedVideo(
   const VideoTimeline::VideoClipContent &content,
   const VideoEffectRenderSettings &settings, wxWindow *parent, double from, double to)
{
   using Operation = VideoTimeline::VideoClipContent::Operation;
   const auto window = PlanVideoGeneratorWindow(content, from, to);
   const auto rate = window.frameRate;
   const auto count = window.frameCount;
   int width = content.width, height = content.height;
   const auto maximum = settings.maximumHeight > 0 ? settings.maximumHeight : settings.height;
   if (maximum > 0 && height > maximum) {
      width = std::max(2, int(std::llround(double(width) * maximum / height)) & ~1); height = maximum & ~1;
   }
   width = std::max(2, width & ~1); height = std::max(2, height & ~1);
   const auto dimensions = std::to_string(width) + "x" + std::to_string(height);
   const auto materialize = [&]() -> std::shared_ptr<const VideoPreview::PreparedMedia> {
   if (content.operation == Operation::Text) {
      std::ostringstream description; description.imbue(std::locale::classic()); description << std::setprecision(17);
      description << "text-overlay=1:" << dimensions << ':' << rate << '\n'; DescribeContent(description, content);
      const auto recipe = description.str();
      return VideoPreview::PrepareRenderedVideoMedia("text-overlay:" + recipe, recipe, {}, window.duration,
         [&](const wxString &output) {
            if (!RenderVideoTextPicture(content, width, height).SaveFile(output, wxBITMAP_TYPE_PNG))
               throw std::runtime_error("Cannot prepare the text overlay.");
         }, VideoPreview::RenderedVideoKind::StaticImage);
   }
   if (content.operation == Operation::Image || content.operation == Operation::Title) {
      const auto recipe = std::string("static-picture=2:") + dimensions + ':' + Number(rate) + ':' +
         std::to_string(count) + ':' + content.title + ':' + (settings.originalQuality ? "lossless" : "preview");
      const std::vector<std::string> sources = content.imagePath.empty()
         ? std::vector<std::string>{} : std::vector<std::string>{content.imagePath};
      return VideoPreview::PrepareRenderedVideoMedia("static-picture:" + content.imagePath + ':' + content.title +
         ':' + Number(rate) + ':' + std::to_string(count), recipe, sources, window.duration,
         [&](const wxString &output) {
            ScratchFile scratch{std::filesystem::path(output.ToStdWstring()).parent_path() / "_title.png"};
            auto image = wxString::FromUTF8(content.imagePath);
            if (content.operation == Operation::Title) {
               image = wxString(scratch.path.wstring()); SaveTitlePicture(content, width, height, image);
            }
            Run({"-v", "error", "-nostdin", "-n",
               "-i", image, "-vf", wxString::FromUTF8("trim=end_frame=1,loop=loop=-1:size=1:start=0,setpts=N/(" +
                  Number(rate) + "*TB),trim=end_frame=" + std::to_string(count) + ",scale=" + std::to_string(width) + ':' + std::to_string(height) +
                  ":force_original_aspect_ratio=decrease:force_divisible_by=2,pad=" + std::to_string(width) + ':' +
                  std::to_string(height) + ":(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,format=yuv420p"),
               "-an", "-map_metadata", "-1", "-frames:v", wxString::Format("%lld", count),
               "-c:v", "libx264", "-preset", "veryfast", "-crf", settings.originalQuality ? "0" : "20",
               "-g", "15", "-bf", "0", "-threads", "2", output}, parent);
         });
   }
   if (content.operation != Operation::Reverse && content.operation != Operation::Freeze)
      throw std::runtime_error("The requested picture generator is unsupported.");
   std::ostringstream description; description.imbue(std::locale::classic()); description << std::setprecision(17);
   description << "generated-picture=2:" << dimensions << ':' << rate << ':' << settings.originalQuality << ':' <<
      (content.operation == Operation::Reverse ? window.contentStart : 0) << ':' << window.duration << '\n';
   DescribeContent(description, content);
   std::ostringstream purpose; purpose.imbue(std::locale::classic()); purpose << std::setprecision(17);
   purpose << (content.operation == Operation::Reverse ? window.contentStart : 0) << ':' << window.duration << ':';
   DescribeContent(purpose, content);
   VideoTimeline::Clip original{1, 0, 0, content.duration};
   original.content = std::make_shared<VideoTimeline::VideoClipContent>(content);
   const bool alpha = VideoTimeline::VideoContentUsesAlpha(original);
   std::vector<std::string> originals;
   for (const auto &source : VideoTimeline::VideoContentSources(original, {})) originals.push_back(source.path);
   // Inspect the final entry before preparing intermediates. The recipe names
   // saved picture data and original inputs, never disposable child cache paths.
   return VideoPreview::PrepareRenderedVideoMedia("generated-picture:" + purpose.str(), description.str(), originals, window.duration,
      [&](const wxString &destination) {
   VideoTimeline::Timeline input; input.clips = content.parts; input.nextId = 1;
   for (const auto &part : input.clips) input.nextId = std::max(input.nextId, part.id + 1);
   input.frameRate = content.frameRate;
   input = VideoTimeline::CopyVideoRange(input, window.inputStart, window.inputEnd);
   VideoEffectRenderSettings sourceSettings{width, height, 0, rate, settings.originalQuality, alpha};
   const auto prepared = PrepareVideoComposition({&input}, input.End(), sourceSettings, parent, true);
   if (!prepared) throw std::runtime_error("The picture generator has no retained input.");
   const auto spans = PlanVideoTransitions({&prepared->timeline}, input.End(), rate);
   std::vector<std::shared_ptr<const VideoPreview::PreparedMedia>> chunks;
   if (content.operation == Operation::Freeze) {
      const std::int64_t held{}; // The retained input was sliced at this held frame.
      auto found = std::find_if(spans.begin(), spans.end(), [&](const auto &span) {
         return held >= span.firstFrame && held < span.firstFrame + span.frameCount;
      });
      if (found == spans.end()) throw std::runtime_error("The frozen frame is outside retained content.");
      auto frame = *found;
      for (auto &layer : frame.layers) layer.sourceStart += double(held - frame.firstFrame) / rate * layer.playbackRate;
      frame.firstFrame = held; frame.frameCount = 1;
      auto graph = VideoTransitionFilterGraph(frame, width, height, rate, alpha);
      const auto end = graph.rfind("[vout]");
      graph.replace(end, 6, "[still];\n[still]tpad=stop_mode=clone:stop=-1,trim=end_frame=" +
         std::to_string(count) + ",setpts=N/(" + Number(rate) + "*TB)[vout]");
      frame.frameCount = count;
      RenderVideoBlendSpan(frame, width, height, rate, settings.originalQuality, parent, destination, graph, alpha);
      return;
   }
   // Reverse buffers decoded frames. Split AFTER composition/scaling into at
   // most two seconds and about 64 MiB of YUV frames, then reverse chunk order.
   const auto maximumFrames = std::max<std::int64_t>(1, std::min<std::int64_t>(
      std::max<std::int64_t>(1, static_cast<std::int64_t>(rate * 2)),
      static_cast<std::int64_t>(64.0 * 1024 * 1024 / (width * double(height) * 1.5))));
   for (auto span = spans.rbegin(); span != spans.rend(); ++span) {
      auto end = span->firstFrame + span->frameCount;
      while (end > span->firstFrame) {
         auto chunk = *span; chunk.firstFrame = std::max(span->firstFrame, end - maximumFrames);
         chunk.frameCount = end - chunk.firstFrame;
         for (auto &layer : chunk.layers)
            layer.sourceStart += double(chunk.firstFrame - span->firstFrame) / rate * layer.playbackRate;
         auto graph = VideoTransitionFilterGraph(chunk, width, height, rate, alpha);
         const auto output = graph.rfind("[vout]");
         graph.replace(output, 6, "[forward];\n[forward]reverse,setpts=N/(" + Number(rate) + "*TB)[vout]");
         if (chunks.size() >= VideoTimeline::MaximumVideoRenderSegments)
            throw std::runtime_error("This reverse operation exceeds its render-segment budget. Split the clip into shorter pieces.");
         chunks.push_back(VideoPreview::PrepareRenderedVideoMedia("reverse-picture-chunk:" + graph, graph + (settings.originalQuality ? "lossless" : "preview"),
            SourcePaths(chunk), double(chunk.frameCount) / rate, [&](const wxString &path) {
               RenderVideoBlendSpan(chunk, width, height, rate, settings.originalQuality, parent, path, graph, alpha);
            }, alpha ? VideoPreview::RenderedVideoKind::Transparent : VideoPreview::RenderedVideoKind::Ordinary));
         end = chunk.firstFrame;
      }
   }
         ScratchFile list{std::filesystem::path(destination.ToStdWstring()).parent_path() / "_concat.txt"};
         std::ofstream file(list.path, std::ios::binary); file << "ffconcat version 1.0\n";
         for (const auto &chunk : chunks) {
            auto path = Utf8(chunk->playbackPath); std::replace(path.begin(), path.end(), '\\', '/');
            std::string escaped; for (const auto c : path) escaped += c == '\'' ? "'\\''" : std::string(1, c);
            file << "file '" << escaped << "'\n";
         }
         file.close(); if (!file) throw std::runtime_error("Cannot write the reverse picture input list.");
         Run({"-v", "error", "-nostdin", "-n", "-f", "concat", "-safe", "0", "-i", wxString(list.path.wstring()),
            "-an", "-map_metadata", "-1", "-c:v", "copy", "-frames:v", wxString::Format("%lld", count), destination}, parent);
      }, alpha ? VideoPreview::RenderedVideoKind::Transparent : VideoPreview::RenderedVideoKind::Ordinary);
   };
   return {materialize(), window.contentStart, rate, width, height};
}
}
