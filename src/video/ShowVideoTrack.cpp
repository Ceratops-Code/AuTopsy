// The video strip uses Audacity's public viewport coordinates and a semantic
// ruler anchor. It neither masquerades as an audio track nor modifies its model.
#include "ShowVideoTrack.h"
#include "EditVideoTimeline.h"
#include "Project.h"
#include "ProjectWindow.h"
#include "ProjectAudioIO.h"
#include "ProjectAudioManager.h"
#include "AudioIO.h"
#include "AdornedRulerPanel.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include <wx/dcbuffer.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include <algorithm>
#include <cmath>
#include <optional>

namespace VideoTimeline {
namespace {
class VideoTrackPanel final : public wxPanel {
   std::weak_ptr<AudacityProject> mProject;
   wxTimer mTimer{this};
   std::shared_ptr<const VideoPreview::PreparedMedia> mMedia;
   wxImage mSheet;
   std::vector<wxBitmap> mThumbnails;
   uint64_t mSelected{};
   struct Drag { uint64_t id; double start, mouseTime; };
   std::optional<Drag> mDrag;
   double mDragStart{};
   double mLastView{-1}, mLastZoom{-1}, mLastCursor{-1};
   static constexpr int CellWidth = 112, CellHeight = 63;

   template<typename Operation> void Edit(Operation operation) {
      if (const auto project = mProject.lock()) try {
         if (!ProjectAudioIO::Get(*project).IsAudioActive()) operation(*project);
      }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Video Track"), wxOK | wxICON_ERROR, this);
      }
      Refresh(false);
   }
   double Cursor(AudacityProject &project) const {
      auto *audio = AudioIO::Get();
      return audio && ProjectAudioIO::Get(project).IsAudioActive() &&
         ProjectAudioManager::Get(project).Playing() ? audio->GetStreamTime() :
         ViewInfo::Get(project).selectedRegion.t0();
   }
   void Paint(wxPaintEvent &) {
      wxAutoBufferedPaintDC dc(this);
      dc.SetBackground(wxBrush(wxColour(29, 31, 35))); dc.Clear();
      const auto project = mProject.lock(); if (!project) return;
      const auto &timeline = GetProjectTimeline(*project);
      const auto &view = ViewInfo::Get(*project);
      const int left = view.GetLeftOffset();
      const int right = std::min(GetClientSize().x, left + view.GetTracksUsableWidth());
      dc.SetClippingRegion(0, 0, left, GetClientSize().y);
      dc.SetTextForeground(*wxWHITE); dc.DrawText(_("VIDEO"), 9, 8);
      dc.DrawText(wxFileName(wxString::FromUTF8(timeline.sourcePath)).GetFullName(), 9, 31);
      dc.SetTextForeground(wxColour(155, 180, 203));
      dc.DrawText(mMedia && mMedia->lightweight ? _("540p preview") : _("Original preview"), 9, 54);
      dc.DrawText(_("Double-click for clips"), 9, 76);
      dc.DestroyClippingRegion();
      if (right <= left) return;
      const auto x = [&](double time) {
         return int(std::clamp<int64_t>(view.TimeToPosition(time, left), -1000000, 1000000));
      };
      dc.SetClippingRegion(left, 0, right - left, GetClientSize().y);
      for (const auto &clip : timeline.clips) {
         const auto start = mDrag && mDrag->id == clip.id ? mDragStart : clip.start;
         const int from = x(start), to = x(start + clip.duration);
         if (to <= left || from >= right) continue;
         const int visibleLeft = std::max(left, from), visibleRight = std::min(right, to);
         dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(wxColour(60, 107, 143)));
         dc.DrawRectangle(visibleLeft, 3, visibleRight - visibleLeft, 91);
         dc.SetClippingRegion(visibleLeft, 24, visibleRight - visibleLeft, CellHeight);
         int cell = from + int(std::floor(double(visibleLeft - from) / CellWidth)) * CellWidth;
         for (; cell < visibleRight; cell += CellWidth) {
            if (!mThumbnails.empty()) {
               const double projectTime = view.PositionToTime(cell + CellWidth / 2, left);
               const double sourceTime = clip.sourceStart + std::clamp(projectTime - start, 0.0, clip.duration);
               const auto index = std::clamp(int(sourceTime / timeline.sourceDuration * mThumbnails.size()),
                  0, int(mThumbnails.size()) - 1);
               dc.DrawBitmap(mThumbnails[index], cell, 24);
            }
            dc.SetPen(wxPen(wxColour(22, 23, 26))); dc.DrawLine(cell, 24, cell, 87);
         }
         dc.DestroyClippingRegion();
         dc.SetClippingRegion(left, 0, right - left, GetClientSize().y);
         dc.SetTextForeground(*wxWHITE);
         dc.DrawText(wxString::Format("#%llu  %.3f s", static_cast<unsigned long long>(clip.id), start),
            visibleLeft + 4, 5);
         dc.SetBrush(*wxTRANSPARENT_BRUSH);
         dc.SetPen(wxPen(clip.id == mSelected ? wxColour(252, 211, 90) : wxColour(93, 143, 183),
            clip.id == mSelected ? 2 : 1));
         dc.DrawRectangle(visibleLeft, 3, visibleRight - visibleLeft, 91);
      }
      dc.SetPen(wxPen(wxColour(240, 92, 73), 2));
      dc.DrawLine(x(Cursor(*project)), 0, x(Cursor(*project)), GetClientSize().y);
   }
   void MouseDown(wxMouseEvent &event) {
      const auto project = mProject.lock(); if (!project) return;
      if (ProjectAudioIO::Get(*project).IsAudioActive()) return;
      const auto &view = ViewInfo::Get(*project);
      if (event.GetX() < view.GetLeftOffset()) return;
      const double time = std::max(0.0, view.PositionToTime(event.GetX(), view.GetLeftOffset()));
      ViewInfo::Get(*project).selectedRegion.setTimes(time, time);
      const auto *clip = ClipAt(GetProjectTimeline(*project), time);
      mSelected = clip ? clip->id : 0;
      if (clip) { mDrag = Drag{clip->id, clip->start, time}; mDragStart = clip->start; CaptureMouse(); }
      SetFocus(); Viewport::Get(*project).Redraw(); Refresh(false);
   }
   void MouseMove(wxMouseEvent &event) {
      if (!mDrag || !event.Dragging()) return;
      if (const auto project = mProject.lock()) {
         const auto &view = ViewInfo::Get(*project);
         const auto &timeline = GetProjectTimeline(*project);
         const auto time = view.PositionToTime(event.GetX(), view.GetLeftOffset());
         mDragStart = std::max(0.0, std::round((mDrag->start + time - mDrag->mouseTime) *
            timeline.frameRate) / timeline.frameRate);
         Refresh(false);
      }
   }
   void MouseUp(wxMouseEvent &) {
      const auto drag = mDrag; mDrag.reset();
      if (HasCapture()) ReleaseMouse();
      if (drag) Edit([&](auto &project) { MoveProjectVideoClip(project, drag->id, mDragStart); });
   }
public:
   explicit VideoTrackPanel(AudacityProject &project, wxWindow &parent)
      : wxPanel(&parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 100), wxWANTS_CHARS),
        mProject(project.shared_from_this()) {
      SetName("AuTopsy Video Track"); SetLabel(_("Video Track"));
      SetMinSize(wxSize(-1, 100)); SetBackgroundStyle(wxBG_STYLE_PAINT);
      SetToolTip(_("Video picture is separate from its audio. Drag a clip to move it; right-click for editing."));
      Bind(wxEVT_PAINT, &VideoTrackPanel::Paint, this);
      Bind(wxEVT_LEFT_DOWN, &VideoTrackPanel::MouseDown, this);
      Bind(wxEVT_MOTION, &VideoTrackPanel::MouseMove, this);
      Bind(wxEVT_LEFT_UP, &VideoTrackPanel::MouseUp, this);
      Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { mDrag.reset(); Refresh(false); });
      Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent &) {
         mDrag.reset(); if (HasCapture()) ReleaseMouse();
         Edit([](auto &project) { ShowProjectVideoClips(project); });
      });
      Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) {
         if (event.GetKeyCode() == WXK_DELETE && mSelected)
            Edit([&](auto &project) { RemoveProjectVideoClip(project, mSelected); });
         else event.Skip();
      });
      Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent &event) {
         const auto project = mProject.lock(); if (!project || ProjectAudioIO::Get(*project).IsAudioActive()) return;
         const auto &view = ViewInfo::Get(*project);
         if (event.GetX() >= view.GetLeftOffset()) {
            const auto time = std::max(0.0, view.PositionToTime(event.GetX(), view.GetLeftOffset()));
            ViewInfo::Get(*project).selectedRegion.setTimes(time, time);
            const auto *clip = ClipAt(GetProjectTimeline(*project), time); mSelected = clip ? clip->id : 0;
         }
         wxMenu menu;
         auto *split = menu.Append(wxID_ANY, _("Split Video at Cursor"));
         auto *remove = menu.Append(wxID_ANY, _("Delete Selected Video Clip")); remove->Enable(mSelected != 0);
         auto *clips = menu.Append(wxID_ANY, _("Video Clips..."));
         menu.Bind(wxEVT_MENU, [&](wxCommandEvent &) { Edit([](auto &p) {
            SplitProjectVideo(p, ViewInfo::Get(p).selectedRegion.t0()); }); }, split->GetId());
         menu.Bind(wxEVT_MENU, [&](wxCommandEvent &) { Edit([&](auto &p) {
            RemoveProjectVideoClip(p, mSelected); }); }, remove->GetId());
         menu.Bind(wxEVT_MENU, [&](wxCommandEvent &) { Edit([](auto &p) { ShowProjectVideoClips(p); }); }, clips->GetId());
         PopupMenu(&menu); Viewport::Get(*project).Redraw(); Refresh(false);
      });
      Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
         const auto project = mProject.lock(); if (!project || ProjectWindow::Get(*project).IsBeingDeleted()) { mTimer.Stop(); return; }
         const auto &view = ViewInfo::Get(*project);
         const double cursor = Cursor(*project);
         if (mLastView != view.hpos || mLastZoom != view.GetZoom() || mLastCursor != cursor) {
            mLastView = view.hpos; mLastZoom = view.GetZoom(); mLastCursor = cursor; Refresh(false);
         }
      }, mTimer.GetId());
   }
   ~VideoTrackPanel() override { mTimer.Stop(); }
   void Update(std::shared_ptr<const VideoPreview::PreparedMedia> media) {
      const auto project = mProject.lock(); if (!project) return;
      const auto &timeline = GetProjectTimeline(*project);
      if (mMedia && mMedia->sourcePath != wxString::FromUTF8(timeline.sourcePath)) {
         mMedia.reset(); mSheet.Destroy(); mThumbnails.clear();
      }
      if (media && media != mMedia && media->sourcePath == wxString::FromUTF8(timeline.sourcePath)) {
         wxImage sheet(media->contactSheet, wxBITMAP_TYPE_JPEG);
         if (!sheet.IsOk() || sheet.GetWidth() != VideoPreview::ThumbnailColumns * VideoPreview::ThumbnailWidth ||
             sheet.GetHeight() != VideoPreview::ThumbnailCount / VideoPreview::ThumbnailColumns * VideoPreview::ThumbnailHeight)
            throw std::runtime_error("The video thumbnail sheet is incomplete.");
         mSheet = std::move(sheet); mThumbnails.clear();
         for (int i = 0; i < VideoPreview::ThumbnailCount; ++i) {
            auto frame = mSheet.GetSubImage(wxRect(i % 16 * 160, i / 16 * 90, 160, 90));
            mThumbnails.emplace_back(frame.Scale(CellWidth, CellHeight, wxIMAGE_QUALITY_NORMAL));
         }
         mMedia = std::move(media);
      }
      if (timeline.sourcePath.empty()) {
         mMedia.reset(); mSheet.Destroy(); mThumbnails.clear(); Hide(); mTimer.Stop();
      }
      else { Show(); mTimer.Start(40); }
      GetParent()->Layout(); Refresh(false);
   }
   VideoTrackStatus Status() const {
      const auto project = mProject.lock(); if (!project) return {};
      const auto &view = ViewInfo::Get(*project);
      return {IsShown(), !mThumbnails.empty(), view.GetLeftOffset(), GetClientSize().x, view.GetTracksUsableWidth(),
         view.hpos, view.GetScreenEndTime()};
   }
};
class ProjectVideoTrack final : public ClientData::Base {
public: wxWeakRef<VideoTrackPanel> panel;
};
const AudacityProject::AttachedObjects::RegisteredFactory sVideoTrack{
   [](AudacityProject &) { return std::make_shared<ProjectVideoTrack>(); }
};
ProjectVideoTrack &TrackFor(AudacityProject &project) {
   return project.AttachedObjects::Get<ProjectVideoTrack>(sVideoTrack);
}
}
void ShowProjectVideoTrack(AudacityProject &project,
   std::shared_ptr<const VideoPreview::PreparedMedia> media)
{
   auto &track = TrackFor(project);
   if (!track.panel && GetProjectTimeline(project).sourcePath.empty()) return;
   if (!track.panel) {
      auto *parent = ProjectWindow::Get(project).GetTrackListWindow();
      auto *layout = dynamic_cast<wxBoxSizer *>(parent->GetSizer());
      if (!layout || layout->GetOrientation() != wxVERTICAL)
         throw std::runtime_error("The audio timeline container no longer supports a video strip.");
      const auto *ruler = &AdornedRulerPanel::Get(project);
      size_t index{}, anchor{}, matches{};
      for (const auto &item : layout->GetChildren()) {
         if (item->GetWindow() == ruler) { anchor = index; ++matches; }
         ++index;
      }
      if (matches != 1) throw std::runtime_error("The project timeline ruler is missing or ambiguous.");
      track.panel = new VideoTrackPanel(project, *parent);
      layout->Insert(anchor + 1, track.panel.get(), 0, wxEXPAND);
   }
   track.panel->Update(std::move(media));
}
VideoTrackStatus GetProjectVideoTrackStatus(AudacityProject &project)
{ const auto &track = TrackFor(project); return track.panel ? track.panel->Status() : VideoTrackStatus{}; }
}
