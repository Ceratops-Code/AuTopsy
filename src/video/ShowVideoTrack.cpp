// Native TrackPanel adapters. The host owns scrolling, sizing, focus, track
// order and Undo. There is no separate video panel or copied track list.
#include "ShowVideoTrack.h"
#include "PrepareVideoPreview.h"
#include "CreateVideoTrack.h"
#include "EditVideoFades.h"
#include "EditVideoComposition.h"
#include "PlanVideoEdits.h"
#include "../clip-groups/LinkMediaClips.h"
#include "../clip-groups/GroupAudioClips.h"
#include "../clip-groups/SelectMediaClips.h"
#include "../clip-groups/OpenClipActions.h"
#include "../clip-groups/EditClipPlacement.h"
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
#include "tracks/ui/CommonTrackInfo.h"
#include "tracks/ui/ButtonHandle.h"
#include "tracks/playabletrack/ui/PlayableTrackControls.h"
#include "AColor.h"
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
#include <wx/textdlg.h>
#include <wx/msgdlg.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <unordered_set>

namespace {
using namespace VideoTimeline;
struct ShiftInterval final : ChannelGroupInterval, ClipGroups::LinkedClipInterval {
   Clip clip;
   ClipGroups::TemporaryClipSelection::Key selectionKey{};
   explicit ShiftInterval(Clip value, ClipGroups::TemporaryClipSelection::Key key = 0)
      : clip(std::move(value)), selectionKey(key) {}
   double Start() const override { return clip.start; }
   double End() const override { return clip.End(); }
   std::vector<std::string> MediaGroupIds() const override { return VideoTimeline::VideoContentLinkGroups(clip); }
   unsigned long long VideoClipId() const override { return clip.id; }
};

// The existing move engine supplies snapping, multi-track selection, scrolling,
// vertical migration and Escape rollback. This policy supplies video intervals,
// source-frame quantization and free-space constraints.
class VideoTrackShifter final : public TrackShifter {
   std::shared_ptr<AuTopsyVideoTrack> mTrack;
   AudacityProject &mProject;
   static ShiftInterval &Data(const std::shared_ptr<ChannelGroupInterval> &value)
   { return static_cast<ShiftInterval&>(*value); }
public:
   explicit VideoTrackShifter(AuTopsyVideoTrack &track, AudacityProject &project)
      : mTrack(track.SharedPointer<AuTopsyVideoTrack>()), mProject(project) {
      for (const auto &clip : track.Timeline().clips) {
         auto value = clip; value.media = SourceFor(track.Timeline(), clip);
         const auto key = ClipGroups::TemporaryVideoClipSelectionKey(project, track, clip.id);
         mFixed.push_back(std::make_shared<ShiftInterval>(std::move(value), key));
      }
   }
   Track &GetTrack() const override { return *mTrack; }
   bool SyncLocks() override { return true; }
   HitTestResult HitTest(double time, const ViewInfo &view, HitTestParams *params) override {
      const auto *clip = ClipAt(mTrack->Timeline(), time);
      if (!clip) return HitTestResult::Miss;
      if (params)
         for (const auto &interval : mFixed)
            if (Data(interval).clip.id == clip->id) { params->hitInterval = interval; break; }
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
      const auto &clip = Data(mMoving.front()).clip;
      // A source frame occupies less project time after speeding up the clip.
      // Keep free drags on that project-frame grid; source-rate rounding can
      // otherwise land on a nearby magnetic target even while Alt bypasses it.
      const auto rate = VideoContentFrameRate(clip, clip.media) * clip.playbackRate;
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
   { return dynamic_cast<AuTopsyVideoTrack*>(&other) != nullptr &&
      !ClipGroups::BoundVideoTrackMoveFamilies(TrackList::Get(mProject)).count(mTrack.get()); }
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
       const auto &clip = Data(intervals.front()).clip;
       const auto rate = VideoContentFrameRate(clip, clip.media);
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
         attached.push_back(std::make_shared<ShiftInterval>(std::move(clip), Data(interval).selectionKey));
      }
      std::sort(candidate.clips.begin(), candidate.clips.end(),
         [](const auto &a, const auto &b) { return a.start < b.start; });
      RequireWellFormedTimeline(candidate); mTrack->SetTimeline(std::move(candidate));
      for (const auto &interval : attached)
         ClipGroups::MoveTemporaryVideoClipReference(mProject, Data(interval).selectionKey, *mTrack, Data(interval).clip.id);
      mMoving.insert(mMoving.end(), attached.begin(), attached.end());
      return true;
   }
};
}
using MakeVideoShifter = MakeTrackShifter::Override<AuTopsyVideoTrack>;
DEFINE_ATTACHED_VIRTUAL_OVERRIDE(MakeVideoShifter) {
    return [](AuTopsyVideoTrack &track, AudacityProject &project) {
       return std::make_unique<VideoTrackShifter>(track, project);
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
   bool mDoubleClick{};
   bool mPointClick{}, mDragged{};
   wxPoint mPress;
   double mPressTime{};
public:
   using AffordanceHandle::AffordanceHandle;
   Result Click(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      mDoubleClick = event.event.LeftDClick();
      mPress = event.event.GetPosition(); mDragged = false;
      mPointClick = !mDoubleClick && !event.event.ShiftDown() && !event.event.CmdDown();
      mPressTime = std::max(0.0, ViewInfo::Get(*project).PositionToTime(mPress.x, event.rect.x));
      return AffordanceHandle::Click(event, project);
   }
   Result Drag(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      mDragged = mDragged || std::abs(event.event.GetX() - mPress.x) >= 3 ||
         std::abs(event.event.GetY() - mPress.y) >= 3;
      return AffordanceHandle::Drag(event, project);
   }
   Result Release(const TrackPanelMouseEvent &event, AudacityProject *project, wxWindow *parent) override {
      // Native release can refresh/reassign the hovered handle. Retain the
      // press-time intent across that dispatch, including after a canceled drag.
      const bool point = mPointClick && !mDragged;
      const auto at = mPressTime;
      const auto track = GetTrack();
      const auto result = AffordanceHandle::Release(event, project, parent);
      if (point && track && project) {
         ViewInfo::Get(*project).selectedRegion.setTimes(at, at);
         TrackFocus::Get(*project).Set(track.get(), true);
         ProjectHistory::Get(*project).ModifyState(false);
         Viewport::Get(*project).Redraw();
         return result | RefreshCode::RefreshAll;
      }
      return result;
   }
   Result SelectAt(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      const auto track = std::dynamic_pointer_cast<AuTopsyVideoTrack>(GetTrack());
      if (!track || !project) return RefreshCode::RefreshNone;
      auto &view = ViewInfo::Get(*project);
      if (const auto *clip = ClipAt(track->Timeline(),
            view.PositionToTime(event.event.GetX(), event.rect.x))) {
         const auto at = std::max(0.0, view.PositionToTime(event.event.GetX(), event.rect.x));
         if (mDoubleClick) view.selectedRegion.setTimes(clip->start, clip->End());
         else view.selectedRegion.setTimes(at, at);
         TrackFocus::Get(*project).Set(track.get(), true);
         ProjectHistory::Get(*project).ModifyState(false);
      }
      return RefreshCode::RefreshAll;
   }
};

// Picture clicks keep the host's edit cursor at the pointer. Double clicks
// select one clip, and drags retain native range selection and cancellation.
class VideoSelectionHandle final : public SelectHandle {
   std::weak_ptr<AuTopsyVideoTrack> mTrack;
   wxPoint mClick;
   uint64_t mId{};
   bool mDragged{};
   bool mDoubleClick{};
   double mPressTime{};
   Result SelectPoint(AudacityProject *project, double at) {
      const auto track = mTrack.lock();
      if (!track || !mId) return RefreshCode::RefreshNone;
      ViewInfo::Get(*project).selectedRegion.setTimes(at, at);
      TrackFocus::Get(*project).Set(track.get(), true);
      ProjectHistory::Get(*project).ModifyState(false);
      return RefreshCode::RefreshAll;
   }
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
      mDoubleClick = event.event.LeftDClick();
      mPressTime = std::max(0.0, ViewInfo::Get(*project).PositionToTime(mClick.x, event.rect.x));
      if (!event.event.ShiftDown() && !event.event.CmdDown())
         if (const auto track = mTrack.lock())
            if (const auto *clip = ClipAt(track->Timeline(),
                  ViewInfo::Get(*project).PositionToTime(mClick.x, event.rect.x)))
               mId = clip->id;
      const auto result = SelectHandle::Click(event, project);
      return mDoubleClick ? result | SelectClip(project) : result | SelectPoint(project, mPressTime);
   }
   Result Drag(const TrackPanelMouseEvent &event, AudacityProject *project) override {
      mDragged = mDragged || std::abs(event.event.GetX() - mClick.x) >= 3 ||
         std::abs(event.event.GetY() - mClick.y) >= 3;
      return mDragged ? SelectHandle::Drag(event, project) : RefreshCode::RefreshNone;
   }
   Result Release(const TrackPanelMouseEvent &event, AudacityProject *project, wxWindow *parent) override {
      const bool point = !mDragged && !mDoubleClick && mId;
      const bool whole = !mDragged && mDoubleClick;
      const auto at = mPressTime;
      const auto result = SelectHandle::Release(event, project, parent);
      return whole ? result | SelectClip(project) : point ? result | SelectPoint(project, at) : result;
   }
};
class VideoTrackView;
class VideoClipHandle final : public UIHandle {
   std::weak_ptr<AuTopsyVideoTrack> mTrack;
   std::weak_ptr<VideoTrackView> mView;
   uint64_t mId, mEditedId{};
   int mEdge;
   bool mContext{};
   bool mGestureActive{};
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
         data.media = VideoPreview::FindPreparedVideoThumbnails(wxString::FromUTF8(source.path),
            source.duration, parent);
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
   void ClearMissingThumbnails() {
      for (auto it = mSources.begin(); it != mSources.end();)
         if (it->second.frames.empty()) it = mSources.erase(it); else ++it;
   }
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
      if (edge)
      {
         // Escape can rotate the native panel to its fallback range selector.
         // That old candidate must not keep priority over a newly hit edge.
         // Retire its identity here; CommonChannelView supplies a fresh fallback.
         mSelectHandle.reset();
         return {AssignUIHandlePtr(mClipHandle, std::make_shared<VideoClipHandle>(
            track, std::static_pointer_cast<VideoTrackView>(shared_from_this()), clip->id, edge))};
      }
      if (clip && state.state.GetY() < state.rect.y+24)
         return {AssignUIHandlePtr(mAffordanceHandle,
            std::make_shared<VideoAffordanceHandle>(track))};
      // Gaps need only the native range selector. Adding a second selector
      // there lets the host retain its old lower-priority hit after Escape,
      // masking a newly hovered video edge on the next gesture.
      if (!clip) return {};
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
      const bool explicitSelection = ClipGroups::HasTemporaryClipSelection(*project);
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
         dc.SetBrush(wxBrush(track->VideoVisible() && !clip.muted ? wxColour(57,105,143) : wxColour(69,72,77)));
         dc.DrawRectangle(left,rect.y+2,right-left,rect.height-4);
         const int pictureHeight = std::max(0,std::min(FrameHeight,rect.height-30));
         // A muted occurrence remains recognizable in the editing pane. This
         // thumbnail-only copy does not alter playback or the retained content.
         auto thumbnailClip = clip; thumbnailClip.muted = false;
         if (pictureHeight) {
            dc.SetClippingRegion(left,rect.y+25,right-left,pictureHeight);
            int cell = from+int(std::floor(double(left-from)/FrameWidth))*FrameWidth;
            for (; cell < right; cell += FrameWidth) {
               const auto time = std::clamp(view.PositionToTime(cell+FrameWidth/2,rect.x),
                  clip.start, std::nextafter(clip.End(), clip.start));
               const auto sample = SampleVideoContent(thumbnailClip, source, clip.ContentTimeAtProjectTime(time));
               if (sample) {
                  auto &frames = Frames(sample.source,&ProjectWindow::Get(*project)).frames;
                  if (!frames.empty()) {
                     const auto index = std::clamp(int(sample.sourceTime/sample.source.duration*frames.size()),
                        0,int(frames.size())-1);
                     dc.DrawBitmap(frames[index],cell,rect.y+25);
                  }
               }
               dc.SetPen(wxPen(wxColour(22,23,26))); dc.DrawLine(cell,rect.y+25,cell,rect.y+25+pictureHeight);
            }
         }
         dc.DestroyClippingRegion(); dc.SetClippingRegion(rect);
         const auto &selection = view.selectedRegion;
         const bool selected = explicitSelection
            ? ClipGroups::IsVideoClipInTemporarySelection(*project, *track, clip.id)
            : track->GetSelected() && selection.t0() <= clip.start+1e-7 && selection.t1() >= clip.End()-1e-7;
         dc.SetPen(wxPen(selected ? wxColour(252,211,90) : wxColour(111,155,187),selected ? 2 : 1));
         dc.SetBrush(*wxTRANSPARENT_BRUSH); dc.DrawRectangle(left,rect.y+2,right-left,rect.height-4);
         dc.SetTextForeground(*wxWHITE);
         auto title = clip.content ? wxString::Format(_("Joined video (%zu pieces)"), clip.content->parts.size())
             : wxFileName(wxString::FromUTF8(source.path)).GetFullName();
         if (clip.playbackRate != 1) title = wxString::Format("%.8g%%  ", clip.playbackRate * 100) + title;
         if (!track->Timeline().placement.parentId.empty()) title = _("[Embedded] ") + title;
         if (clip.muted) title = _("[Muted] ") + title;
         if (clip.timingLocked) title = _("[Locked] ") + title;
         dc.DrawText(title,left+5,rect.y+5);
         {
            wxDCClipper clipFlags{dc, wxRect{left, rect.y, right-left, 24}};
            ClipMarkers::DrawClipMarkerFlags(dc, rect, view, clip.markers,
               clip.ProjectTimeAtContentTime(0), 1 / clip.playbackRate, clip.start, clip.End());
         }
         dc.DestroyClippingRegion();
      }
      CommonChannelView::Draw(context,rect,pass);
   }
};

