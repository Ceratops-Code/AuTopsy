// Native TrackPanel adapters. The host owns scrolling, sizing, focus, track
// order and Undo. There is no separate video panel or copied track list.
#include "ShowVideoTrack.h"
#include "CreateVideoTrack.h"
#include "../clip-groups/LinkMediaClips.h"
#include "../clip-groups/GroupAudioClips.h"
#include "Project.h"
#include "ProjectHistory.h"
#include "ProjectAudioIO.h"
#include "ProjectWindow.h"
#include "TrackPanel.h"
#include "TrackPanelMouseEvent.h"
#include "TrackPanelDrawingContext.h"
#include "TrackArtist.h"
#include "TrackFocus.h"
#include "ViewInfo.h"
#include "Viewport.h"
#include "SelectUtilities.h"
#include "HitTestResult.h"
#include "RefreshCode.h"
#include "CommandManager.h"
#include "CommandContext.h"
#include "CommonCommandFlags.h"
#include "Clipboard.h"
#include "tracks/ui/CommonChannelView.h"
#include "tracks/ui/CommonTrackControls.h"
#include "tracks/ui/ChannelVRulerControls.h"
#include "tracks/ui/TimeShiftHandle.h"
#include "tracks/ui/AffordanceHandle.h"
#include "tracks/ui/SelectHandle.h"
#include "widgets/PopupMenuTable.h"
#include <wx/dc.h>
#include <wx/bitmap.h>
#include <wx/cursor.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <unordered_set>

