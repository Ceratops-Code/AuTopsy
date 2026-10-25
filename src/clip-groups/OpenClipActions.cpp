#include "OpenClipActions.h"
#include "EditClipPlacement.h"
#include "CommandContext.h"
#include "CommonCommandFlags.h"
#include "DoEffect.h"
#include "MenuRegistry.h"
#include "PluginManager.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include "ProjectWindow.h"
#include "Track.h"
#include "TrackFocus.h"
#include "TrackPanel.h"
#include "ViewInfo.h"
#include "WaveClip.h"
#include "WaveTrack.h"
#include "export/ExportAudioDialog.h"
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
#include "../video/CreateVideoTrack.h"
#include "../video/ChooseVideoExport.h"
#include "../video/ExportVideoMix.h"
#include "Export.h"
#include <wx/filedlg.h>
#include <wx/filefn.h>
#endif

namespace {
// Selection is an adapter input, not a permanent side effect of cancelling an
// effect or exporting. Track IDs survive native list replacements during Undo.
class ScopedClipSelection {
   AudacityProject& mProject;
   SelectedRegion mRegion;
   std::vector<std::pair<TrackId, bool>> mSelected;
   std::weak_ptr<Track> mFocus;
   bool mRestore{true};
public:
   explicit ScopedClipSelection(AudacityProject& project)
      : mProject{project}, mRegion{ViewInfo::Get(project).selectedRegion}
   {
      for (auto* track : TrackList::Get(project))
         mSelected.emplace_back(track->GetId(), track->GetSelected());
      if (auto* focus = TrackFocus::Get(project).Get())
         mFocus = focus->SharedPointer<Track>();
   }
   void Select(WaveTrack& target, double start, double end)
   {
      for (auto* track : TrackList::Get(mProject)) track->SetSelected(track == &target);
      TrackFocus::Get(mProject).Set(&target);
      ViewInfo::Get(mProject).selectedRegion.setTimes(start, end);
      // Paint the clip scope before a modal effect/export window takes over.
      // The dialog and the visible project must start from the same selection.
      auto& panel = TrackPanel::Get(mProject);
      panel.Refresh(false);
      panel.Update();
   }
   void Keep() { mRestore = false; }
   ~ScopedClipSelection()
   {
      if (mRestore) {
         ViewInfo::Get(mProject).selectedRegion = mRegion;
         for (auto* track : TrackList::Get(mProject)) {
            const auto found = std::find_if(mSelected.begin(), mSelected.end(),
               [&](const auto& entry) { return entry.first == track->GetId(); });
            if (found != mSelected.end()) track->SetSelected(found->second);
         }
         if (auto focus = mFocus.lock())
            if (focus->GetOwner().get() == &TrackList::Get(mProject))
               TrackFocus::Get(mProject).Set(focus.get());
      }
      TrackPanel::Get(mProject).Refresh(false);
   }
};

struct AudioClipTarget {
   WaveTrack* track{};
   double start{}, end{};
   wxString name;
};
AudioClipTarget FindAudioClip(const CommandContext& context)
{
   auto& project = context.project;
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before opening clip actions.");
   auto* track = dynamic_cast<WaveTrack*>(context.temporarySelection.pTrack
      ? context.temporarySelection.pTrack : TrackFocus::Get(project).Get());
   if (!track || track->GetOwner().get() != &TrackList::Get(project))
      throw std::runtime_error("Choose an audio clip first.");
   const auto& region = context.temporarySelection.pSelectedRegion
      ? *context.temporarySelection.pSelectedRegion : ViewInfo::Get(project).selectedRegion;
   for (const auto& clip : track->Intervals()) {
      const auto start = clip->GetPlayStartTime(), end = clip->GetPlayEndTime();
      // A context click always owns its clip, regardless of any older selection.
      // Keyboard invocation requires a point inside or the exact whole clip.
      if ((region.isPoint() && start <= region.t0() && region.t0() < end) ||
          (start == region.t0() && end == region.t1()))
         return {track, start, end, clip->GetName()};
   }
   throw std::runtime_error("Choose one audio clip, then open its clip action.");
}
template<typename Action> void WithClipError(AudacityProject& project, Action&& action)
{
   try { action(); }
   catch (const std::exception& error) {
      wxMessageBox(wxString::FromUTF8(error.what()), _("Clip Actions"),
         wxOK | wxICON_ERROR, &ProjectWindow::Get(project));
   }
}
}

