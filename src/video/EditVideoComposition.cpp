#include "EditVideoComposition.h"
#include "CreateVideoTrack.h"
#include "PrepareVideoEffects.h"
#include "ProjectWindow.h"
#include "ShowVideoCrop.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include "Project.h"
#include "ProjectAudioIO.h"
#include <wx/button.h>
#include <wx/choicdlg.h>
#include <unordered_set>
#include "ProjectHistory.h"
#include "Viewport.h"
#include "ViewInfo.h"
#include "PlanVideoExport.h"
#include "ExportVideoMix.h"
#include "Uuid.h"
#include <wx/checkbox.h>
#include <wx/clrpicker.h>
#include <wx/combobox.h>
#include <wx/fontenum.h>
#include <wx/panel.h>
#include <wx/textctrl.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <algorithm>
#include <stdexcept>

namespace VideoEditing {
bool ApplyProjectVideoPictureAction(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, VideoClipPictureAction action, double at, double seconds, int copies)
{
   ClipActions::MediaDialogCommandGuard commands;
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing the picture.");
   if (track.GetOwner().get() != &TrackList::Get(project)) throw std::runtime_error("The video track was removed.");
   auto candidate = track.Timeline();
   bool changed{};
   if (action == VideoClipPictureAction::Reverse) changed = VideoTimeline::ReverseVideoClip(candidate, clipId);
   else if (action == VideoClipPictureAction::Freeze) changed = VideoTimeline::FreezeVideoClip(candidate, clipId, at, seconds);
   else changed = VideoTimeline::RepeatVideoClip(candidate, clipId, copies);
   if (!changed) return false;
   auto prepared = PrepareCandidateVideoEffects(project, track, candidate, &ProjectWindow::Get(project));
   const auto previous = track.Timeline(); track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(XO("Changed video picture"), XO("Video Picture")); }
   catch (...) { track.SetTimeline(previous); throw; }
   AdoptPreparedVideoEffects(project, std::move(prepared));
   Viewport::Get(project).HandleResize(); return true;
}
void ShowVideoClipPictureAction(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, VideoClipPictureAction action, wxWindow *parent, double at)
{
   ClipActions::MediaDialogCommandGuard commands;
   const auto found = std::find_if(track.Timeline().clips.begin(), track.Timeline().clips.end(),
      [clipId](const auto &clip) { return clip.id == clipId; });
   if (found == track.Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
   if (action == VideoClipPictureAction::Reverse) {
      ApplyProjectVideoPictureAction(project, track, clipId, action, at, found->duration); return;
   }
   const bool freeze = action == VideoClipPictureAction::Freeze;
   if (freeze && (at < found->start || at >= found->End()))
      throw std::runtime_error("Click inside the picture at the frame you want to freeze, then open Freeze Frame.");
   wxTextEntryDialog dialog{parent,
      freeze ? _("Hold the frame at the cursor for how many seconds?\nThis changes picture only; audio stays unchanged.")
         : _("How many extra copies of this picture clip?\nCopies fill the following space. Audio stays unchanged."),
      freeze ? _("Freeze Frame") : _("Repeat Picture"), freeze ? wxString::Format("%.8g", found->duration) : wxString{"1"}};
   if (dialog.ShowModal() != wxID_OK) return;
   double seconds{}; long count{1};
   if (freeze ? !dialog.GetValue().ToDouble(&seconds) : !dialog.GetValue().ToLong(&count))
      throw std::runtime_error("Enter a numeric value.");
   ApplyProjectVideoPictureAction(project, track, clipId, action, at, seconds, static_cast<int>(count));
}
void AddProjectVideoPicture(AudacityProject &project, VideoTimeline::Clip clip, const wxString &name)
{
   ClipActions::MediaDialogCommandGuard commands;
   if (ProjectAudioIO::Get(project).IsAudioActive()) throw std::runtime_error("Stop playback or recording before adding a picture.");
   VideoTimeline::Timeline timeline; timeline.nextId = clip.id + 1; timeline.clips.push_back(std::move(clip));
   timeline.sourceDuration = timeline.clips.front().content->duration;
   timeline.sourceWidth = timeline.clips.front().content->width; timeline.sourceHeight = timeline.clips.front().content->height;
   // Complete potentially cancellable rendering before publishing a native row.
   const auto prepared = PrepareVideoComposition({&timeline}, timeline.End(), {0, 0, 540, 30, false}, &ProjectWindow::Get(project));
   const auto tracks = TrackList::Get(project).Any<Track>();
   const auto track = VideoTimeline::CreateProjectVideoTrack(project, tracks.empty() ? nullptr : *tracks.begin(),
      true, false, std::move(timeline), name);
   try { ProjectHistory::Get(project).PushState(XO("Added still picture or title"), XO("Add Video Picture")); }
   catch (...) { TrackList::Get(project).Remove(*track); throw; }
   Viewport::Get(project).ShowTrack(*track); Viewport::Get(project).HandleResize();
}

bool ChangeProjectVideoClipMute(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, bool muted)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before changing clip picture mute.");
   auto candidate = track.Timeline();
   if (!VideoTimeline::ChangeVideoClipMute(candidate, clipId, muted)) return false;
   const auto previous = track.Timeline();
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(
      muted ? XO("Muted video clip picture") : XO("Unmuted video clip picture"), XO("Video Clip Mute")); }
   catch (...) { track.SetTimeline(previous); throw; }
   Viewport::Get(project).HandleResize();
   return true;
}

