#include "PrepareVideoEffects.h"
#include "RenderVideoComposition.h"
#include <unordered_set>
#include "PrepareGeneratedVideo.h"
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
       << Number(settings.frameRate) << ':' << settings.originalQuality << ':' << settings.preserveAlpha;
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
   const VideoEffectRenderSettings &settings, wxWindow *parent, bool force)
{
   if (!force && !RequiresVideoComposition(layers)) return {};
   const auto rate = settings.frameRate;
   auto spans = PlanVideoTransitions(layers, duration, rate);
   const VideoBlendLayer *reference{};
   for (const auto &span : spans) {
      for (const auto &layer : span.layers) if (layer.rectangle == VideoFrameRectangle{}) { reference = &layer; break; }
      if (reference) break;
   }
   VideoBlendLayer fallback;
   if (!reference) for (const auto *layer : layers) if (!layer->clips.empty()) {
      const auto &clip = layer->clips.front();
      fallback.source = VideoTimeline::SourceFor(*layer, clip); fallback.crop = clip.crop;
      if (clip.content && clip.content->operation != VideoTimeline::VideoClipContent::Operation::Sequence) {
         fallback.source.width = clip.content->width; fallback.source.height = clip.content->height;
      }
      reference = &fallback; if (layer->placement.parentId.empty()) break;
   }
   if (!reference) return {};
   int width = settings.width, height = settings.height;
   const bool automaticCanvas = !settings.width;
   if (automaticCanvas) {
      // Older projects may have no saved geometry. Read original display
      // dimensions through the same maintained probe used by ordinary export.
      // Import/relink metadata describes originals, never proxy dimensions.
      auto sourceWidth = reference->source.width, sourceHeight = reference->source.height;
      if (!sourceWidth || !sourceHeight) {
         const auto size = VideoExport::ReadVideoSourcePictureSize(wxString::FromUTF8(reference->source.path), parent);
         if (!size) throw VideoPreview::PreviewPreparationCancelled{};
         sourceWidth = size->first; sourceHeight = size->second;
      }
      const auto canvas = VideoExport::CalculateVideoExportCanvas(sourceWidth, sourceHeight, reference->crop);
      width = canvas.first; height = canvas.second;
   }
   if (settings.maximumHeight > 0 && height > settings.maximumHeight) {
      width = std::max(2, int(std::llround(double(width) * settings.maximumHeight / height)) / 2 * 2);
      height = settings.maximumHeight / 2 * 2;
   }
   if (width < 2 || height < 2 || width > 16384 || height > 16384 || width % 2 || height % 2)
      throw std::runtime_error("Picture transitions require usable even canvas dimensions.");
   std::vector<std::shared_ptr<const VideoPreview::PreparedMedia>> generated;
   for (auto &span : spans) for (auto &layer : span.layers) if (layer.generated) {
      const auto from = std::max(0.0, layer.sourceStart);
      const auto to = std::min(layer.generated->duration, from + double(span.frameCount) / rate * layer.playbackRate);
      const auto constant = VideoTimeline::IsStaticVideoContent(*layer.generated);
      auto content = layer.generated;
      if (layer.transparent) {
         const auto box = CalculateVideoPlacementRectangle(layer.rectangle, width, height);
         VideoTimeline::Clip layout{1, 0, 0, content->duration}; layout.content = content;
         VideoTimeline::ResizeVideoTextContent(layout, box.width, box.height);
         content = layout.content;
      }
      auto prepared = PrepareGeneratedVideo(*content, settings, parent,
         constant ? 0 : from, constant ? std::min(layer.generated->duration, 1 / layer.generated->frameRate) : to);
      if (constant) { layer.sourceStart = 0; layer.playbackRate = 1; span.requiresBlend = true; }
      else layer.sourceStart -= prepared.contentStart;
      layer.source = {Utf8(prepared.media->playbackPath), prepared.media->duration, prepared.frameRate, prepared.width, prepared.height};
      layer.generated.reset(); generated.push_back(std::move(prepared.media));
   }
   auto result = std::make_shared<PreparedVideoComposition>(); result->settings = settings;
   result->media = std::move(generated);
   auto &timeline = result->timeline;
   timeline.sourcePath = reference->source.path; timeline.sourceDuration = reference->source.duration;
   timeline.frameRate = rate; timeline.sourceWidth = width; timeline.sourceHeight = height;
   const auto canvasSource = reference->source.path;
   const auto canvasCrop = reference->crop;
   for (const auto *layer : layers) for (const auto &clip : layer->clips)
      for (const auto &source : VideoTimeline::VideoContentSources(clip, VideoTimeline::SourceFor(*layer, clip)))
         timeline.protectedSourcePaths.push_back(source.path);
   for (auto &span : spans) {
      if (settings.preserveAlpha) span.requiresBlend = true;
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
            layer.fillsCanvas = automaticCanvas && layer.rectangle == VideoFrameRectangle{} && layer.source.path == canvasSource && layer.crop == canvasCrop;
         const auto graph = VideoTransitionFilterGraph(span, width, height, rate, settings.preserveAlpha);
         std::vector<std::string> sources;
         std::string group = "picture-transition";
         for (const auto &layer : span.layers) { sources.push_back(layer.source.path); group += '\n' + layer.source.path; }
         const auto recipe = graph + (settings.originalQuality ? "lossless" : "preview");
         auto media = VideoPreview::PrepareRenderedVideoMedia(group, recipe, sources, seconds,
            [&](const wxString &output) {
               RenderVideoBlendSpan(span, width, height, rate, settings.originalQuality, parent, output, graph, settings.preserveAlpha);
            }, settings.preserveAlpha ? VideoPreview::RenderedVideoKind::Transparent : VideoPreview::RenderedVideoKind::Ordinary);
         clip.media = {Utf8(media->playbackPath), seconds, rate, width, height};
         result->media.push_back(std::move(media));
      }
      timeline.clips.push_back(std::move(clip));
   }
   VideoTimeline::RequireWellFormedTimeline(timeline, VideoTimeline::MaximumVideoRenderSegments);
   return result;
}