namespace {
using namespace VideoTimeline;
struct ShiftInterval final : ChannelGroupInterval, ClipGroups::LinkedClipInterval {
   Clip clip;
   explicit ShiftInterval(Clip value) : clip(std::move(value)) {}
   double Start() const override { return clip.start; }
   double End() const override { return clip.End(); }
   const std::string &MediaGroupId() const override { return clip.linkGroup; }
   unsigned long long VideoClipId() const override { return clip.id; }
};

// The existing move engine supplies snapping, multi-track selection, scrolling,
// vertical migration and Escape rollback. This policy supplies video intervals,
// source-frame quantization and free-space constraints.
class VideoTrackShifter final : public TrackShifter {
   std::shared_ptr<AuTopsyVideoTrack> mTrack;
   static ShiftInterval &Data(const std::shared_ptr<ChannelGroupInterval> &value)
   { return static_cast<ShiftInterval&>(*value); }
public:
   explicit VideoTrackShifter(AuTopsyVideoTrack &track)
      : mTrack(track.SharedPointer<AuTopsyVideoTrack>()) {
      for (const auto &clip : track.Timeline().clips) {
         auto value = clip; value.media = SourceFor(track.Timeline(), clip);
         mFixed.push_back(std::make_shared<ShiftInterval>(std::move(value)));
      }
   }
   Track &GetTrack() const override { return *mTrack; }
   bool SyncLocks() override { return true; }
   HitTestResult HitTest(double time, const ViewInfo &view, HitTestParams *) override {
      const auto *clip = ClipAt(mTrack->Timeline(), time);
      if (!clip) return HitTestResult::Miss;
      const auto &selection = view.selectedRegion;
      if (mTrack->GetSelected() && selection.t1() > selection.t0() &&
          time >= selection.t0() && time < selection.t1()) {
         SelectInterval({selection.t0(), selection.t1()});
         return HitTestResult::Selection;
      }
       // Like the audio shifter, choose intervals without changing project
       // focus or selection inside the native move engine's hit test.
      const auto id = clip->id;
      UnfixIntervals([id](const auto &interval) {
         return static_cast<const ShiftInterval&>(interval).clip.id == id;
      });
      return HitTestResult::Intervals;
   }
   void SelectInterval(TimeInterval interval) override {
      // Touching endpoints are not an overlap: selecting one split clip must
      // not capture its immediate neighbor in the native move engine.
      UnfixIntervals([&](const auto &clip) {
         return clip.End() > interval.Start() + 1e-7 &&
            clip.Start() < interval.End() - 1e-7;
      });
   }
   double QuantizeOffset(double offset) override {
      if (mMoving.empty()) return offset;
      const auto rate = Data(mMoving.front()).clip.media.frameRate;
      return std::round(offset * rate) / rate;
   }
   double AdjustOffsetSmaller(double offset) override {
      for (const auto &moving : mMoving) {
         if (offset < 0) offset = std::max(offset, -moving->Start());
         for (const auto &fixed : mFixed) {
            if (offset > 0 && moving->End() <= fixed->Start() + 1e-7)
               offset = std::min(offset, std::max(0.0, fixed->Start() - moving->End()));
            else if (offset < 0 && moving->Start() >= fixed->End() - 1e-7)
               offset = std::max(offset, std::min(0.0, fixed->End() - moving->Start()));
         }
      }
      return offset;
   }
   void DoHorizontalOffset(double offset) override {
      if (mMoving.empty() || std::abs(offset) < 1e-10) return;
      auto changed = mTrack->Timeline();
      for (auto &clip : changed.clips)
         if (std::any_of(mMoving.begin(), mMoving.end(),
               [&](const auto &interval) { return Data(interval).clip.id == clip.id; }))
            clip.start += offset;
      std::sort(changed.clips.begin(), changed.clips.end(),
         [](const auto &a, const auto &b) { return a.start < b.start; });
      mTrack->SetTimeline(std::move(changed));
      for (auto &interval : mMoving) Data(interval).clip.start += offset;
   }
   bool MayMigrateTo(Track &other) override
   { return dynamic_cast<AuTopsyVideoTrack*>(&other) != nullptr; }
   Intervals Detach() override {
      auto changed = mTrack->Timeline();
      for (const auto &interval : mMoving) RemoveClip(changed, Data(interval).clip.id);
      mTrack->SetTimeline(std::move(changed));
      auto result = std::move(mMoving); mMoving.clear(); return result;
   }
   bool AdjustFit(const Track &destination, const Intervals &intervals, double &offset, double) override {
       const auto *videoDestination = dynamic_cast<const AuTopsyVideoTrack*>(&destination);
       if (!videoDestination) return false;
       if (intervals.empty()) return true;
       // The native engine calls the source policy with the destination as an
       // argument. Its hovered destination has no moving intervals yet, so
       // finish source-frame snapping here before checking destination space.
       const auto rate = Data(intervals.front()).clip.media.frameRate;
       const auto fittedOffset = std::round(offset * rate) / rate;
       auto candidate = videoDestination->Timeline();
       for (const auto &interval : intervals) {
          auto clip = Data(interval).clip; clip.id = candidate.nextId++; clip.start += fittedOffset;
         candidate.clips.push_back(std::move(clip));
      }
      std::sort(candidate.clips.begin(), candidate.clips.end(),
         [](const auto &a, const auto &b) { return a.start < b.start; });
       try { RequireWellFormedTimeline(candidate); offset = fittedOffset; return true; } catch (...) { return false; }
   }
   bool Attach(Intervals intervals, double offset) override {
      auto candidate = mTrack->Timeline();
      std::unordered_set<uint64_t> ids;
      for (const auto &clip : candidate.clips) ids.insert(clip.id);
      Intervals attached;
      for (auto &interval : intervals) {
         auto clip = Data(interval).clip; clip.start += offset;
         if (ids.count(clip.id)) clip.id = candidate.nextId++;
         candidate.nextId = std::max(candidate.nextId, clip.id + 1); ids.insert(clip.id);
         candidate.clips.push_back(clip);
         attached.push_back(std::make_shared<ShiftInterval>(std::move(clip)));
      }
      std::sort(candidate.clips.begin(), candidate.clips.end(),
         [](const auto &a, const auto &b) { return a.start < b.start; });
      RequireWellFormedTimeline(candidate); mTrack->SetTimeline(std::move(candidate));
      mMoving.insert(mMoving.end(), attached.begin(), attached.end());
      return true;
   }
};
}
using MakeVideoShifter = MakeTrackShifter::Override<AuTopsyVideoTrack>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(MakeVideoShifter) {
    return [](AuTopsyVideoTrack &track, AudacityProject &) {
       return std::make_unique<VideoTrackShifter>(track);
   };
}