bool ChangeProjectVideoClipCrop(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, const VideoFrameCrop &crop)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing picture crop.");
   auto candidate = track.Timeline();
   if (!VideoTimeline::ChangeVideoClipCrop(candidate, clipId, crop)) return false;
   const auto previous = track.Timeline();
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(XO("Cropped video picture"), XO("Video Crop")); }
   catch (...) { track.SetTimeline(previous); throw; }
   Viewport::Get(project).HandleResize();
   return true;
}

void ShowVideoClipCropDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent)
{
   ClipActions::MediaDialogCommandGuard commands;
   const auto found = std::find_if(track.Timeline().clips.begin(), track.Timeline().clips.end(),
      [clipId](const auto &clip) { return clip.id == clipId; });
   if (found == track.Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
   const auto original = *found;
   ShowVideoPictureCrop(parent, original, VideoTimeline::SourceFor(track.Timeline(), original),
      ViewInfo::Get(project).selectedRegion.t0(), [&](const VideoFrameCrop &selected) {
         ChangeProjectVideoClipCrop(project, track, clipId, selected);
      });
}
}


namespace VideoEditing {
bool ChangeProjectVideoTrackPlacement(AudacityProject &project, AuTopsyVideoTrack &track,
   const VideoLayerPlacement &placement)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before placing video layers.");
   if (track.GetOwner().get() != &TrackList::Get(project))
      throw std::runtime_error("The video track was removed.");
   RequireUsableVideoPlacement(placement);
   auto candidate = track.Timeline();
   if (placement.id != candidate.placement.id)
      throw std::runtime_error("Video placement cannot replace a track's identity.");
   if (placement.parentId == candidate.placement.parentId && placement.rectangle == candidate.placement.rectangle) return false;
   const auto tracks = VideoTimeline::ProjectVideoTracks(project);
   std::unordered_set<std::string> seen{placement.id};
   auto ancestor = placement.parentId;
   while (!ancestor.empty()) {
      if (!seen.insert(ancestor).second) throw std::runtime_error("A video cannot be embedded in itself or one of its descendants.");
      const auto found = std::find_if(tracks.begin(), tracks.end(), [&](const auto *item) { return item->Timeline().placement.id == ancestor; });
      if (found == tracks.end()) throw std::runtime_error("Choose an existing parent video track.");
      ancestor = (*found)->Timeline().placement.parentId;
   }
   candidate.placement = placement;
   auto prepared = PrepareCandidateVideoEffects(project, track, candidate, &ProjectWindow::Get(project));
   const auto previous = track.Timeline();
   track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(XO("Changed picture-in-picture placement"), XO("Picture-in-Picture")); }
   catch (...) { track.SetTimeline(previous); throw; }
   AdoptPreparedVideoEffects(project, std::move(prepared));
   Viewport::Get(project).Redraw(); return true;
}

