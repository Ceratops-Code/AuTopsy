#include "PlanVideoRecording.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace VideoRecording {
namespace {
void RequireText(const std::string &text, bool required) {
   if ((required && text.empty()) || text.size() > 4096 ||
       text.find_first_of(std::string{"\r\n\0", 3}) != std::string::npos)
      throw std::invalid_argument("Choose a usable capture device identifier.");
}
std::string DeviceToken(const std::string &text) {
   RequireText(text, true);
   std::string result{"'"};
   for (const auto ch : text) result += ch == '\'' ? "'\\''" : std::string(1, ch);
   return result + '\'';
}
std::string Number(double value) {
   std::ostringstream out; out.imbue(std::locale::classic());
   out << std::setprecision(17) << value; return out.str();
}
}

std::vector<CaptureDevice> ReadCaptureDeviceList(const std::string &listing) {
   if (listing.size() > 512 * 1024)
      throw std::runtime_error("The capture device list exceeds supported bounds.");
   std::vector<CaptureDevice> result;
   std::unordered_set<std::string> seen;
   std::istringstream lines{listing}; std::string line;
   while (std::getline(lines, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      DeviceKind kind;
      size_t suffix{};
      if (line.size() >= 8 && line.compare(line.size()-8, 8, " (video)") == 0)
         kind = DeviceKind::Video, suffix = 8;
      else if (line.size() >= 8 && line.compare(line.size()-8, 8, " (audio)") == 0)
         kind = DeviceKind::Audio, suffix = 8;
      else if (line.size() >= 7 && line.compare(line.size()-7, 7, " (none)") == 0)
         kind = DeviceKind::Unknown, suffix = 7;
      else continue;
      const auto end = line.size()-suffix;
      const auto split = line.rfind(" [", end);
      const auto start = line.find_first_not_of(" \t");
      if (split == std::string::npos || start == std::string::npos ||
          split <= start || end <= split+2 || line[end-1] != ']')
         throw std::runtime_error("The capture device list has an unreadable device entry.");
      CaptureDevice device{line.substr(start, split-start), line.substr(split+2, end-split-3), kind};
      RequireText(device.identifier, true); RequireText(device.label, true);
      // A repeated provider listing cannot silently select a different device.
      const auto key = std::to_string(int(kind)) + ":" + device.identifier;
      if (!seen.insert(key).second) continue;
      result.push_back(std::move(device));
      if (result.size() > 256) throw std::runtime_error("Too many capture devices were reported.");
   }
   return result;
}

std::vector<std::string> PrepareVideoRecordingArguments(const RecordingOptions &o, bool allowGeneratedInput) {
   if (!std::isfinite(o.frameRate) || o.frameRate < 1 || o.frameRate > 120 ||
       !std::isfinite(o.duration) || o.duration < 0 || o.duration > 86400 ||
       !std::isfinite(o.projectStart) || o.projectStart < 0 || o.projectStart > 100000000 ||
       o.projectStart + o.duration > 100000000)
      throw std::invalid_argument("Choose a usable recording position, frame rate and duration.");
   if (o.destination.empty() || !o.destination.is_absolute() || o.destination.filename().empty())
      throw std::invalid_argument("Choose an absolute filename for the recording.");
   auto extension = o.destination.extension().u8string();
   std::transform(extension.begin(), extension.end(), extension.begin(),
      [](unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? char(ch-'A'+'a') : char(ch); });
   if (extension != ".mkv" && extension != ".mp4")
      throw std::invalid_argument("Choose an MKV or MP4 recording filename.");
   if ((o.maximumWidth == 0) != (o.maximumHeight == 0) ||
       o.maximumWidth < 0 || o.maximumHeight < 0 || o.maximumWidth > 16384 || o.maximumHeight > 16384 ||
       (o.maximumWidth && (o.maximumWidth < 2 || o.maximumHeight < 2)))
      throw std::invalid_argument("Choose usable recording picture dimensions.");
   RequireText(o.audio, false);
   std::vector<std::string> args{"-hide_banner", "-loglevel", "warning", "-nostats",
      "-progress", "pipe:2", "-stats_period", "0.25"};
   const auto add = [&](std::initializer_list<std::string> values) {
      args.insert(args.end(), values.begin(), values.end());
   };
   bool audio = !o.audio.empty();
   int audioInput{};
   switch (o.source) {
   case SourceKind::Webcam: {
      // One DirectShow input shares its hardware clock with the microphone.
      auto input = "video=" + DeviceToken(o.camera);
      if (audio) input += ":audio=" + DeviceToken(o.audio);
      add({"-thread_queue_size", "64", "-f", "dshow", "-rtbufsize", "128M", "-i", input});
      break;
   }
   case SourceKind::Screen:
   case SourceKind::Window:
      add({"-thread_queue_size", "64", "-use_wallclock_as_timestamps", "1", "-f", "gdigrab",
         "-framerate", Number(o.frameRate), "-draw_mouse", o.includePointer ? "1" : "0"});
      if (o.source == SourceKind::Screen) {
         if (o.region.width < 2 || o.region.height < 2 || o.region.width > 32768 || o.region.height > 32768 ||
             std::abs(int64_t(o.region.left)) > 131072 || std::abs(int64_t(o.region.top)) > 131072)
            throw std::invalid_argument("The selected display has no usable recording rectangle.");
         add({"-offset_x", std::to_string(o.region.left), "-offset_y", std::to_string(o.region.top),
            "-video_size", std::to_string(o.region.width)+"x"+std::to_string(o.region.height), "-i", "desktop"});
      }
      else {
         if (!o.window) throw std::invalid_argument("The selected recording window is unavailable.");
         add({"-i", "hwnd=" + std::to_string(o.window)});
      }
      if (audio) {
         audioInput = 1;
         add({"-thread_queue_size", "64", "-use_wallclock_as_timestamps", "1", "-isync", "0",
            "-f", "dshow", "-i", "audio=" + DeviceToken(o.audio)});
      }
      break;
   case SourceKind::GeneratedTest:
      if (!allowGeneratedInput)
         throw std::invalid_argument("Generated recording input requires a marked private test project.");
      audio = o.generatedAudio; audioInput = 1;
      add({"-re", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=" + Number(o.frameRate)});
      if (audio) add({"-re", "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000"});
      break;
   default: throw std::invalid_argument("Choose a supported video recording source.");
   }
   add({"-map", "0:v:0", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "23",
      "-pix_fmt", "yuv420p", "-r", Number(o.frameRate), "-g", std::to_string(int(std::ceil(o.frameRate))),
      "-threads", "2"});
   if (o.maximumWidth) add({"-vf", "scale=w='min(iw,"+std::to_string(o.maximumWidth)+")':h='min(ih,"+
      std::to_string(o.maximumHeight)+")':force_original_aspect_ratio=decrease:force_divisible_by=2"});
   else add({"-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2"});
   if (audio) {
      add({"-map", std::to_string(audioInput)+":a:0", "-c:a", extension == ".mkv" ? "pcm_s16le" : "aac"});
      if (extension == ".mp4") add({"-b:a", "192k"});
   }
   else add({"-an"});
   if (o.duration > 0) add({"-t", Number(o.duration)});
   // Both containers remain readable without a seekable output or final index.
   // The process owner periodically flushes the already-created output handle.
    // Delay the header until the first fragment so AAC encoder priming can be
    // represented by an edit list. An empty header stretches the first picture
    // interval and reports the wrong average frame rate for otherwise CFR input.
    if (extension == ".mp4") add({"-movflags", "+frag_keyframe+delay_moov+default_base_moof", "-f", "mp4"});
   else add({"-cluster_time_limit", "1000", "-f", "matroska"});
   add({"-flush_packets", "1", "pipe:1"});
   return args;
}
}
