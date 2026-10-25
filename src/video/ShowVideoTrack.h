// Native TrackPanel view and diagnostics for video tracks.
#pragma once
#include <cstdint>
#include <vector>
class AudacityProject;
namespace VideoTimeline {
struct VideoTrackStatus {
   bool visible{}, thumbnails{};
   int leftOffset{}, width{}, height{}, usableWidth{};
   double viewStart{}, viewEnd{};
   struct Button { int x{}, y{}, width{}, height{}; bool active{}; };
   struct Row {
      int index{}, x{}, y{}, width{}, height{}, dataLeft{}, dataWidth{}, dataTop{};
      bool selected{}, thumbnails{};
      Button mute, solo;
   };
   uintptr_t panelHandle{};
   std::vector<Row> rows;
};
void ShowProjectVideoTrack(AudacityProject &project);
VideoTrackStatus GetProjectVideoTrackStatus(AudacityProject &project);
}