void ShowVideoTrackEmbeddingDialog(AudacityProject &project, AuTopsyVideoTrack &track, wxWindow *parent)
{
   ClipActions::MediaDialogCommandGuard commands;
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before placing video layers.");
   const auto original = track.Timeline();
   const auto tracks = VideoTimeline::ProjectVideoTracks(project);
   wxArrayString names; names.Add(_("None (full frame)"));
   std::vector<AuTopsyVideoTrack*> parents{nullptr};
   int selected{};
   for (auto *item : tracks) {
      std::unordered_set<std::string> ancestors;
      auto id = item->Timeline().placement.id;
      bool descendant{};
      while (!id.empty() && ancestors.insert(id).second) {
         if (id == original.placement.id) { descendant = true; break; }
         const auto found = std::find_if(tracks.begin(), tracks.end(), [&](const auto *other) { return other->Timeline().placement.id == id; });
         id = found == tracks.end() ? std::string{} : (*found)->Timeline().placement.parentId;
      }
      if (descendant) continue;
      if (item->Timeline().placement.id == original.placement.parentId) selected = static_cast<int>(parents.size());
      parents.push_back(item); names.Add(item->GetName());
   }
   wxSingleChoiceDialog choice{parent,
      _("Choose the parent video. The embedded track keeps its own clips and audio.\n"
        "Its picture is hidden wherever its parent has no picture."),
      _("Picture-in-Picture Parent"), names};
   choice.SetSelection(selected);
   if (choice.ShowModal() != wxID_OK) return;
   const auto index = choice.GetSelection();
   if (index < 0 || static_cast<size_t>(index) >= parents.size()) return;
   auto placement = original.placement;
   if (!index) {
      placement.parentId.clear(); placement.rectangle = {};
      ChangeProjectVideoTrackPlacement(project, track, placement); return;
   }
   const auto *background = parents[index];
   placement.parentId = background->Timeline().placement.id;
   if (original.placement.parentId.empty()) placement.rectangle = {.65, .05, .3, .3};
   const auto at = ViewInfo::Get(project).selectedRegion.t0();
   const auto frame = [&](const VideoTimeline::Timeline &timeline) -> std::optional<VideoPlacementPicture> {
      if (timeline.clips.empty()) return {};
      auto *clip = VideoTimeline::ClipAt(timeline, at); if (!clip) clip = &timeline.clips.front();
      return VideoPlacementPicture{*clip, VideoTimeline::SourceFor(timeline, *clip)};
   };
   ShowVideoPicturePlacement(parent, frame(original), frame(background->Timeline()), at, placement.rectangle,
      [&](const VideoFrameRectangle &rectangle) { placement.rectangle = rectangle; ChangeProjectVideoTrackPlacement(project, track, placement); });
}
}

