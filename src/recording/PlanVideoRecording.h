// Capture requests and FFmpeg arguments contain no Audacity or window objects.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace VideoRecording {
enum class DeviceKind { Video, Audio, Unknown };
struct CaptureDevice {
   std::string identifier, label;
   DeviceKind kind{DeviceKind::Unknown};
   bool IsCameraCandidate() const noexcept
   { return kind == DeviceKind::Video || kind == DeviceKind::Unknown; }
};
enum class SourceKind { Webcam, Screen, Window, GeneratedTest };
struct ScreenRegion { int left{}, top{}, width{}, height{}; };
struct RecordingOptions {
   SourceKind source{SourceKind::Webcam};
   std::string camera, audio;
   ScreenRegion region;
   uint64_t window{};
   double frameRate{30}, duration{}, projectStart{};
   int maximumWidth{1280}, maximumHeight{720};
   bool includePointer{true}, generatedAudio{};
   std::filesystem::path destination;
};
// FFmpeg's -sources output gives stable identifiers separately from labels.
// Unknown devices remain selectable as cameras; inactive virtual cameras can
// report no media type before they start. Enumeration never opens a capture.
std::vector<CaptureDevice> ReadCaptureDeviceList(const std::string &listing);
// The output is pipe:1. The process owner exclusively creates the destination
// and streams into that handle, so no existing user file can be overwritten.
// Generated input is allowed only after the native adapter checks test ownership.
std::vector<std::string> PrepareVideoRecordingArguments(
   const RecordingOptions &options, bool allowGeneratedInput = false);
}
