#include "EditCaptionCues.h"
#include "RenderVideoComposition.h"
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>

namespace VideoCaptions {
namespace {
constexpr int64_t MaximumTimeMs = 100000000000LL;
void RequireCue(const Cue &cue)
{
   if (cue.startMs < 0 || cue.endMs <= cue.startMs || cue.endMs > MaximumTimeMs ||
       cue.text.empty() || cue.text.size() > 16384 || cue.text.find('\0') != std::string::npos)
      throw std::runtime_error("Each caption needs text and an ordered, non-negative time range.");
}
std::string Trim(const std::string &text)
{
   const auto first = text.find_first_not_of(" \t\r\n");
   return first == std::string::npos ? std::string{} : text.substr(first, text.find_last_not_of(" \t\r\n")-first+1);
}
int64_t ReadTimestamp(std::string value)
{
   value = Trim(value);
   const auto space = value.find_first_of(" \t");
   if (space != std::string::npos) value.resize(space); // WebVTT cue settings.
   std::replace(value.begin(), value.end(), ',', '.');
   std::vector<std::string> parts;
   std::istringstream fields{value}; std::string field;
   while (std::getline(fields, field, ':')) parts.push_back(field);
   if (parts.size() != 2 && parts.size() != 3)
      throw std::runtime_error("A subtitle has an invalid timestamp.");
   const auto integer = [](const std::string &part) -> int64_t {
      if (part.empty() || part.size() > 8 || part.find_first_not_of("0123456789") != std::string::npos)
         throw std::runtime_error("A subtitle timestamp contains an invalid number.");
      return std::stoll(part);
   };
   const auto seconds = parts.back(); const auto dot = seconds.find('.');
   if (dot == std::string::npos || seconds.size()-dot != 4)
      throw std::runtime_error("Subtitle timestamps need three millisecond digits.");
   const auto sec = integer(seconds.substr(0, dot)), ms = integer(seconds.substr(dot+1));
   const auto minute = integer(parts[parts.size()-2]);
   const auto hour = parts.size() == 3 ? integer(parts.front()) : 0;
   const auto result = ((hour*60+minute)*60+sec)*1000+ms;
   if (minute >= 60 || sec >= 60 || result > MaximumTimeMs)
      throw std::runtime_error("A subtitle timestamp is outside supported bounds.");
   return result;
}
std::string ReadPlainText(const std::string &value)
{
   // Captions currently have one style per clip. Keep words and line breaks;
   // presentation tags, speaker tags and inline WebVTT timestamps are omitted.
   std::string plain;
   for (size_t i{}; i < value.size(); ++i) {
      if (value[i] == '<') {
         const auto end = value.find('>', i+1);
         if (end != std::string::npos) {
            auto tag = value.substr(i+1, end-i-1);
            if (!tag.empty() && tag.front() == '/') tag.erase(0, 1);
            const auto name = tag.substr(0, tag.find_first_of(" .\t"));
            if (name == "b" || name == "i" || name == "u" || name == "font" || name == "v" ||
                name == "c" || name == "lang" || name == "ruby" || name == "rt" ||
                (!name.empty() && name.front() >= '0' && name.front() <= '9' && name.find(':') != std::string::npos)) {
               i = end; continue;
            }
         }
      }
      if (value[i] == '&') {
         const std::pair<const char*, const char*> entities[] = {
            {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"&quot;", "\""}, {"&apos;", "'"}};
         bool found{};
         for (const auto &entity : entities) {
            const std::string name{entity.first};
            if (value.compare(i, name.size(), name) == 0) {
               plain += entity.second; i += name.size()-1; found = true; break;
            }
         }
         if (found) continue;
      }
      plain += value[i];
   }
   return Trim(plain);
}
std::string EscapeText(const std::string &text)
{
   std::string result;
   for (const auto c : text) switch (c) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '\r': break;
      default: result += c;
   }
   return result;
}
std::string Timestamp(int64_t ms, char separator)
{
   std::ostringstream text; text.imbue(std::locale::classic());
   text << std::setfill('0') << std::setw(2) << ms/3600000 << ':'
      << std::setw(2) << ms/60000%60 << ':' << std::setw(2) << ms/1000%60
      << separator << std::setw(3) << ms%1000;
   return text.str();
}
struct TextRange { double from{}, to{}; std::string text; };
std::vector<TextRange> ReadClipTextRanges(const VideoTimeline::Clip &clip)
{
   using Operation = VideoTimeline::VideoClipContent::Operation;
   if (clip.muted || !clip.content) return {};
   const auto &content = *clip.content;
   if (content.operation == Operation::Text) return {{clip.start, clip.End(), content.title}};
   std::vector<TextRange> result;
   for (const auto &part : content.parts) for (auto range : ReadClipTextRanges(part)) {
      if (content.operation == Operation::Reverse) {
         // The picture worker reverses integer frames. Caption boundaries use
         // the same outward frame clock, including sub-frame source edits.
         const auto frames = std::ceil(content.duration*content.frameRate-1e-8);
         const auto from = (frames-std::ceil(range.to*content.frameRate-1e-8))/content.frameRate;
         range.to = (frames-std::ceil(range.from*content.frameRate-1e-8))/content.frameRate;
         range.from = from;
      }
      else if (content.operation == Operation::Freeze) {
         const auto held = std::floor(content.heldTime*content.frameRate+1e-8)/content.frameRate;
         if (held < range.from || held >= range.to) continue;
         range.from = 0; range.to = content.duration;
      }
      range.from = std::max(clip.start, clip.ProjectTimeAtContentTime(range.from));
      range.to = std::min(clip.End(), clip.ProjectTimeAtContentTime(range.to));
      if (range.to > range.from) result.push_back(std::move(range));
      if (result.size() > VideoTimeline::MaximumVideoTrackClips) throw std::runtime_error("Too many retained caption pieces.");
   }
   return result;
}
}