namespace {
class VideoRuler final : public ChannelVRulerControls {
public:
   using ChannelVRulerControls::ChannelVRulerControls;
   void UpdateRuler(const wxRect &) override {}
};
// Header clicks use the same click-versus-drag contract as audio clips.
class VideoAffordanceHandle final : public AffordanceHandle {
public:
   using AffordanceHandle::AffordanceHandle;
   Result SelectAt(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      const auto track = std::dynamic_pointer_cast<AuTopsyVideoTrack>(GetTrack());
      if (!track || !project) return RefreshCode::RefreshNone;
      auto &view = ViewInfo::Get(*project);
      if (const auto *clip = ClipAt(track->Timeline(),
            view.PositionToTime(event.event.GetX(), event.rect.x))) {
         view.selectedRegion.setTimes(clip->start, clip->End());
         TrackFocus::Get(*project).Set(track.get(), true);
         ProjectHistory::Get(*project).ModifyState(false);
      }
      return RefreshCode::RefreshAll;
   }
};

// Picture clicks select one clip. Actual drags still use the host's range,
// snapping, modifier keys, autoscroll and Escape handling.
class VideoSelectionHandle final : public SelectHandle {
   std::weak_ptr<AuTopsyVideoTrack> mTrack;
   wxPoint mClick;
   uint64_t mId{};
   bool mDragged{};
   Result SelectClip(AudacityProject *project) {
      const auto track = mTrack.lock();
      if (!track || !mId) return RefreshCode::RefreshNone;
      for (const auto &clip : track->Timeline().clips) if (clip.id == mId) {
         ViewInfo::Get(*project).selectedRegion.setTimes(clip.start, clip.End());
         TrackFocus::Get(*project).Set(track.get(), true);
         ProjectHistory::Get(*project).ModifyState(false);
         return RefreshCode::RefreshAll;
      }
      return RefreshCode::RefreshNone;
   }
public:
   VideoSelectionHandle(const std::shared_ptr<ChannelView> &view,
      const std::shared_ptr<AuTopsyVideoTrack> &track,
      const TrackPanelMouseState &state, const AudacityProject &project)
      : SelectHandle(view, true, TrackList::Get(project), state, ViewInfo::Get(project)),
        mTrack(track) {}
   Result Click(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      mClick = event.event.GetPosition(); mDragged = false; mId = 0;
      if (!event.event.ShiftDown() && !event.event.CmdDown())
         if (const auto track = mTrack.lock())
            if (const auto *clip = ClipAt(track->Timeline(),
                  ViewInfo::Get(*project).PositionToTime(mClick.x, event.rect.x)))
               mId = clip->id;
      const auto result = SelectHandle::Click(event, project);
      return event.event.LeftDClick() ? result | SelectClip(project) : result;
   }
   Result Drag(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      mDragged = mDragged || std::abs(event.event.GetX() - mClick.x) >= 3 ||
         std::abs(event.event.GetY() - mClick.y) >= 3;
      return mDragged ? SelectHandle::Drag(event, project) : RefreshCode::RefreshNone;
   }
   Result Release(const TrackPanelMouseEvent &event, AudacityProject *project, wxWindow *parent) override {
      const auto result = SelectHandle::Release(event, project, parent);
      return mDragged ? result : result | SelectClip(project);
   }
};
class VideoTrackView;
class VideoClipHandle final : public UIHandle {
   std::weak_ptr<AuTopsyVideoTrack> mTrack;
   std::weak_ptr<VideoTrackView> mView;
   uint64_t mId;
   int mEdge;
   bool mContext{};
   int mOrigin{};
   double mClickTime{};
   std::optional<Timeline> mCandidate;
   Timeline mOriginal;
public:
   VideoClipHandle(const std::shared_ptr<AuTopsyVideoTrack> &track,
      const std::shared_ptr<VideoTrackView> &view, uint64_t id, int edge)
      : mTrack(track), mView(view), mId(id), mEdge(edge) {}
   bool HandlesRightClick() override { return true; }
   std::shared_ptr<const Track> FindTrack() const override { return mTrack.lock(); }
   Result Click(const TrackPanelMouseEvent &, AudacityProject *) override;
   Result Drag(const TrackPanelMouseEvent &, AudacityProject *) override;
   Result Release(const TrackPanelMouseEvent &, AudacityProject *, wxWindow *) override;
   Result Cancel(AudacityProject *) override;
   HitTestPreview Preview(const TrackPanelMouseState &, AudacityProject *) override {
      static wxCursor edge(wxCURSOR_SIZEWE), arrow(wxCURSOR_ARROW);
      return {mEdge ? XO("Drag to trim the video clip edge") : XO("Video clip commands"),
         mEdge ? &edge : &arrow};
   }
};

class VideoTrackView final : public CommonChannelView {
   struct Thumbnails {
      std::shared_ptr<const VideoPreview::PreparedMedia> media;
      std::vector<wxBitmap> frames;
      uint64_t used{};
   };
   std::map<std::string, Thumbnails> mSources;
   uint64_t mUse{};
   std::weak_ptr<VideoClipHandle> mClipHandle;
   std::weak_ptr<VideoAffordanceHandle> mAffordanceHandle;
   std::weak_ptr<VideoSelectionHandle> mVideoSelectionHandle;
   std::optional<Timeline> mGhost;
   wxRect mLastRect;
   static constexpr int FrameWidth = 112, FrameHeight = 63;
   Thumbnails &Frames(const MediaSource &source, wxWindow *parent) {
      auto found = mSources.find(source.path);
      if (found == mSources.end()) {
         Thumbnails data;
         try {
         data.media = VideoPreview::PrepareVideoMedia(wxString::FromUTF8(source.path),
            source.duration, true, parent, false);
         if (!data.media) data.media = VideoPreview::PrepareVideoMedia(wxString::FromUTF8(source.path),
             source.duration, false, parent, false);
         }
         catch (const std::exception &) {
            // Offline media or an unavailable cache must not throw from a
            // native paint callback. Keep the clip and its filename editable.
         }
         if (data.media) {
            wxImage sheet;
            if (sheet.LoadFile(data.media->contactSheet, wxBITMAP_TYPE_JPEG)) {
               const int columns = sheet.GetWidth()/160, rows = sheet.GetHeight()/90;
               for (int i = 0; i < columns*rows; ++i)
                  data.frames.emplace_back(sheet.GetSubImage(wxRect((i%columns)*160,(i/columns)*90,160,90))
                     .Scale(FrameWidth,FrameHeight,wxIMAGE_QUALITY_NORMAL));
            }
         }
         // Bound decoded images; the preparation component owns disk retention.
         if (mSources.size() >= 4) {
            const auto oldest = std::min_element(mSources.begin(),mSources.end(),
               [](const auto &a,const auto &b) { return a.second.used < b.second.used; });
            mSources.erase(oldest);
         }
         found = mSources.emplace(source.path,std::move(data)).first;
      }
      found->second.used = ++mUse; return found->second;
   }
public:
   explicit VideoTrackView(const std::shared_ptr<Channel> &channel) : CommonChannelView(channel) {}
   const wxRect &DataRect() const { return mLastRect; }
   bool HasThumbnails() const {
      return std::any_of(mSources.begin(),mSources.end(),
         [](const auto &source) { return !source.second.frames.empty(); });
   }
   void SetGhost(std::optional<Timeline> value) { mGhost = std::move(value); }
   std::shared_ptr<ChannelVRulerControls> DoGetVRulerControls() override
   { return std::make_shared<VideoRuler>(shared_from_this()); }
   std::vector<UIHandlePtr> DetailedHitTest(const TrackPanelMouseState &state,
      const AudacityProject *project,int,bool) override {
      const auto track = std::dynamic_pointer_cast<AuTopsyVideoTrack>(FindTrack());
      if (!track || !project) return {};
      const auto &view = ViewInfo::Get(*project);
      const auto pointer = state.state.GetX();
      const Clip *clip{};
      // Painting rounds each boundary to a pixel. Converting that pixel back
      // to time can fall before a split and select the other clip's edge.
      // Use the same half-open pixel ranges as Draw, so the shared seam
      // belongs to the right clip even at fractional zoom and scroll offsets.
      for (const auto &candidate : track->Timeline().clips)
         if (view.TimeToPosition(candidate.start, state.rect.x) <= pointer &&
             pointer < view.TimeToPosition(candidate.End(), state.rect.x)) {
            clip = &candidate;
            break;
         }
      if (state.state.RightIsDown())
         return {AssignUIHandlePtr(mClipHandle, std::make_shared<VideoClipHandle>(
            track, std::static_pointer_cast<VideoTrackView>(shared_from_this()),
            clip ? clip->id : 0, 0))};
      int edge{};
      int distance = 6;
      const auto consider = [&](const Clip &candidate, double at, int side) {
         const auto pixels = int(std::abs(view.TimeToPosition(at, state.rect.x) - state.state.GetX()));
         // At a shared boundary prefer the clip containing the pointer.
         if (pixels < distance || (pixels == distance && clip == &candidate)) {
            distance = pixels; clip = &candidate; edge = side;
         }
      };
      const auto *underPointer = clip;
      for (const auto &candidate : track->Timeline().clips) {
         if (underPointer && &candidate != underPointer) continue;
         consider(candidate, candidate.start, -1); consider(candidate, candidate.End(), 1);
      }
      if (std::getenv("AUTOPSY_TEST_VIDEO_INPUT") && state.state.LeftIsDown())
         std::fprintf(stderr, "AUTOPSY_VIDEO_HIT pointer=%d origin=%d paintedOrigin=%d clip=%llu edge=%d distance=%d\n",
            pointer, state.rect.x, mLastRect.x, clip ? static_cast<unsigned long long>(clip->id) : 0,
            edge, distance);
      if (edge)
         return {AssignUIHandlePtr(mClipHandle, std::make_shared<VideoClipHandle>(
            track, std::static_pointer_cast<VideoTrackView>(shared_from_this()), clip->id, edge))};
      if (clip && state.state.GetY() < state.rect.y+24)
         return {AssignUIHandlePtr(mAffordanceHandle,
            std::make_shared<VideoAffordanceHandle>(track))};
      return {AssignUIHandlePtr(mVideoSelectionHandle,
         std::make_shared<VideoSelectionHandle>(shared_from_this(), track, state, *project))};
   }
   void Draw(TrackPanelDrawingContext &context,const wxRect &rect,unsigned pass) override {
      if (pass != TrackArtist::PassTracks) { CommonChannelView::Draw(context,rect,pass); return; }
      mLastRect = rect;
      const auto track = std::dynamic_pointer_cast<AuTopsyVideoTrack>(FindTrack());
      if (!track) return;
      const auto owner = track->GetOwner();
      const auto project = owner ? owner->GetOwner() : nullptr;
      if (!project) return;
      const auto &view = ViewInfo::Get(*project);
      const auto &timeline = mGhost ? *mGhost : track->Timeline();
      auto &dc = context.dc;
      dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(wxColour(29,31,35)));
      dc.DrawRectangle(rect);
      const auto x = [&](double time) {
         return int(std::clamp<int64_t>(view.TimeToPosition(time,rect.x),-1000000,1000000));
      };
      for (const auto &clip : timeline.clips) {
         const int from = x(clip.start),to = x(clip.End());
         const auto left = std::max(rect.x,from),right = std::min(rect.GetRight()+1,to);
         if (right <= left) continue;
         const auto source = SourceFor(timeline,clip);
         dc.SetClippingRegion(left,rect.y,right-left,rect.height);
         dc.SetPen(*wxTRANSPARENT_PEN);
         dc.SetBrush(wxBrush(track->VideoVisible() ? wxColour(57,105,143) : wxColour(69,72,77)));
         dc.DrawRectangle(left,rect.y+2,right-left,rect.height-4);
         auto &frames = Frames(source,&ProjectWindow::Get(*project)).frames;
         const int pictureHeight = std::max(0,std::min(FrameHeight,rect.height-30));
         if (pictureHeight) {
            dc.SetClippingRegion(left,rect.y+25,right-left,pictureHeight);
            int cell = from+int(std::floor(double(left-from)/FrameWidth))*FrameWidth;
            for (; cell < right; cell += FrameWidth) {
               if (!frames.empty()) {
                  const auto time = view.PositionToTime(cell+FrameWidth/2,rect.x);
                  const auto sourceTime = clip.sourceStart+std::clamp(time-clip.start,0.0,clip.duration);
                  const auto index = std::clamp(int(sourceTime/source.duration*frames.size()),0,int(frames.size())-1);
                  dc.DrawBitmap(frames[index],cell,rect.y+25);
               }
               dc.SetPen(wxPen(wxColour(22,23,26))); dc.DrawLine(cell,rect.y+25,cell,rect.y+25+pictureHeight);
            }
         }
         dc.DestroyClippingRegion(); dc.SetClippingRegion(rect);
         const auto &selection = view.selectedRegion;
         const bool selected = track->GetSelected() && selection.t0() <= clip.start+1e-7 &&
            selection.t1() >= clip.End()-1e-7;
         dc.SetPen(wxPen(selected ? wxColour(252,211,90) : wxColour(111,155,187),selected ? 2 : 1));
         dc.SetBrush(*wxTRANSPARENT_BRUSH); dc.DrawRectangle(left,rect.y+2,right-left,rect.height-4);
         dc.SetTextForeground(*wxWHITE);
         dc.DrawText(wxFileName(wxString::FromUTF8(source.path)).GetFullName(),left+5,rect.y+5);
         {
            wxDCClipper clipFlags{dc, wxRect{left, rect.y, right-left, 24}};
            ClipMarkers::DrawClipMarkerFlags(dc, rect, view, clip.markers,
               clip.start - clip.sourceStart, 1, clip.start, clip.End());
         }
         dc.DestroyClippingRegion();
      }
      CommonChannelView::Draw(context,rect,pass);
   }
};