void ClipActions::ShowAudioClipVolume(const CommandContext& context)
{
   WithClipError(context.project, [&] {
      const auto target = FindAudioClip(context);
      const auto id = PluginManager::Get().GetByCommandIdentifier("Amplify");
      if (id.empty()) throw std::runtime_error("The built-in Amplify effect is unavailable.");
      ScopedClipSelection selection{context.project};
      selection.Select(*target.track, target.start, target.end);
      // Native sample processing provides both positive/negative dB, clipping
      // controls, cancellation and the existing project-compatible Undo state.
      if (EffectUI::DoEffect(id, context.project, 0)) selection.Keep();
   });
}

void ClipActions::ShowAudioClipExport(const CommandContext& context)
{
   WithClipError(context.project, [&] {
      const auto target = FindAudioClip(context);
      ScopedClipSelection selection{context.project};
      selection.Select(*target.track, target.start, target.end);
      ExportAudioDialog dialog{&ProjectWindow::Get(context.project), context.project,
         target.name, "", ExportAudioDialog::ExportMode::SelectedOnly};
      dialog.ShowModal();
   });
}

void ClipActions::ShowAudioClipProperties(const CommandContext& context)
{
   WithClipError(context.project, [&] {
      const auto target = FindAudioClip(context);
      for (const auto &clip : target.track->Intervals()) if (clip->GetPlayStartTime() == target.start) {
         ShowAudioClipProperties(context.project, *target.track, *clip, &ProjectWindow::Get(context.project)); return;
      }
   });
}
void ClipActions::CloseAudioClipGap(const CommandContext& context)
{
   WithClipError(context.project, [&] {
      const auto target = FindAudioClip(context);
      for (const auto &clip : target.track->Intervals()) if (clip->GetPlayStartTime() == target.start) {
         CloseGapBeforeAudioClip(context.project, *target.track, *clip); return;
      }
   });
}

#ifdef AUTOPSY_HAS_VIDEO_PREVIEW
void ClipActions::ShowVideoClipExport(AudacityProject& project,
   const AuTopsyVideoTrack& track, uint64_t clipId, wxWindow* parent)
{
   WithClipError(project, [&] {
      if (ProjectAudioIO::Get(project).IsAudioActive())
         throw std::runtime_error("Stop playback or recording before exporting a clip.");
      // Freeze picture and audio range before opening dialogs. Exporting a clip
      // uses its own retained content, even when another video covers its track.
      auto plan = VideoTimeline::PlanVideoClipExport(track.Timeline(), clipId);
      auto& picture = plan.picture;
      for (auto* video : VideoTimeline::ProjectVideoTracks(project))
         for (const auto& clip : video->Timeline().clips)
            for (const auto& source : VideoTimeline::VideoContentSources(clip,
                  VideoTimeline::SourceFor(video->Timeline(), clip)))
               picture.protectedSourcePaths.push_back(source.path);
      const wxFileName source{wxString::FromUTF8(picture.sourcePath)};
      wxFileDialog dialog{parent, _("Export Video Clip with Audio Mix"), source.GetPath(),
         source.GetName() + "-clip.mp4",
         _("MP4 video (*.mp4)|*.mp4|Matroska video (*.mkv)|*.mkv"),
         wxFD_SAVE | wxFD_OVERWRITE_PROMPT};
      if (dialog.ShowModal() != wxID_OK) return;
      const auto settings = VideoExport::ChooseVideoExport(parent, {});
      if (!settings) return;
      try {
         VideoExport::ExportMix(project, picture, plan.duration, dialog.GetPath(),
            wxFileExists(dialog.GetPath()), parent, *settings, plan.projectStart);
      }
      catch (const ExportException& error) { throw std::runtime_error(error.What().ToStdString()); }
      catch (const ExportErrorException& error) { throw std::runtime_error(error.GetMessage().Translation().ToStdString()); }
      catch (const ExportDiskFullError&) { throw std::runtime_error("Not enough space to export the clip."); }
   });
}
#endif

namespace {
using namespace MenuRegistry;
AttachedItem sClipVolume{
   Command("ChangeAudioClipVolume", XXO("Clip &Volume..."), ClipActions::ShowAudioClipVolume,
      AudioIONotBusyFlag()), wxT("Edit/Other/Clip")};
AttachedItem sClipProperties{
   Command("AudioClipProperties", XXO("Clip &Properties..."),
      [](const CommandContext &context) { ClipActions::ShowAudioClipProperties(context); },
      AudioIONotBusyFlag()), wxT("Edit/Other/Clip")};
AttachedItem sClipCloseGap{
   Command("CloseAudioClipGap", XXO("Close Gap Before Clip"), ClipActions::CloseAudioClipGap,
      AudioIONotBusyFlag()), wxT("Edit/Other/Clip")};
AttachedItem sClipExport{
   Command("ExportAudioClip", XXO("&Export Clip..."), ClipActions::ShowAudioClipExport,
      AudioIONotBusyFlag()), wxT("Edit/Other/Clip")};
}