std::vector<Cue> ReadCaptionCues(const std::string &utf8, FileFormat format)
{
   if (utf8.size() > MaximumCaptionFileBytes || utf8.find('\0') != std::string::npos)
      throw std::runtime_error("The subtitle file is too large or is not UTF-8 text.");
   auto input = utf8;
   if (input.compare(0, 3, "\xef\xbb\xbf") == 0) input.erase(0, 3);
   input.erase(std::remove(input.begin(), input.end(), '\r'), input.end());
   std::istringstream stream{input}; std::string line;
   if (format == FileFormat::WebVtt) {
      if (!std::getline(stream, line) || (line != "WEBVTT" && line.rfind("WEBVTT ", 0) != 0))
         throw std::runtime_error("A WebVTT subtitle file must start with WEBVTT.");
      while (std::getline(stream, line) && !Trim(line).empty())
         if (line.rfind("X-TIMESTAMP-MAP", 0) == 0)
            throw std::runtime_error("Import a standalone WebVTT file without a streaming timestamp map.");
   }
   std::vector<Cue> result;
   while (std::getline(stream, line)) {
      if (Trim(line).empty()) continue;
      if (format == FileFormat::WebVtt && (line == "NOTE" || line.rfind("NOTE ", 0) == 0 || line == "STYLE" || line == "REGION")) {
         while (std::getline(stream, line) && !Trim(line).empty()) {}
         continue;
      }
      if (line.find("-->") == std::string::npos && !std::getline(stream, line))
         throw std::runtime_error("A subtitle cue is missing its timestamp line.");
      const auto arrow = line.find("-->");
      if (arrow == std::string::npos) throw std::runtime_error("A subtitle cue is missing its time range.");
      Cue cue{ReadTimestamp(line.substr(0, arrow)), ReadTimestamp(line.substr(arrow+3)), {}};
      while (std::getline(stream, line) && !Trim(line).empty()) {
         if (!cue.text.empty()) cue.text += '\n'; cue.text += line;
      }
      cue.text = ReadPlainText(cue.text); RequireCue(cue);
      result.push_back(std::move(cue));
      if (result.size() > VideoTimeline::MaximumVideoTrackClips)
         throw std::runtime_error("The subtitle file contains too many captions for one track.");
   }
   return result;
}
std::string WriteCaptionCues(const std::vector<Cue> &cues, FileFormat format)
{
   if (cues.size() > VideoTimeline::MaximumVideoTrackClips)
      throw std::runtime_error("There are too many captions to export.");
   std::string result = format == FileFormat::WebVtt ? "WEBVTT\n\n" : "";
   size_t number{};
   for (const auto &cue : cues) {
      RequireCue(cue);
      if (cue.text.find("\n\n") != std::string::npos)
         throw std::runtime_error("Remove blank lines inside a caption before subtitle export; blank lines separate subtitle cues.");
      if (format == FileFormat::Srt) result += std::to_string(++number)+"\n";
      const auto separator = format == FileFormat::Srt ? ',' : '.';
      result += Timestamp(cue.startMs, separator)+" --> "+Timestamp(cue.endMs, separator)+"\n"+EscapeText(cue.text)+"\n\n";
      if (result.size() > MaximumCaptionFileBytes) throw std::runtime_error("The subtitle output is too large.");
   }
   return result;
}
std::vector<Cue> CombineCaptionOverlaps(const std::vector<Cue> &cues)
{
   if (cues.size() > VideoTimeline::MaximumVideoTrackClips) throw std::runtime_error("Too many caption cues.");
   std::map<int64_t, std::vector<std::pair<size_t, bool>>> events;
   for (size_t i{}; i < cues.size(); ++i) {
      RequireCue(cues[i]); events[cues[i].startMs].push_back({i, true}); events[cues[i].endMs].push_back({i, false});
   }
   std::vector<Cue> result; std::set<size_t> active;
   for (auto it = events.begin(); it != events.end(); ++it) {
      for (const auto &event : it->second) if (event.second) active.insert(event.first); else active.erase(event.first);
      const auto next = std::next(it);
      if (next == events.end() || active.empty()) continue;
      Cue cue{it->first, next->first, {}};
      for (const auto id : active) { if (!cue.text.empty()) cue.text += '\n'; cue.text += cues[id].text; }
      RequireCue(cue); result.push_back(std::move(cue));
      if (result.size() > VideoTimeline::MaximumVideoTrackClips) throw std::runtime_error("Overlapping subtitles need too many caption clips.");
   }
   return result;
}
std::vector<Cue> PlaceCaptionCues(const std::vector<Cue> &cues, double offset, double from, double to)
{
   if (!std::isfinite(offset) || !std::isfinite(from) || !std::isfinite(to) || from < 0 || to < from ||
       to > MaximumTimeMs/1000.0 || std::abs(offset) > MaximumTimeMs/1000.0)
      throw std::runtime_error("Choose a usable caption time range.");
   const auto delta = std::llround(offset*1000), first = std::llround(from*1000), last = std::llround(to*1000);
   std::vector<Cue> result;
   for (auto cue : cues) {
      RequireCue(cue); cue.startMs = std::max(first, cue.startMs+delta); cue.endMs = std::min(last, cue.endMs+delta);
      if (cue.endMs > cue.startMs) { RequireCue(cue); result.push_back(std::move(cue)); }
   }
   return CombineCaptionOverlaps(result);
}
VideoTimeline::Timeline PlanCaptionClips(const std::vector<Cue> &cues, int width, int height,
   const VideoEditing::VideoTextAppearance &appearance)
{
   VideoTimeline::Timeline result;
   for (const auto &cue : CombineCaptionOverlaps(cues)) {
      result.clips.push_back(VideoTimeline::MakeTextVideoClip(result.nextId++, cue.startMs/1000.0,
         (cue.endMs-cue.startMs)/1000.0, cue.text, width, height, appearance));
   }
   result.sourceWidth = width; result.sourceHeight = height;
   VideoTimeline::RequireWellFormedTimeline(result); return result;
}
std::vector<Cue> ReadTimelineCaptions(const VideoTimeline::Timeline &timeline)
{
   VideoTimeline::RequireWellFormedTimeline(timeline);
   std::vector<Cue> result;
   for (const auto &clip : timeline.clips) {
      for (const auto &range : ReadClipTextRanges(clip)) {
         Cue cue{std::llround(range.from*1000), std::llround(range.to*1000), range.text};
         if (cue.endMs > cue.startMs) { RequireCue(cue); result.push_back(std::move(cue)); }
      }
   }
   return CombineCaptionOverlaps(result);
}
}
