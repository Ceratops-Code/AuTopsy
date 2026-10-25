#include "PrepareVideoEffects.h"
#include "PlanVideoTransitions.h"
#include "RenderVideoComposition.h"
#include "PlanVideoExport.h"
#include "CreateVideoTrack.h"
#include "ExportVideoMix.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include "Project.h"
#include "ProjectWindow.h"
#include <iomanip>
#include <locale>
#include <sstream>

namespace VideoEditing {
namespace {
std::string Number(double value) {
   std::ostringstream out; out.imbue(std::locale::classic());
   out << std::setprecision(17) << value; return out.str();
}
std::string Utf8(const wxString &value) { const auto s = value.ToUTF8(); return {s.data(), s.length()}; }
struct ProjectEffects final : ClientData::Base {
   std::string key;
   VideoPreview::PreviewQuality quality{VideoPreview::PreviewQuality::Automatic};
   std::shared_ptr<const PreparedVideoComposition> current;
   bool preparing{};
};
const AudacityProject::AttachedObjects::RegisteredFactory sEffectsKey{
   [](AudacityProject &) { return std::make_shared<ProjectEffects>(); }
};
ProjectEffects &Effects(AudacityProject &project) { return project.AttachedObjects::Get<ProjectEffects>(sEffectsKey); }
std::string ProjectKey(AudacityProject &project, const VideoEffectRenderSettings &settings) {
   std::ostringstream out; out.imbue(std::locale::classic());
   out << settings.width << ':' << settings.height << ':' << settings.maximumHeight << ':'
       << Number(settings.frameRate) << ':' << settings.originalQuality;
   for (const auto *track : VideoTimeline::ProjectVideoTracks(project))
      out << '/' << track->PictureRevision() << ':' << track->VideoVisible() << ':' << track->VideoSolo();
   return out.str();
}
VideoEffectRenderSettings PreviewSettings(const std::vector<const VideoTimeline::Timeline*> &layers,
   VideoPreview::PreviewQuality quality, bool precise) {
   VideoEffectRenderSettings result;
   if (layers.empty()) return result;
   const auto &timeline = *layers.front();
   auto profile = VideoPreview::ChoosePreviewQualityProfile(quality,
      timeline.sourceWidth, timeline.sourceHeight, timeline.frameRate);
   if (quality == VideoPreview::PreviewQuality::Automatic && (!timeline.sourceWidth || !timeline.sourceHeight))
      profile = {true, 540};
   result.originalQuality = precise || !profile.lightweight;
   result.maximumHeight = result.originalQuality ? 0 : profile.maximumHeight;
   // Precise inspection retains every source frame, including lower layers.
   result.frameRate = result.originalQuality ? timeline.frameRate : 30;
   if (result.originalQuality) for (const auto *layer : layers)
      for (const auto &clip : layer->clips)
         result.frameRate = std::max(result.frameRate, VideoTimeline::VideoContentFrameRate(
            clip, VideoTimeline::SourceFor(*layer, clip)) * clip.playbackRate);
   result.frameRate = std::min(1000.0, result.frameRate);
   return result;
}
struct PreparingGuard {
   bool &flag;
   PreparingGuard(bool &value) : flag(value) {
      if (flag) throw std::runtime_error("Picture transitions are already being prepared.");
      flag = true;
   }
   ~PreparingGuard() { flag = false; }
};
}

std::shared_ptr<const PreparedVideoComposition> PrepareVideoComposition(
   const std::vector<const VideoTimeline::Timeline*> &layers, double duration,
   const VideoEffectRenderSettings &settings, wxWindow *parent)
{
   if (!HasVideoOpacity(layers)) return {};
   const auto rate = settings.frameRate;
   auto spans = PlanVideoTransitions(layers, duration, rate);
   const VideoBlendLayer *reference{};
   for (const auto &span : spans) if (!span.layers.empty()) { reference = &span.layers.back(); break; }
   if (!reference) return {};
   int width = settings.width, height = settings.height;
   const bool automaticCanvas = !settings.width;
   if (automaticCanvas) {
      // Older projects may have no saved geometry. Read original display
      // dimensions through the same maintained probe used by ordinary export;
      // a proxy or a stale saved value must not choose the picture canvas.
      const auto size = VideoExport::ReadVideoSourcePictureSize(wxString::FromUTF8(reference->source.path), parent);
      if (!size) throw VideoPreview::PreviewPreparationCancelled{};
      const auto canvas = VideoExport::CalculateVideoExportCanvas(size->first, size->second, reference->crop);
      width = canvas.first; height = canvas.second;
   }
   if (settings.maximumHeight > 0 && height > settings.maximumHeight) {
      width = std::max(2, int(std::llround(double(width) * settings.maximumHeight / height)) / 2 * 2);
      height = settings.maximumHeight / 2 * 2;
   }
   if (width < 2 || height < 2 || width > 16384 || height > 16384 || width % 2 || height % 2)
      throw std::runtime_error("Picture transitions require usable even canvas dimensions.");
   auto result = std::make_shared<PreparedVideoComposition>(); result->settings = settings;
   auto &timeline = result->timeline;
   timeline.sourcePath = reference->source.path; timeline.sourceDuration = reference->source.duration;
   timeline.frameRate = rate; timeline.sourceWidth = width; timeline.sourceHeight = height;
   const auto canvasSource = reference->source.path;
   const auto canvasCrop = reference->crop;
   for (const auto *layer : layers) for (const auto &clip : layer->clips)
      for (const auto &source : VideoTimeline::VideoContentSources(clip, VideoTimeline::SourceFor(*layer, clip)))
         timeline.protectedSourcePaths.push_back(source.path);
   for (auto &span : spans) {
      if (span.layers.empty()) continue;
      const auto start = static_cast<double>(span.firstFrame) / rate;
      const auto seconds = static_cast<double>(span.frameCount) / rate;
      VideoTimeline::Clip clip{timeline.nextId++, start, 0, std::min(seconds, duration - start)};
      if (!span.requiresBlend) {
         const auto &layer = span.layers.back();
         clip.media = layer.source; clip.sourceStart = layer.sourceStart;
         clip.playbackRate = layer.playbackRate; clip.crop = layer.crop;
      }
      else {
         // The source that chose the automatic canvas fills it exactly, even
         // when mod-2 encoder rounding slightly changes its aspect ratio.
         for (auto &layer : span.layers)
            layer.fillsCanvas = automaticCanvas && layer.source.path == canvasSource && layer.crop == canvasCrop;
         const auto graph = VideoTransitionFilterGraph(span, width, height, rate);
         std::vector<std::string> sources;
         std::string group = "picture-transition";
         for (const auto &layer : span.layers) { sources.push_back(layer.source.path); group += '\n' + layer.source.path; }
         const auto recipe = graph + (settings.originalQuality ? "lossless" : "preview");
         auto media = VideoPreview::PrepareRenderedVideoMedia(group, recipe, sources, seconds,
            [&](const wxString &output) {
               std::vector<wxString> arguments{"-v", "error", "-nostdin", "-n", "-copyts", "-start_at_zero",
                  "-filter_complex_threads", "1"};
               for (const auto &layer : span.layers) {
                  const auto seek = std::max(0.0, layer.sourceStart - 2.0 / layer.source.frameRate);
                  arguments.insert(arguments.end(), {"-ss", wxString::FromUTF8(Number(seek)),
                     "-threads", "2", "-i", wxString::FromUTF8(layer.source.path)});
               }
               arguments.insert(arguments.end(), {"-filter_complex", wxString::FromUTF8(graph), "-map", "[vout]",
                  "-an", "-sn", "-dn", "-map_metadata", "-1", "-frames:v", wxString::Format("%lld", span.frameCount),
                  "-c:v", "libx264", "-preset", "veryfast", "-crf", settings.originalQuality ? "0" : "20",
                  "-g", "15", "-bf", "0", "-threads", "2", "-pix_fmt", "yuv420p", output});
               if (!VideoExport::RunBundledVideoTool(arguments, parent, _("Prepare Picture Transitions"),
                     _("Preparing smooth fades. Original video and audio stay unchanged...")))
                  throw VideoPreview::PreviewPreparationCancelled{};
            });
         clip.media = {Utf8(media->playbackPath), seconds, rate, width, height};
         result->media.push_back(std::move(media));
      }
      timeline.clips.push_back(std::move(clip));
   }
   VideoTimeline::RequireWellFormedTimeline(timeline, VideoTimeline::MaximumVideoRenderSegments);
   return result;
}

std::shared_ptr<const PreparedVideoComposition> PrepareProjectVideoPreview(AudacityProject &project,
   VideoPreview::PreviewQuality quality, bool precise, wxWindow *parent)
{
   auto &state = Effects(project);
   const auto layers = VideoTimeline::GetProjectVisibleVideoLayers(project);
   const auto settings = PreviewSettings(layers, quality, precise);
   const auto key = ProjectKey(project, settings);
   if (state.key == key) { state.quality = quality; return state.current; }
   PreparingGuard preparing(state.preparing);
   ClipActions::MediaDialogCommandGuard commands;
   // Snapshot owned values before any progress dialog yields native events.
   std::vector<VideoTimeline::Timeline> values;
   for (const auto *layer : layers) values.push_back(*layer);
   std::vector<const VideoTimeline::Timeline*> snapshot;
   for (const auto &value : values) snapshot.push_back(&value);
   auto result = PrepareVideoComposition(snapshot, VideoTimeline::GetProjectVideoDuration(project), settings, parent);
   state.key = key; state.current = result; state.quality = quality; return result;
}
void PrepareProjectVideoPlayback(AudacityProject &project) {
   auto &state = Effects(project);
   if (!state.preparing) PrepareProjectVideoPreview(project, state.quality, false, &ProjectWindow::Get(project));
}
std::shared_ptr<const PreparedVideoComposition> PrepareProjectVideoExport(AudacityProject &project,
   const std::vector<const VideoTimeline::Timeline*> &layers, double duration,
   const VideoEffectRenderSettings &settings, wxWindow *parent) {
   auto &state = Effects(project); PreparingGuard preparing(state.preparing);
   ClipActions::MediaDialogCommandGuard commands;
   return PrepareVideoComposition(layers, duration, settings, parent);
}
bool IsPreparingVideoEffects(AudacityProject &project) { return Effects(project).preparing; }
std::shared_ptr<const PreparedVideoComposition> PrepareCandidateVideoEffects(AudacityProject &project,
   const std::vector<const VideoTimeline::Timeline*> &layers, wxWindow *parent)
{
   auto &state = Effects(project); PreparingGuard preparing(state.preparing);
   ClipActions::MediaDialogCommandGuard commands;
   return PrepareVideoComposition(layers, VideoTimeline::GetProjectVideoDuration(project),
      PreviewSettings(layers, state.quality, false), parent);
}
void AdoptPreparedVideoEffects(AudacityProject &project, std::shared_ptr<const PreparedVideoComposition> prepared) {
   auto &state = Effects(project);
   state.key = prepared ? ProjectKey(project, prepared->settings) : std::string{};
   state.current = std::move(prepared);
}
std::shared_ptr<const VideoPreview::PreparedMedia> FindPreparedEffectMedia(
   const PreparedVideoComposition &composition, const std::string &path) {
   for (const auto &media : composition.media) if (Utf8(media->playbackPath) == path) return media;
   return {};
}
}
