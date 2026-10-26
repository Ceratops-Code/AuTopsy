#include "PlanVideoRecording.h"
#include <catch2/catch.hpp>
#include <algorithm>
#include <limits>

using namespace VideoRecording;
namespace {
RecordingOptions Options() {
   RecordingOptions result;
   result.camera = "camera";
   result.destination = std::filesystem::temp_directory_path() / "_recording-test.mkv";
   return result;
}
std::string Value(const std::vector<std::string> &args, const std::string &key) {
   const auto found = std::find(args.begin(), args.end(), key);
   REQUIRE(found != args.end()); REQUIRE(found+1 != args.end()); return *(found+1);
}
}

TEST_CASE("Capture device identities remain distinct from labels and list ordering") {
   const std::string list = "Auto-detected sources for dshow:\r\n"
      "  camera-one [Same camera] (video)\r\n  microphone [Mic] (audio)\r\n"
      "  camera-two [Same camera] (video)\r\n  virtual [Virtual camera] (none)\r\n"
      "  camera-one [Same camera] (video)\r\n";
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto devices = ReadCaptureDeviceList(list);
      REQUIRE(devices.size() == 4);
      REQUIRE(devices[0].identifier == "camera-one");
      REQUIRE(devices[2].identifier == "camera-two");
      REQUIRE(devices[0].label == devices[2].label);
      REQUIRE(devices[1].kind == DeviceKind::Audio);
      REQUIRE(devices[3].kind == DeviceKind::Unknown);
   }
   REQUIRE(ReadCaptureDeviceList("Auto-detected sources for dshow:\n").empty());
   REQUIRE_THROWS(ReadCaptureDeviceList("  bad camera (video)\n"));
   REQUIRE_THROWS(ReadCaptureDeviceList(std::string(512*1024+1, 'x')));
}

TEST_CASE("Webcam recording keeps one clock and quotes device syntax as data") {
   auto options = Options();
   options.camera = "Camera: Roman's [HD]"; options.audio = "Microphone: USB";
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto args = PrepareVideoRecordingArguments(options);
      REQUIRE(Value(args, "-i") == "video='Camera: Roman'\\''s [HD]':audio='Microphone: USB'");
      REQUIRE(std::count(args.begin(), args.end(), "-i") == 1);
      REQUIRE(Value(args, "-f") == "dshow");
      REQUIRE(Value(args, "-c:a") == "pcm_s16le");
      REQUIRE(args.back() == "pipe:1");
      REQUIRE(std::find(args.begin(), args.end(), options.destination.u8string()) == args.end());
   }
   options.audio.clear();
   const auto silent = PrepareVideoRecordingArguments(options);
   REQUIRE(std::find(silent.begin(), silent.end(), "-an") != silent.end());
}

TEST_CASE("Screen recording preserves negative display coordinates and window identities") {
   auto options = Options(); options.source = SourceKind::Screen;
   options.region = {-1920, -150, 1920, 1080}; options.audio = "microphone";
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto args = PrepareVideoRecordingArguments(options);
      REQUIRE(Value(args, "-offset_x") == "-1920");
      REQUIRE(Value(args, "-offset_y") == "-150");
      REQUIRE(Value(args, "-video_size") == "1920x1080");
      REQUIRE(Value(args, "-isync") == "0");
      REQUIRE(std::count(args.begin(), args.end(), "-use_wallclock_as_timestamps") == 2);
   }
   options.source = SourceKind::Window; options.window = 123456789;
   REQUIRE(Value(PrepareVideoRecordingArguments(options), "-i") == "hwnd=123456789");
   options.window = 0;
   REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
}

TEST_CASE("Recording rejects unusable requests before creating media or invoking devices") {
   auto options = Options();
   for (const auto value : {0.0, -1.0, 121.0, std::numeric_limits<double>::quiet_NaN()}) {
      options.frameRate = value; REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   }
   options = Options(); options.duration = -1; REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   options = Options(); options.projectStart = -1; REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   options = Options(); options.camera = "invalid\nname"; REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   options = Options(); options.camera.clear(); REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   options = Options(); options.destination = "relative.mkv"; REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   options = Options(); options.destination.replace_extension(".aup3"); REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   options = Options(); options.maximumHeight = 0; REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   options = Options(); options.source = SourceKind::Screen;
   REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
}

TEST_CASE("Generated capture stays private and MP4 stays streamable after interruption") {
   auto options = Options(); options.source = SourceKind::GeneratedTest; options.generatedAudio = true;
   options.duration = 1.25; options.destination.replace_extension(".MP4");
   REQUIRE_THROWS(PrepareVideoRecordingArguments(options));
   for (int repeat{}; repeat < 2; ++repeat) {
      const auto args = PrepareVideoRecordingArguments(options, true);
      REQUIRE(Value(args, "-t") == "1.25");
      REQUIRE(Value(args, "-c:a") == "aac");
      REQUIRE(Value(args, "-movflags") == "+frag_keyframe+delay_moov+default_base_moof");
      REQUIRE(std::count(args.begin(), args.end(), "-re") == 2);
      REQUIRE(args.back() == "pipe:1");
   }
}
