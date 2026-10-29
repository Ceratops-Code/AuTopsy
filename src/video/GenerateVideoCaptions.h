// Thin project/menu adapter for local recognition and editable caption batches.
#pragma once
class AudacityProject;
class Track;
namespace VideoCaptions {
void ShowGenerateCaptions(AudacityProject &project, Track *source = nullptr);
}