std::shared_ptr<const PreparedVideoComposition> PrepareProjectVideoPreview(AudacityProject &project,
   VideoPreview::PreviewQuality quality, bool precise, wxWindow *parent, bool allowPreparation)
{
   auto &state = Effects(project);
   const auto layers = VideoTimeline::GetProjectVisibleVideoLayers(project);
   const auto settings = PreviewSettings(layers, quality, precise);
   const auto key = ProjectKey(project, settings);
   if (state.key == key) { state.quality = quality; return state.current; }
   // Keep the last completed picture during a gesture or while hidden. This
   // policy gates expensive work only; ordinary decoder seeking stays live.
   if (!allowPreparation && RequiresVideoComposition(layers)) return state.current;
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
void PrepareVideoTextLayouts(const std::vector<const VideoTimeline::Timeline*> &layers,
   const std::string &changedLayerId, wxWindow *parent)
{
   for (const auto *layer : layers) {
      if (std::none_of(layer->clips.begin(), layer->clips.end(), [](const auto &clip) {
             return VideoTimeline::VideoContentUsesAlpha(clip); })) continue;
      double widthFraction = 1, heightFraction = 1;
      const auto *root = layer; bool affected{};
      std::unordered_set<const VideoTimeline::Timeline*> seen;
      for (;;) {
         if (!seen.insert(root).second) throw std::runtime_error("A caption cannot have cyclic parents.");
         affected = affected || root->placement.id == changedLayerId;
         widthFraction *= root->placement.rectangle.width;
         heightFraction *= root->placement.rectangle.height;
         if (root->placement.parentId.empty()) break;
         const auto found = std::find_if(layers.begin(), layers.end(), [&](const auto *item) {
            return item->placement.id == root->placement.parentId;
         });
         if (found == layers.end()) throw std::runtime_error("The caption parent was removed.");
         root = *found;
      }
      if (!affected) continue;
      if (root->clips.empty()) throw std::runtime_error("A caption needs a parent picture canvas.");
      const auto &picture = root->clips.front();
      auto source = VideoTimeline::SourceFor(*root, picture);
      if (!source.width || !source.height) {
         const auto size = VideoExport::ReadVideoSourcePictureSize(wxString::FromUTF8(source.path), parent);
         if (!size) throw VideoPreview::PreviewPreparationCancelled{};
         source.width = size->first; source.height = size->second;
      }
      const auto canvas = VideoExport::CalculateVideoExportCanvas(source.width, source.height, picture.crop);
      const int width = std::max(2, int(std::llround(canvas.first * widthFraction)) & ~1);
      const int height = std::max(2, int(std::llround(canvas.second * heightFraction)) & ~1);
      for (auto clip : layer->clips) if (VideoTimeline::VideoContentUsesAlpha(clip)) {
         VideoTimeline::ResizeVideoTextContent(clip, width, height);
         const auto prepare = [&](const auto &self, const VideoTimeline::VideoClipContent &content) -> void {
            if (content.operation == VideoTimeline::VideoClipContent::Operation::Text)
               PrepareGeneratedVideo(content, {width, height, 0, content.frameRate, true}, parent,
                  0, std::min(content.duration, 1 / content.frameRate));
            else for (const auto &part : content.parts) if (part.content) self(self, *part.content);
         };
         prepare(prepare, *clip.content);
      }
   }
}

std::shared_ptr<const PreparedVideoComposition> PrepareCandidateVideoEffects(AudacityProject &project,
   const AuTopsyVideoTrack &track, const VideoTimeline::Timeline &candidate, wxWindow *parent)
{
   auto &state = Effects(project); PreparingGuard preparing(state.preparing);
   ClipActions::MediaDialogCommandGuard commands;
   std::vector<VideoTimeline::VideoTimelineLayer> input;
   for (const auto *item : VideoTimeline::ProjectVideoTracks(project))
      input.push_back({item == &track ? &candidate : &item->Timeline(), {!item->VideoVisible(), item->VideoSolo()}});
   std::vector<const VideoTimeline::Timeline*> all;
   for (const auto &layer : input) all.push_back(layer.timeline);
   PrepareVideoTextLayouts(all, candidate.placement.id, parent);
   const auto layers = VideoTimeline::VisibleVideoLayers(input);
   return PrepareVideoComposition(layers, std::max(VideoTimeline::GetProjectVideoDuration(project), candidate.End()),
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