namespace VideoEditing {
namespace {
void RequireTextTrack(AudacityProject &project, const AuTopsyVideoTrack &track)
{
   if (ProjectAudioIO::Get(project).IsAudioActive())
      throw std::runtime_error("Stop playback or recording before editing text overlays.");
   if (track.GetOwner().get() != &TrackList::Get(project))
      throw std::runtime_error("The video track was removed.");
}
std::shared_ptr<const PreparedVideoComposition> PrepareAddedTextTrack(AudacityProject &project,
   AuTopsyVideoTrack &parent, const VideoTimeline::Timeline &candidate)
{
   std::vector<VideoTimeline::VideoTimelineLayer> input;
   for (auto *track : VideoTimeline::ProjectVideoTracks(project)) {
      input.push_back({&track->Timeline(), {!track->VideoVisible(), track->VideoSolo()}});
      if (track == &parent) input.push_back({&candidate, {false, false}});
   }
   std::vector<const VideoTimeline::Timeline*> all;
   for (const auto &layer : input) all.push_back(layer.timeline);
   PrepareVideoTextLayouts(all, candidate.placement.id, &ProjectWindow::Get(project));
   return PrepareVideoComposition(VideoTimeline::VisibleVideoLayers(input),
      std::max(VideoTimeline::GetProjectVideoDuration(project), candidate.End()),
      {0, 0, 540, 30, false}, &ProjectWindow::Get(project));
}
}

std::shared_ptr<AuTopsyVideoTrack> AddProjectVideoTextOverlay(AudacityProject &project,
   AuTopsyVideoTrack &parent, const VideoTextOverlaySettings &settings)
{
   ClipActions::MediaDialogCommandGuard commands;
   RequireTextTrack(project, parent);
   const auto *picture = VideoTimeline::ClipAt(parent.Timeline(), settings.start);
   if (!picture) throw std::runtime_error("Choose a point inside the parent video for the text overlay.");
   if (!std::isfinite(settings.seconds) || settings.seconds <= 0 ||
       settings.start + settings.seconds > parent.Timeline().End() + 1e-7)
      throw std::runtime_error("Keep the text overlay inside the parent video's time range.");
   auto source = VideoTimeline::SourceFor(parent.Timeline(), *picture);
   if (picture->content) { source.width = picture->content->width; source.height = picture->content->height; }
   if (source.width < 2 || source.height < 2) {
      const auto size = VideoExport::ReadVideoSourcePictureSize(wxString::FromUTF8(source.path), &ProjectWindow::Get(project));
      if (!size) throw VideoPreview::PreviewPreparationCancelled{};
      source.width = size->first; source.height = size->second;
   }
   const auto canvas = VideoExport::CalculateVideoExportCanvas(source.width, source.height, picture->crop);
   VideoTimeline::Timeline candidate;
   candidate.placement = {audacity::Uuid::Generate().ToHexString(), parent.Timeline().placement.id, settings.rectangle};
   RequireUsableVideoPlacement(candidate.placement);
   const auto width = std::max(2, int(std::llround(canvas.first * settings.rectangle.width)) & ~1);
   const auto height = std::max(2, int(std::llround(canvas.second * settings.rectangle.height)) & ~1);
   candidate.clips.push_back(VideoTimeline::MakeTextVideoClip(1, settings.start, settings.seconds,
      settings.text, width, height, settings.appearance));
   candidate.nextId = 2; candidate.sourceWidth = width; candidate.sourceHeight = height;
   candidate.sourceDuration = settings.seconds;
   // Fonts, layout, source reads and cancellable rendering all finish before
   // inserting the row. One history entry owns the complete visible effect.
   auto prepared = PrepareAddedTextTrack(project, parent, candidate);
   auto track = VideoTimeline::CreateProjectVideoTrack(project, &parent, false, false,
      std::move(candidate), _("Text Overlays"));
   try { ProjectHistory::Get(project).PushState(XO("Added text overlay"), XO("Text Overlay")); }
   catch (...) { TrackList::Get(project).Remove(*track); throw; }
   AdoptPreparedVideoEffects(project, std::move(prepared));
   Viewport::Get(project).ShowTrack(*track); Viewport::Get(project).HandleResize();
   return track;
}

bool ChangeProjectVideoTextOverlay(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, const VideoTextOverlaySettings &settings)
{
   ClipActions::MediaDialogCommandGuard commands;
   RequireTextTrack(project, track);
   auto candidate = track.Timeline();
   auto found = std::find_if(candidate.clips.begin(), candidate.clips.end(), [clipId](const auto &clip) { return clip.id == clipId; });
   if (found == candidate.clips.end() || !VideoTimeline::FindEditableVideoTextContent(*found))
      throw std::runtime_error("Select a text overlay clip. Separate joined text clips to edit their individual wording.");
   if (found->timingLocked && (settings.start != found->start || settings.seconds != found->duration))
      throw std::runtime_error("Unlock the text clip before changing its timing.");
   VideoTimeline::ChangeVideoTextContent(*found, settings.text, settings.appearance);
   auto content = std::make_shared<VideoTimeline::VideoClipContent>(*found->content);
   if (content->operation == VideoTimeline::VideoClipContent::Operation::Text ||
       content->operation == VideoTimeline::VideoClipContent::Operation::Freeze)
      content->duration = std::max(content->duration, found->sourceStart + settings.seconds * found->playbackRate);
   found->content = std::move(content); found->start = settings.start; found->duration = settings.seconds;
   candidate.placement.rectangle = settings.rectangle;
   std::sort(candidate.clips.begin(), candidate.clips.end(), [](const auto &a, const auto &b) { return a.start < b.start; });
   VideoTimeline::RequireWellFormedTimeline(candidate);
   if (candidate.placement.rectangle == track.Timeline().placement.rectangle &&
       candidate.clips.size() == track.Timeline().clips.size() &&
       std::equal(candidate.clips.begin(), candidate.clips.end(), track.Timeline().clips.begin(),
          [](const auto &a, const auto &b) { return a.id == b.id && a.start == b.start && a.duration == b.duration &&
             a.sourceStart == b.sourceStart && VideoTimeline::SameVideoContent(a, b); })) return false;
   auto prepared = PrepareCandidateVideoEffects(project, track, candidate, &ProjectWindow::Get(project));
   const auto previous = track.Timeline(); track.SetTimeline(std::move(candidate));
   try { ProjectHistory::Get(project).PushState(XO("Changed text overlay"), XO("Text Overlay")); }
   catch (...) { track.SetTimeline(previous); throw; }
   AdoptPreparedVideoEffects(project, std::move(prepared)); Viewport::Get(project).Redraw(); return true;
}

bool ShowVideoTextOverlayDialog(AudacityProject &project, AuTopsyVideoTrack &track,
   uint64_t clipId, wxWindow *parent, double at, bool edit)
{
   ClipActions::MediaDialogCommandGuard commands;
   RequireTextTrack(project, track);
   VideoTextOverlaySettings value;
   const auto found = std::find_if(track.Timeline().clips.begin(), track.Timeline().clips.end(),
      [clipId](const auto &clip) { return clip.id == clipId; });
   if (found == track.Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
   if (edit) {
      const auto *content = VideoTimeline::FindEditableVideoTextContent(*found);
      if (!content)
         throw std::runtime_error("Select a text overlay clip.");
      value.text = content->title; value.appearance = content->textAppearance;
      value.start = found->start; value.seconds = found->duration; value.rectangle = track.Timeline().placement.rectangle;
   }
   else { value.text = std::string(_("Your caption").ToUTF8());
      const auto frame = 1 / VideoTimeline::VideoContentFrameRate(*found, VideoTimeline::SourceFor(track.Timeline(), *found));
      value.start = std::clamp(at, found->start, std::max(found->start, found->End() - std::min(frame, found->duration)));
      value.seconds = std::min(5.0, found->End() - value.start); }
   wxDialog dialog(parent, wxID_ANY, edit ? _("Edit Text Overlay") : _("Add Text Overlay"),
      wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
   auto *layout = new wxBoxSizer(wxVERTICAL);
   auto *text = new wxTextCtrl(&dialog, wxID_ANY, wxString::FromUTF8(value.text), wxDefaultPosition,
      wxSize(480, 90), wxTE_MULTILINE); text->SetName("Caption text");
   layout->Add(new wxStaticText(&dialog, wxID_ANY, _("Text (line breaks are supported):")), 0, wxALL, 6);
   layout->Add(text, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);
   auto *fields = new wxFlexGridSizer(2, 6, 8); fields->AddGrowableCol(1);
   const auto label = [&](const wxString &name, wxWindow *control) {
      fields->Add(new wxStaticText(&dialog, wxID_ANY, name), 0, wxALIGN_CENTER_VERTICAL);
      fields->Add(control, 1, wxEXPAND);
   };
   auto fonts = wxFontEnumerator::GetFacenames(); fonts.Sort(); fonts.Insert(_("Default sans-serif"), 0);
   if (!value.appearance.fontFace.empty() && fonts.Index(wxString::FromUTF8(value.appearance.fontFace)) == wxNOT_FOUND)
      fonts.Add(wxString::FromUTF8(value.appearance.fontFace));
   auto *font = new wxComboBox(&dialog, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, fonts, wxCB_READONLY);
   font->SetSelection(value.appearance.fontFace.empty() ? 0 : fonts.Index(wxString::FromUTF8(value.appearance.fontFace)));
   label(_("Font:"), font);
   const auto spin = [&](const wxString &name, double initial, double low, double high, int digits) {
      auto *control = new wxSpinCtrlDouble(&dialog, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
         wxSP_ARROW_KEYS, low, high, initial, digits ? .1 : 1); control->SetDigits(digits); control->SetName(name);
      label(name, control); return control;
   };
   auto *size = spin(_("Text size (% of box height):"), value.appearance.heightFraction * 100, 1, 90, 1);
   auto *colour = new wxColourPickerCtrl(&dialog, wxID_ANY, wxColour((value.appearance.rgb >> 16) & 255,
      (value.appearance.rgb >> 8) & 255, value.appearance.rgb & 255)); label(_("Colour:"), colour);
   auto *styles = new wxPanel(&dialog); auto *styleLayout = new wxBoxSizer(wxHORIZONTAL);
   auto *bold = new wxCheckBox(styles, wxID_ANY, _("Bold")); bold->SetValue(value.appearance.bold);
   auto *italic = new wxCheckBox(styles, wxID_ANY, _("Italic")); italic->SetValue(value.appearance.italic);
   auto *outline = new wxCheckBox(styles, wxID_ANY, _("Black outline")); outline->SetValue(value.appearance.outline);
   for (auto *control : {bold, italic, outline}) styleLayout->Add(control, 0, wxRIGHT, 8);
   styles->SetSizer(styleLayout); label(_("Style:"), styles);
   auto *start = spin(_("Start (project seconds):"), value.start, 0, 100000000, 4);
   auto *seconds = spin(_("Duration (seconds):"), value.seconds, .001, 3600, 4);
   auto *x = spin(_("Box left (%):"), value.rectangle.x * 100, 0, 100, 1);
   auto *y = spin(_("Box top (%):"), value.rectangle.y * 100, 0, 100, 1);
   auto *width = spin(_("Box width (%):"), value.rectangle.width * 100, .1, 100, 1);
   auto *height = spin(_("Box height (%):"), value.rectangle.height * 100, .1, 100, 1);
   layout->Add(fields, 0, wxEXPAND | wxALL, 8);
   layout->Add(new wxStaticText(&dialog, wxID_ANY,
      _("The background stays transparent. The embedded text track follows its parent video.\n"
        "Resize or move its box with Picture-in-Picture; placement applies to the text track.")), 0, wxALL, 8);
   layout->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALIGN_RIGHT | wxALL, 8);
   dialog.SetSizerAndFit(layout); dialog.SetMinSize(dialog.GetSize()); text->SetFocus(); text->SelectAll();
   while (dialog.ShowModal() == wxID_OK) {
      value.text = std::string(text->GetValue().ToUTF8()); value.appearance.fontFace = font->GetSelection() == 0 ?
         std::string{} : std::string(font->GetValue().ToUTF8());
      value.appearance.heightFraction = size->GetValue() / 100;
      const auto selected = colour->GetColour();
      value.appearance.rgb = (uint32_t(selected.Red()) << 16) | (uint32_t(selected.Green()) << 8) | selected.Blue();
      value.appearance.bold = bold->GetValue(); value.appearance.italic = italic->GetValue(); value.appearance.outline = outline->GetValue();
      value.start = start->GetValue(); value.seconds = seconds->GetValue();
      value.rectangle = {x->GetValue()/100, y->GetValue()/100, width->GetValue()/100, height->GetValue()/100};
      try {
         if (edit) ChangeProjectVideoTextOverlay(project, track, clipId, value);
         else AddProjectVideoTextOverlay(project, track, value);
         return true;
      }
      catch (const VideoPreview::PreviewPreparationCancelled &) { return false; }
      catch (const std::exception &error) { wxMessageBox(wxString::FromUTF8(error.what()), _("Text Overlay"), wxOK | wxICON_ERROR, &dialog); }
   }
   return false;
}
}