UIHandle::Result VideoClipHandle::Click(const TrackPanelMouseEvent &event,AudacityProject *project)
{
   const auto track = mTrack.lock();
   if (!track || !project || ProjectAudioIO::Get(*project).IsAudioActive()) return RefreshCode::Cancelled;
   mContext = event.event.RightDown(); mOriginal = track->Timeline();
   mCandidate.reset(); mOrigin = event.rect.x;
   mClickTime = ViewInfo::Get(*project).PositionToTime(event.event.GetX(), mOrigin);
   const auto found = std::find_if(mOriginal.clips.begin(),mOriginal.clips.end(),
      [&](const auto &clip) { return clip.id == mId; });
   auto &selection = ViewInfo::Get(*project).selectedRegion;
   if (!mContext || !track->GetSelected() || selection.isPoint() ||
       mClickTime < selection.t0() || mClickTime >= selection.t1()) {
      SelectUtilities::SelectNone(*project); track->SetSelected(true);
      if (found != mOriginal.clips.end()) selection.setTimes(found->start, found->End());
      else selection.setTimes(std::max(0.0, mClickTime), std::max(0.0, mClickTime));
   }
   if (!mContext && found == mOriginal.clips.end()) return RefreshCode::Cancelled;
   TrackFocus::Get(*project).Set(track.get(),true);
   return RefreshCode::RefreshAll;
}
UIHandle::Result VideoClipHandle::Drag(const TrackPanelMouseEvent &event,AudacityProject *project)
{
   if (std::getenv("AUTOPSY_TEST_VIDEO_INPUT"))
      std::fprintf(stderr, "AUTOPSY_VIDEO_DRAG id=%llu edge=%d context=%d pointer=%d origin=%d click=%g\n",
         static_cast<unsigned long long>(mId), mEdge, int(mContext), event.event.GetX(), mOrigin, mClickTime);
   if (mContext || !mEdge || !project) return RefreshCode::RefreshNone;
   const auto track = mTrack.lock(); const auto view = mView.lock();
   if (!track || !view) return RefreshCode::Cancelled;
   const auto found = std::find_if(mOriginal.clips.begin(),mOriginal.clips.end(),
      [&](const auto &clip) { return clip.id == mId; });
   if (found == mOriginal.clips.end()) return RefreshCode::Cancelled;
   // Keep the original track origin even when dragging over the ruler or
   // another cell, and preserve the pointer's initial distance from the edge.
   const auto at = (mEdge < 0 ? found->start : found->End()) +
      ViewInfo::Get(*project).PositionToTime(event.event.GetX(), mOrigin) - mClickTime;
   auto candidate = mOriginal;
   try {
      DragVideoClipEdge(candidate, mId, mEdge < 0, at);
      mCandidate = std::move(candidate); view->SetGhost(mCandidate);
   } catch (const std::exception &) { /* retain the last usable trim preview */ }
   return RefreshCode::RefreshAll;
}
UIHandle::Result VideoClipHandle::Cancel(AudacityProject *)
{
   if (const auto view = mView.lock()) view->SetGhost({});
   mCandidate.reset(); return RefreshCode::RefreshAll;
}
UIHandle::Result VideoClipHandle::Release(const TrackPanelMouseEvent &,AudacityProject *project,wxWindow *parent)
{
   const auto track = mTrack.lock();
   if (const auto view = mView.lock()) view->SetGhost({});
   if (std::getenv("AUTOPSY_TEST_VIDEO_INPUT"))
      std::fprintf(stderr, "AUTOPSY_VIDEO_RELEASE id=%llu edge=%d candidate=%d\n",
         static_cast<unsigned long long>(mId), mEdge, int(mCandidate.has_value()));
   if (!project || !track || track->GetOwner().get() != &TrackList::Get(*project)) return RefreshCode::RefreshAll;
   try {
      if (mContext) {
         wxMenu menu;
         menu.Append(1,_("Cut")); menu.Append(2,_("Copy")); menu.Append(3,_("Paste"));
         menu.AppendSeparator(); menu.Append(4,_("Split at Cursor")); menu.Append(5,_("Delete and Leave Gap"));
         menu.Append(6,_("Trim to Selection"));
         for (int id : {1, 2, 4, 5, 6})
            menu.Enable(id, !ViewInfo::Get(*project).selectedRegion.isPoint());
         menu.Enable(3, !Clipboard::Get().GetTracks().Any<const AuTopsyVideoTrack>().empty());
         menu.Enable(4, mId != 0);
         auto *moves = new wxMenu;
         std::vector<AuTopsyVideoTrack*> targets;
         for (auto *candidate : ProjectVideoTracks(*project)) if (candidate != track.get()) {
            targets.push_back(candidate); moves->Append(100+int(targets.size())-1,candidate->GetName());
         }
         if (mId && !targets.empty()) menu.AppendSubMenu(moves,_("Move Clip to Track")); else delete moves;
         const auto choice = parent->GetPopupMenuSelectionFromUser(menu);
         if (choice >= 100 && size_t(choice-100) < targets.size()) {
            const auto found = std::find_if(track->Timeline().clips.begin(),track->Timeline().clips.end(),
               [&](const auto &clip) { return clip.id == mId; });
            if (found != track->Timeline().clips.end())
               MoveVideoClipBetweenTracks(*project,*track,*targets[choice-100],mId,found->start);
         }
         else {
            const wxString commands[]{"","Cut","Copy","Paste","Split","SplitDelete","Trim"};
            if (choice > 0 && choice < 7) {
               if (choice == 4)
                  ViewInfo::Get(*project).selectedRegion.setTimes(mClickTime, mClickTime);
               CommandManager::Get(*project).HandleTextualCommand(commands[choice],
                  CommandContext{*project},AlwaysEnabledFlag,false);
            }
         }
      }
      else if (mCandidate) {
         ClipGroups::CommitVideoClipTrim(*project, *track, std::move(*mCandidate));
         for (const auto &clip : track->Timeline().clips) if (clip.id == mId)
            ViewInfo::Get(*project).selectedRegion.setTimes(clip.start, clip.End());
      }
   }
   catch (const std::exception &error) {
      wxMessageBox(wxString::FromUTF8(error.what()),_("Video Track"),wxOK|wxICON_ERROR,parent);
   }
   mCandidate.reset(); return RefreshCode::RefreshAll;
}