UIHandle::Result VideoClipHandle::Click(const TrackPanelMouseEvent &event,AudacityProject *project)
{
   mGestureActive = false;
   const auto track = mTrack.lock();
   if (!track || !project || ProjectAudioIO::Get(*project).IsAudioActive()) return RefreshCode::Cancelled;
   mContext = event.event.RightDown(); mOriginal = track->Timeline();
   mCandidate.reset(); mEditedId = mId; mOrigin = event.rect.x;
   const auto &view = ViewInfo::Get(*project);
   mClickTime = view.PositionToTime(event.event.GetX(), mOrigin);
   if (mContext) {
      // A retained hover handle can outlive a different hit after a popup.
      // Bind this gesture to the current painted, half-open clip bounds.
      mId = 0;
      for (const auto &clip : mOriginal.clips)
         if (view.TimeToPosition(clip.start, mOrigin) <= event.event.GetX() &&
             event.event.GetX() < view.TimeToPosition(clip.End(), mOrigin)) {
            mId = clip.id; break;
         }
   }
   const auto found = std::find_if(mOriginal.clips.begin(),mOriginal.clips.end(),
      [&](const auto &clip) { return clip.id == mId; });
   if (!mContext && mEdge && found != mOriginal.clips.end() && found->timingLocked)
      return RefreshCode::Cancelled;
   auto &selection = ViewInfo::Get(*project).selectedRegion;
   if (!mContext || !track->GetSelected() || selection.isPoint() ||
       mClickTime < selection.t0() || mClickTime >= selection.t1()) {
      SelectUtilities::SelectNone(*project); track->SetSelected(true);
      if (found != mOriginal.clips.end()) selection.setTimes(found->start, found->End());
      else selection.setTimes(std::max(0.0, mClickTime), std::max(0.0, mClickTime));
   }
   if (!mContext && found == mOriginal.clips.end()) return RefreshCode::Cancelled;
   TrackFocus::Get(*project).Set(track.get(),true);
   mGestureActive = true;
   return RefreshCode::RefreshAll;
}
UIHandle::Result VideoClipHandle::Drag(const TrackPanelMouseEvent &event,AudacityProject *project)
{
   if (!mGestureActive || mContext || !mEdge || !project) return RefreshCode::RefreshNone;
   const auto track = mTrack.lock(); const auto view = mView.lock();
   if (!track || !view) return RefreshCode::Cancelled;
   // Keep the initial origin and pointer offset. A shared seam chooses the
   // side shortened by this drag, independent of a one-pixel hit ambiguity.
   const auto motion = ViewInfo::Get(*project).PositionToTime(event.event.GetX(), mOrigin) - mClickTime;
   auto candidate = mOriginal;
   try {
      const auto edge = ChooseVideoTrimEdge(mOriginal, mId, mEdge < 0, motion);
      const auto found = std::find_if(mOriginal.clips.begin(),mOriginal.clips.end(),
         [&](const auto &clip) { return clip.id == edge.id; });
      if (found != mOriginal.clips.end() && found->timingLocked) return RefreshCode::RefreshNone;
      const auto at = (edge.left ? found->start : found->End()) + motion;
      DragVideoClipEdge(candidate, edge.id, edge.left, at);
      mEditedId = edge.id;
      mCandidate = std::move(candidate); view->SetGhost(mCandidate);
   } catch (const std::exception &) { /* retain the last usable trim preview */ }
   return RefreshCode::RefreshAll;
}
UIHandle::Result VideoClipHandle::Cancel(AudacityProject *)
{
   // Mouse-up and coalesced motion can still arrive after Escape. They belong
   // to the closed gesture and must never rebuild its discarded candidate.
   mGestureActive = false;
   if (const auto view = mView.lock()) view->SetGhost({});
   mCandidate.reset(); return RefreshCode::RefreshAll;
}
UIHandle::Result VideoClipHandle::Release(const TrackPanelMouseEvent &event,AudacityProject *project,wxWindow *parent)
{
   if (!mGestureActive) return RefreshCode::RefreshNone;
   const auto track = mTrack.lock();
   if (const auto view = mView.lock()) view->SetGhost({});
   if (!project || !track || track->GetOwner().get() != &TrackList::Get(*project)) {
      mGestureActive = false;
      mCandidate.reset();
      return RefreshCode::RefreshAll;
   }
   try {
      if (!mContext && mEdge) {
         // Windows can coalesce the last motion before mouse-up. Commit the
         // released pointer position, rather than the last painted ghost.
         Drag(event, project);
         if (const auto view = mView.lock()) view->SetGhost({});
      }
       // Close before committing or opening a nested popup event loop. Only
       // this release owns the final candidate; later events cannot commit it.
       mGestureActive = false;
      if (mContext) {
         // Native popup dispatch runs a nested event loop. Hover refresh may
         // overwrite this shared handle; the menu keeps its original identity.
         const auto clipId = mId;
         const auto clickTime = mClickTime;
         wxMenu menu;
         menu.Append(1,_("Cut")); menu.Append(2,_("Copy")); menu.Append(3,_("Paste"));
          menu.AppendSeparator(); menu.Append(4,_("Split at Cursor"));
          const auto gapLabel = XO("Delete and Leave Gap");
          const auto rippleLabel = XO("Ripple Delete (Close Gap)");
          auto &commandManager = CommandManager::Get(*project);
          menu.Append(5, commandManager.FormatLabelForMenu("DeleteKey2", &gapLabel));
          menu.Append(18, commandManager.FormatLabelForMenu("RippleDelete", &rippleLabel));
         menu.Append(6,_("Trim to Selection"));
         menu.AppendSeparator(); menu.Append(7,_("Heal Split")); menu.Append(8,_("Join"));
         menu.Append(9,_("Separate Joined Clip"));
         menu.AppendSeparator(); menu.Append(10,_("Change Clip Speed...")); menu.Enable(10, clipId != 0);
         menu.Append(11, _("Crop Picture...")); menu.Enable(11, clipId != 0);
         menu.Append(21, _("Reverse Picture")); menu.Enable(21, clipId != 0);
         menu.Append(22, _("Freeze Frame...")); menu.Enable(22, clipId != 0);
         menu.Append(23, _("Repeat Picture...")); menu.Enable(23, clipId != 0);
         menu.Append(19, _("Picture-in-Picture..."));
         menu.Append(20, _("Detach Embedded Video"));
         menu.Enable(20, !track->Timeline().placement.parentId.empty());
         menu.Append(16, _("Fade Picture...")); menu.Enable(16, clipId != 0);
         menu.Append(17, _("Crossfade from Video Below...")); menu.Enable(17, clipId != 0);
         menu.Append(14, _("Clip Properties...")); menu.Enable(14, clipId != 0);
         menu.Append(15, _("Close Gap Before Clip")); menu.Enable(15, clipId != 0);
         menu.AppendCheckItem(13, _("Mute Clip")); menu.Enable(13, clipId != 0);
         for (const auto &clip : track->Timeline().clips)
            if (clip.id == clipId) menu.Check(13, clip.muted);
         menu.AppendSeparator(); menu.Append(12, _("Export Clip...")); menu.Enable(12, clipId != 0);
         for (int id : {7, 8, 9}) menu.Enable(id, clipId != 0);
          for (int id : {1, 2, 4, 5, 6, 18})
            menu.Enable(id, !ViewInfo::Get(*project).selectedRegion.isPoint());
         menu.Enable(3, !Clipboard::Get().GetTracks().Any<const AuTopsyVideoTrack>().empty());
         menu.Enable(4, clipId != 0);
         auto *moves = new wxMenu;
         std::vector<AuTopsyVideoTrack*> targets;
         for (auto *candidate : ProjectVideoTracks(*project)) if (candidate != track.get()) {
            targets.push_back(candidate); moves->Append(100+int(targets.size())-1,candidate->GetName());
         }
         if (clipId && !targets.empty()) menu.AppendSubMenu(moves,_("Move Clip to Track")); else delete moves;
         const auto choice = parent->GetPopupMenuSelectionFromUser(menu);
         if (choice >= 100 && size_t(choice-100) < targets.size()) {
            const auto found = std::find_if(track->Timeline().clips.begin(),track->Timeline().clips.end(),
               [&](const auto &clip) { return clip.id == clipId; });
            if (found != track->Timeline().clips.end())
               MoveVideoClipBetweenTracks(*project,*track,*targets[choice-100],clipId,found->start);
         }
          else if (choice == 18)
             commandManager.HandleTextualCommand("RippleDelete", CommandContext{*project}, AlwaysEnabledFlag, false);
          else if (choice == 16 || choice == 17)
            VideoEditing::ShowVideoClipFades(*project, *track, clipId, parent, choice == 17);
         else if (choice == 14)
            ClipActions::ShowVideoClipProperties(*project, *track, clipId, parent);
         else if (choice == 15)
            ClipActions::CloseGapBeforeVideoClip(*project, *track, clipId);
         else if (choice == 13) {
            const auto found = std::find_if(track->Timeline().clips.begin(), track->Timeline().clips.end(),
               [&](const auto &clip) { return clip.id == clipId; });
            if (found == track->Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
            VideoEditing::ChangeProjectVideoClipMute(*project, *track, clipId, !found->muted);
         }
         else if (choice == 12)
            ClipActions::ShowVideoClipExport(*project, *track, clipId, parent);
         else if (choice >= 21 && choice <= 23) {
            const VideoEditing::VideoClipPictureAction actions[]{VideoEditing::VideoClipPictureAction::Reverse,
               VideoEditing::VideoClipPictureAction::Freeze, VideoEditing::VideoClipPictureAction::Repeat};
            VideoEditing::ShowVideoClipPictureAction(*project, *track, clipId, actions[choice - 21], parent, clickTime);
         }
         else if (choice == 19)
            VideoEditing::ShowVideoTrackEmbeddingDialog(*project, *track, parent);
         else if (choice == 20) {
            auto placement = track->Timeline().placement; placement.parentId.clear(); placement.rectangle = {};
            VideoEditing::ChangeProjectVideoTrackPlacement(*project, *track, placement);
         }
         else if (choice == 11)
            VideoEditing::ShowVideoClipCropDialog(*project, *track, clipId, parent);
         else if (choice == 10) {
            const auto found = std::find_if(track->Timeline().clips.begin(), track->Timeline().clips.end(),
               [&](const auto &clip) { return clip.id == clipId; });
            if (found == track->Timeline().clips.end()) throw std::runtime_error("The video clip was removed.");
            wxTextEntryDialog dialog{parent, _("Speed (%; 100 is normal). Linked media changes together."),
               _("Video Clip Speed"), wxString::Format("%.8g", found->playbackRate * 100)};
            if (dialog.ShowModal() == wxID_OK) {
               double percent{};
               if (!dialog.GetValue().ToDouble(&percent)) throw std::runtime_error("Enter a numeric speed percentage.");
               ClipGroups::ChangeLinkedVideoClipSpeed(*project, *track, clipId, percent / 100);
            }
         }
         else if (choice >= 7 && choice <= 9) {
            const VideoEditing::VideoEditKind kinds[]{VideoEditing::VideoEditKind::HealClips,
               VideoEditing::VideoEditKind::JoinClips, VideoEditing::VideoEditKind::SeparateClips};
            const auto &selection = ViewInfo::Get(*project).selectedRegion;
            EditSelectedVideoStructure(*project, kinds[choice - 7], selection.t0(), selection.t1());
         }
         else {
             const wxString commands[]{"","Cut","Copy","Paste","Split","DeleteKey2","Trim"};
            if (choice > 0 && choice < 7) {
               if (choice == 4)
                  ViewInfo::Get(*project).selectedRegion.setTimes(clickTime, clickTime);
               CommandManager::Get(*project).HandleTextualCommand(commands[choice],
                  CommandContext{*project},AlwaysEnabledFlag,false);
            }
         }
      }
      else if (mCandidate) {
         ClipGroups::CommitVideoClipTrim(*project, *track, std::move(*mCandidate));
         for (const auto &clip : track->Timeline().clips) if (clip.id == mEditedId)
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
   void ToggleMute(wxCommandEvent &) {
      auto &track = static_cast<AuTopsyVideoTrack&>(mData->track);
      SetProjectVideoTrackVisibility(mData->project, track, !track.VideoVisible(), track.VideoSolo());
      mData->result = RefreshCode::RefreshAll;
   }
   void ToggleSolo(wxCommandEvent &) {
      auto &track = static_cast<AuTopsyVideoTrack&>(mData->track);
      SetProjectVideoTrackVisibility(mData->project, track, track.VideoVisible(), !track.VideoSolo());
      mData->result = RefreshCode::RefreshAll;
   }
   void PlacePicture(wxCommandEvent &) {
      VideoEditing::ShowVideoTrackEmbeddingDialog(mData->project,
         static_cast<AuTopsyVideoTrack&>(mData->track), &ProjectWindow::Get(mData->project));
      mData->result = RefreshCode::RefreshAll;
   }
   void DetachPicture(wxCommandEvent &) {
      auto &track = static_cast<AuTopsyVideoTrack&>(mData->track);
      auto placement = track.Timeline().placement; placement.parentId.clear(); placement.rectangle = {};
      VideoEditing::ChangeProjectVideoTrackPlacement(mData->project, track, placement);
      mData->result = RefreshCode::RefreshAll;
   }
public:
   static VideoTrackMenu &Instance() { static VideoTrackMenu menu; return menu; }
   void InitUserData(void *data) override { mData = static_cast<CommonTrackControls::InitMenuData*>(data); }
};
BEGIN_POPUP_MENU(VideoTrackMenu)
   AppendItem("PlaceVideoLayer", 32003, XXO("Picture-in-Picture..."), POPUP_MENU_FN(PlacePicture));
   AppendItem("DetachVideoLayer", 32004, XXO("Detach Embedded Video"), POPUP_MENU_FN(DetachPicture),
      [](PopupMenuHandler &handler, wxMenu &menu, int id) {
         const auto &track = static_cast<AuTopsyVideoTrack&>(static_cast<VideoTrackMenu&>(handler).mData->track);
         menu.Enable(id, !track.Timeline().placement.parentId.empty());
      });
   AppendCheckItem("ToggleVideoMute",32001,XXO("Mute Video"),POPUP_MENU_FN(ToggleMute),
      [](PopupMenuHandler &handler, wxMenu &menu, int id) {
         const auto &track = static_cast<AuTopsyVideoTrack&>(static_cast<VideoTrackMenu&>(handler).mData->track);
         menu.Check(id, !track.VideoVisible());
      });
   AppendCheckItem("ToggleVideoSolo",32002,XXO("Solo Video"),POPUP_MENU_FN(ToggleSolo),
      [](PopupMenuHandler &handler, wxMenu &menu, int id) {
         const auto &track = static_cast<AuTopsyVideoTrack&>(static_cast<VideoTrackMenu&>(handler).mData->track);
         menu.Check(id, track.VideoSolo());
      });
END_POPUP_MENU()

class VideoVisibilityButtonHandle final : public ButtonHandle {
   bool mSolo;
public:
   VideoVisibilityButtonHandle(const std::shared_ptr<Track> &track, const wxRect &rect, bool solo)
      : ButtonHandle(track, rect), mSolo(solo) {}
   VideoVisibilityButtonHandle &operator=(const VideoVisibilityButtonHandle&) = default;
   bool SoloButton() const { return mSolo; }
protected:
   Result CommitChanges(const wxMouseEvent &, AudacityProject *project, wxWindow *parent) override {
      const auto track = std::dynamic_pointer_cast<AuTopsyVideoTrack>(GetTrack());
      if (!track || !project) return RefreshCode::RefreshNone;
      try {
         SetProjectVideoTrackVisibility(*project, *track,
            mSolo ? track->VideoVisible() : !track->VideoVisible(),
            mSolo ? !track->VideoSolo() : track->VideoSolo());
      }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Video Mute/Solo"), wxOK | wxICON_ERROR, parent);
      }
      return RefreshCode::RefreshAll;
   }
   TranslatableString Tip(const wxMouseState &, AudacityProject &) const override {
      return mSolo ? XO("Solo this video track. Audio mixing is independent; video Mute overrides Solo.")
         : XO("Mute this video track. Its audio remains in the separate audio track.");
   }
   bool StopsOnKeystroke() override { return true; }
};

void DrawVideoVisibilityButtons(TrackPanelDrawingContext &, const wxRect &, const Track *);
class VideoTrackControls final : public CommonTrackControls {
   std::weak_ptr<VideoVisibilityButtonHandle> mMuteHandle, mSoloHandle;
   wxRect mBounds;
public:
   using CommonTrackControls::CommonTrackControls;
   const TCPLines &GetTCPLines() const override {
      static const TCPLines lines = [] {
         auto result = CommonTrackInfo::StaticTCPLines();
         result.push_back({TrackInfo::TCPLine::kItemMute | TrackInfo::TCPLine::kItemSolo,
            20, 2, DrawVideoVisibilityButtons});
         return result;
      }();
      return lines;
   }
   wxRect ButtonRect(bool solo, const wxRect *bounds = nullptr) const {
      wxRect result;
      const auto track = FindTrack();
      if (!track) return result;
      const auto &rect = bounds ? *bounds : mBounds;
      // Reuse the host's semantic Mute/Solo layout, without making picture an
      // audio PlayableTrack or copying the host's row positions.
      PlayableTrackControls::GetMuteSoloRect(rect, result, solo, track.get());
      if (CommonTrackInfo::HideTopItem(rect, result)) return {};
      return result;
   }
   std::vector<UIHandlePtr> HitTest(const TrackPanelMouseState &state,const AudacityProject *project) override {
      const auto track = FindTrack();
      for (const bool solo : {false, true}) {
         const auto rect = ButtonRect(solo, &state.rect);
         if (track && rect.Contains(state.state.GetPosition())) {
            auto handle = std::make_shared<VideoVisibilityButtonHandle>(track, rect, solo);
            return {AssignUIHandlePtr(solo ? mSoloHandle : mMuteHandle, handle)};
         }
      }
      return CommonTrackControls::HitTest(state,project);
   }
   void Draw(TrackPanelDrawingContext &context, const wxRect &rect, unsigned pass) override {
      if (pass == TrackArtist::PassControls) mBounds = rect;
      CommonTrackControls::Draw(context, rect, pass);
   }
   PopupMenuTable *GetMenuExtension(Track *) override { return &VideoTrackMenu::Instance(); }
};
void DrawVideoVisibilityButtons(TrackPanelDrawingContext &context, const wxRect &, const Track *track)
{
   const auto *video = dynamic_cast<const AuTopsyVideoTrack*>(track);
   if (!video) return;
   const auto &controls = static_cast<const VideoTrackControls&>(TrackControls::Get(*video));
   const auto *target = dynamic_cast<VideoVisibilityButtonHandle*>(context.target.get());
   for (const bool solo : {false, true}) {
      const auto rect = controls.ButtonRect(solo);
      if (rect.IsEmpty()) continue;
      const bool hit = target && target->GetTrack().get() == track && target->SoloButton() == solo;
      const bool pressed = hit && target->IsDragging() && rect.Contains(context.lastState.GetPosition());
      const bool active = solo ? video->VideoSolo() : !video->VideoVisible();
      AColor::Bevel2(context.dc, active == pressed, rect, track->GetSelected(), hit);
      TrackInfo::SetTrackInfoFont(&context.dc);
      const auto label = solo ? _("Solo") : _("Mute");
      const auto size = context.dc.GetTextExtent(label);
      context.dc.DrawText(label, rect.x + (rect.width - size.x) / 2, rect.y + (rect.height - size.y) / 2);
   }
}
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
void VideoTimeline::ShowProjectVideoTrack(AudacityProject &project)
{
   // Preparation can complete after an earlier paint found no cache entry.
   // Retire only those misses; completed decoded filmstrips stay reusable.
   for (auto *track : TrackList::Get(project).Any<AuTopsyVideoTrack>())
      static_cast<VideoTrackView&>(ChannelView::Get(*track)).ClearMissingThumbnails();
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
      result.rows.push_back({index,rect.x,rect.y,rect.width,rect.height,data.x,data.width,data.y,video->GetSelected(),nativeView.HasThumbnails()});
         auto &row = result.rows.back();
         const auto &controls = static_cast<const VideoTrackControls&>(TrackControls::Get(*video));
         const auto mute = controls.ButtonRect(false), solo = controls.ButtonRect(true);
         row.mute = {mute.x, mute.y, mute.width, mute.height, !video->VideoVisible()};
         row.solo = {solo.x, solo.y, solo.width, solo.height, video->VideoSolo()};
         result.thumbnails = result.thumbnails || nativeView.HasThumbnails();
      }
      ++index;
   }
   result.visible = !result.rows.empty(); return result;
}
