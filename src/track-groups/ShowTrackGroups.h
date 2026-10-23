// Presentation entry points used by the small native pane adapters.
#pragma once
class AudacityProject;
class Track;
namespace TrackGroups {
enum class GroupMove { Up, Down, Top, Bottom };
bool IsTrackHidden(const Track &track);
void ShowTrackGroupMenu(AudacityProject &project, Track &track);
bool HandleTrackGroupMove(AudacityProject &project, Track &track, GroupMove choice);
}