class VideoTrackMenu final : public PopupMenuTable {
   VideoTrackMenu() : PopupMenuTable{"VideoTrack"} {}
   DECLARE_POPUP_MENU(VideoTrackMenu);
   CommonTrackControls::InitMenuData *mData{};
   void ToggleVisible(wxCommandEvent &) {
      auto &track = static_cast<AuTopsyVideoTrack&>(mData->track);
      track.SetVideoVisible(!track.VideoVisible());
      ProjectHistory::Get(mData->project).PushState(XO("Changed video track visibility"),XO("Video Visibility"));
      mData->result = RefreshCode::RefreshAll;
   }
public:
   static VideoTrackMenu &Instance() { static VideoTrackMenu menu; return menu; }
   void InitUserData(void *data) override { mData = static_cast<CommonTrackControls::InitMenuData*>(data); }
};
BEGIN_POPUP_MENU(VideoTrackMenu)
   AppendItem("ToggleVideoVisible",32001,XXO("Show / Hide Video"),POPUP_MENU_FN(ToggleVisible));
END_POPUP_MENU()
class VideoTrackControls final : public CommonTrackControls {
public:
   using CommonTrackControls::CommonTrackControls;
   std::vector<UIHandlePtr> HitTest(const TrackPanelMouseState &state,const AudacityProject *project) override
   { return CommonTrackControls::HitTest(state,project); }
   PopupMenuTable *GetMenuExtension(Track *) override { return &VideoTrackMenu::Instance(); }
};
}
using GetVideoControls = DoGetControls::Override<AuTopsyVideoTrack>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(GetVideoControls) {
   return [](AuTopsyVideoTrack &track) { return std::make_shared<VideoTrackControls>(track.SharedPointer()); };
}
using GetVideoView = DoGetView::Override<AuTopsyVideoTrack>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(GetVideoView) {
   return [](AuTopsyVideoTrack &track,size_t channel) {
      return std::make_shared<VideoTrackView>(track.GetChannel(channel));
   };
}
using GetVideoHeight = GetDefaultTrackHeight::Override<AuTopsyVideoTrack>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(GetVideoHeight) {
   return [](AuTopsyVideoTrack &) { return 125; };
}
void VideoTimeline::ShowProjectVideoTrack(AudacityProject &project,std::shared_ptr<const VideoPreview::PreparedMedia>)
{
   Viewport::Get(project).HandleResize(); Viewport::Get(project).Redraw();
}
VideoTimeline::VideoTrackStatus VideoTimeline::GetProjectVideoTrackStatus(AudacityProject &project)
{
   VideoTrackStatus result;
   auto &panel = TrackPanel::Get(project);
   result.panelHandle = reinterpret_cast<uintptr_t>(panel.GetHandle());
   const auto &view = ViewInfo::Get(project);
   result.leftOffset = view.GetLeftOffset(); result.width = panel.GetClientSize().x;
   result.height = panel.GetClientSize().y;
   result.usableWidth = view.GetTracksUsableWidth();
   result.viewStart = view.PositionToTime(result.leftOffset,result.leftOffset);
   result.viewEnd = view.PositionToTime(result.leftOffset+result.usableWidth,result.leftOffset);
   int index{};
   for (auto *track : TrackList::Get(project).Any<Track>()) {
      if (auto *video = dynamic_cast<AuTopsyVideoTrack*>(track)) {
         const auto rect = panel.FindTrackRect(video);
         auto &nativeView = static_cast<VideoTrackView&>(ChannelView::Get(*video));
         const auto data = nativeView.DataRect();
         result.rows.push_back({index,rect.x,rect.y,rect.width,rect.height,data.x,data.width,data.y,video->GetSelected()});
         result.thumbnails = result.thumbnails || nativeView.HasThumbnails();
      }
      ++index;
   }
   result.visible = !result.rows.empty(); return result;
}
