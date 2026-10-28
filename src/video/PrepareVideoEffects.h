// Shared picture-only transition preparation. Original media and project audio
// remain untouched; derived files belong to the leased preview cache.
#pragma once
#include "EditVideoTimeline.h"
#include "PrepareVideoPreview.h"
#include "ChoosePreviewQuality.h"
class AudacityProject;
class AuTopsyVideoTrack;
namespace VideoEditing {
struct VideoEffectRenderSettings {
   int width{}, height{}, maximumHeight{};
   double frameRate{};
   bool originalQuality{};
   bool preserveAlpha{}; // Internal caption intermediates; final output stays opaque.
};
struct PreparedVideoComposition {
   VideoTimeline::Timeline timeline;
   std::vector<std::shared_ptr<const VideoPreview::PreparedMedia>> media;
   VideoEffectRenderSettings settings;
};
std::shared_ptr<const PreparedVideoComposition> PrepareVideoComposition(
   const std::vector<const VideoTimeline::Timeline*> &layers, double duration,
   const VideoEffectRenderSettings &settings, wxWindow *parent, bool force = false);
// Check affected caption layouts even in muted tracks/clips or parent gaps.
// All candidate/ancestor inputs are owned snapshots; nothing is published here.
void PrepareVideoTextLayouts(const std::vector<const VideoTimeline::Timeline*> &layers,
   const std::string &changedLayerId, wxWindow *parent);
std::shared_ptr<const PreparedVideoComposition> PrepareProjectVideoPreview(AudacityProject &project,
   VideoPreview::PreviewQuality quality, bool precise, wxWindow *parent, bool allowPreparation = true);
std::shared_ptr<const PreparedVideoComposition> PrepareProjectVideoExport(AudacityProject &project,
   const std::vector<const VideoTimeline::Timeline*> &layers, double duration,
   const VideoEffectRenderSettings &settings, wxWindow *parent);
void PrepareProjectVideoPlayback(AudacityProject &project);
bool IsPreparingVideoEffects(AudacityProject &project);
// Called before a picture edit publishes live state, so cancellation cannot
// leave a half-applied edit. Adoption retains every generated span's lease.
std::shared_ptr<const PreparedVideoComposition> PrepareCandidateVideoEffects(AudacityProject &project,
   const AuTopsyVideoTrack &track, const VideoTimeline::Timeline &candidate, wxWindow *parent);
void AdoptPreparedVideoEffects(AudacityProject &project,
   std::shared_ptr<const PreparedVideoComposition> prepared);
std::shared_ptr<const VideoPreview::PreparedMedia> FindPreparedEffectMedia(
   const PreparedVideoComposition &composition, const std::string &path);
}
