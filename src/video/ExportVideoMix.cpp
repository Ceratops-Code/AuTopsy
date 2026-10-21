#include "ExportVideoMix.h"
#include "Export.h"
#include "ExportPluginRegistry.h"
#include "Prefs.h"
#include "MemoryX.h"
#include <sndfile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/progdlg.h>
#include <wx/stdpaths.h>
#include <wx/msw/wrapwin.h>
#include <rapidjson/document.h>
#include <optional>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace {
std::string Utf8(const wxString &text) {
   const auto bytes = text.ToUTF8();
   return { bytes.data(), bytes.length() };
}
}
namespace VideoExport {
namespace {
namespace fs = std::filesystem;
constexpr auto WorkspaceKey = "/VideoExport/Workspace";
constexpr auto OwnerSignature = "AuTopsy video export 1";

struct CloseNativeHandle {
   void operator()(void *handle) const noexcept
   { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using NativeHandle = std::unique_ptr<void, CloseNativeHandle>;

std::runtime_error WindowsError(const std::string &operation)
{
   return std::runtime_error(operation + " (Windows error " +
      std::to_string(GetLastError()) + ").");
}

// There is at most one pending workspace per settings profile. The owner file
// is exclusively locked while live. Cleanup removes only the named files,
// never an arbitrary tree or an output supplied by the user.
wxString CleanWorkspace()
{
   wxString stored;
   if (!gPrefs->Read(WorkspaceKey, &stored) || stored.empty()) return {};
   const fs::path path{ stored.ToStdWstring() };
   std::error_code error;
   const auto clear = [] {
      gPrefs->DeleteEntry(WorkspaceKey);
      gPrefs->Flush();
   };
   if (!fs::exists(path, error) && !error) { clear(); return {}; }
   const auto name = path.filename().wstring();
   const auto attributes = GetFileAttributesW(path.c_str());
   if (error || !path.is_absolute() ||
      name.rfind(L"_autopsy-video-export-", 0) != 0 ||
      attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return _("The pending video export workspace could not be safely opened: ") + stored;

   const auto ownerPath = path / "_owner";
   NativeHandle owner{ CreateFileW(ownerPath.c_str(), GENERIC_READ, 0, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
   if (owner.get() == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND && fs::is_empty(path, error) && !error) {
         fs::remove(path, error);
         if (!error) { clear(); return {}; }
      }
      return _("Another export is using its workspace, or cleanup was denied: ") + stored;
   }
   char signature[64]{};
   DWORD read{};
   if (!ReadFile(owner.get(), signature, sizeof(signature) - 1, &read, nullptr) ||
      std::string(OwnerSignature).compare(0, read, signature, read) != 0)
      return _("The pending export workspace has an unknown owner: ") + stored;
   for (const auto *file : { "_mix.wav", "_filters.txt", "_result.mp4", "_result.mkv" }) {
      fs::remove(path / file, error);
      if (error) return _("Could not remove a temporary video export file: ") + stored;
   }
   owner.reset();
   fs::remove(ownerPath, error);
   if (!error) fs::remove(path, error); // Refuse a nonempty directory.
   if (error) return _("Could not finish video export cleanup: ") + stored;
   clear();
   return {};
}

class ExportWorkspace final {
   NativeHandle mOwner;
public:
   fs::path path;
   explicit ExportWorkspace(const fs::path &destination)
   {
      try {
      const auto issue = CleanWorkspace();
      if (!issue.empty()) throw std::runtime_error(Utf8(issue));
      std::random_device random;
      for (unsigned attempt = 0; attempt < 32; ++attempt) {
         std::wostringstream name;
         name << L"_autopsy-video-export-" << std::hex << random() << random();
         path = destination.parent_path() / name.str();
         if (fs::exists(path)) continue;
         // Record ownership before the first filesystem effect, so a new run
         // can remove an interrupted creation using current filesystem state.
         if (!gPrefs->Write(WorkspaceKey, wxString(path.wstring())) || !gPrefs->Flush())
            throw std::runtime_error("Could not record the video export workspace.");
         if (CreateDirectoryW(path.c_str(), nullptr)) break;
         if (GetLastError() != ERROR_ALREADY_EXISTS)
            throw WindowsError("Creating the video export workspace");
      }
      mOwner.reset(CreateFileW((path / "_owner").c_str(), GENERIC_READ | GENERIC_WRITE,
         0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
      if (mOwner.get() == INVALID_HANDLE_VALUE) throw WindowsError("Locking the video export workspace");
      DWORD written{};
      if (!WriteFile(mOwner.get(), OwnerSignature, DWORD(strlen(OwnerSignature)), &written, nullptr) ||
         written != strlen(OwnerSignature) || !FlushFileBuffers(mOwner.get()))
         throw WindowsError("Marking the video export workspace");
      }
      catch (...) {
         mOwner.reset();
         CleanWorkspace();
         throw;
      }
   }
   ~ExportWorkspace() { mOwner.reset(); CleanWorkspace(); }
};

class MixProgress final : public ExportProcessorDelegate {
   wxProgressDialog mDialog;
   bool mCancelled{};
public:
   explicit MixProgress(wxWindow *parent)
      : mDialog{ _("Export Video with Audio Mix"), _("Rendering the project audio mix..."),
         1000, parent, wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE } {}
   bool IsCancelled() const override { return mCancelled; }
   bool IsStopped() const override { return false; }
   void SetStatusString(const TranslatableString &) override {}
   void OnProgress(double value) override
   { mCancelled = !mDialog.Update(int(std::clamp(value, 0.0, 1.0) * 1000)); }
};

// Quote a single CreateProcess argument using the Windows C runtime rules.
// No command shell is involved, including for filenames containing metacharacters.
std::wstring QuoteArgument(const std::wstring &argument)
{
   std::wstring result{ L"\"" };
   size_t slashes{};
   for (const auto ch : argument) {
      if (ch == L'\\') { ++slashes; continue; }
      result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
      slashes = 0;
      if (ch == L'"') result += L'\\';
      result += ch;
   }
   result.append(slashes * 2, L'\\');
   return result + L'"';
}

std::string DescribeProcessOutput(const std::string &text)
{
   if (text.empty()) return "<empty>";
   std::istringstream lines{ text };
   std::string line, result;
   while (std::getline(lines, line)) {
      if (!result.empty()) result += '\n';
      // Blank lines delimit scripting replies. Indenting every child-output
      // line retains diagnostics without accidentally ending the response.
      result += "  " + line;
   }
   return result;
}

// The process is placed in a kill-on-close job before it can run. Cancellation,
// exceptions and termination of AuTopsy cannot leave an encoder running.
class FfmpegProcess final {
   NativeHandle mJob, mProcess, mOutputRead, mErrorRead;
public:
   std::wstring invocation;
   std::string output, errors;
   explicit FfmpegProcess(const std::vector<std::wstring> &arguments)
   {
      for (const auto &argument : arguments) {
         if (!invocation.empty()) invocation += L' ';
         invocation += QuoteArgument(argument);
      }
      SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
      HANDLE outputRead{}, outputWrite{}, errorRead{}, errorWrite{};
      if (!CreatePipe(&outputRead, &outputWrite, &security, 0)) throw WindowsError("Creating FFmpeg output pipe");
      mOutputRead.reset(outputRead);
      NativeHandle outputWriter{ outputWrite };
      if (!CreatePipe(&errorRead, &errorWrite, &security, 0)) throw WindowsError("Creating FFmpeg error pipe");
      mErrorRead.reset(errorRead);
      NativeHandle errorWriter{ errorWrite };
      SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);
      SetHandleInformation(errorRead, HANDLE_FLAG_INHERIT, 0);
      NativeHandle input{ CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
         &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
      if (input.get() == INVALID_HANDLE_VALUE) throw WindowsError("Opening FFmpeg input");

      SIZE_T attributeSize{};
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
      std::vector<unsigned char> attributes(attributeSize);
      auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
      if (!InitializeProcThreadAttributeList(list, 1, 0, &attributeSize))
         throw WindowsError("Preparing FFmpeg handles");
      const auto cleanup = finally([&] { DeleteProcThreadAttributeList(list); });
      HANDLE inherited[]{ outputWrite, errorWrite, input.get() };
      if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
         inherited, sizeof(inherited), nullptr, nullptr)) throw WindowsError("Restricting FFmpeg handles");
      STARTUPINFOEXW startup{};
      startup.StartupInfo.cb = sizeof(startup);
      startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
      startup.StartupInfo.hStdInput = input.get();
      startup.StartupInfo.hStdOutput = outputWrite;
      startup.StartupInfo.hStdError = errorWrite;
      startup.lpAttributeList = list;
      mJob.reset(CreateJobObjectW(nullptr, nullptr));
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!mJob || !SetInformationJobObject(mJob.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
         throw WindowsError("Preparing the FFmpeg process job");
      PROCESS_INFORMATION process{};
      auto commandLine = invocation;
      if (!CreateProcessW(arguments.front().c_str(), commandLine.data(), nullptr, nullptr,
         TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
         nullptr, nullptr, &startup.StartupInfo, &process)) throw WindowsError("Starting bundled FFmpeg");
      mProcess.reset(process.hProcess);
      NativeHandle thread{ process.hThread };
      if (!AssignProcessToJobObject(mJob.get(), mProcess.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
         TerminateProcess(mProcess.get(), 1);
         WaitForSingleObject(mProcess.get(), INFINITE);
         throw WindowsError("Assigning the FFmpeg process job");
      }
   }
   ~FfmpegProcess()
   {
      mJob.reset();
      if (mProcess) WaitForSingleObject(mProcess.get(), INFINITE);
   }
   static void Drain(HANDLE pipe, std::string &text)
   {
      DWORD available{};
      size_t budget = 256 * 1024;
      while (budget && PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
         char buffer[4096]; DWORD received{};
         if (!ReadFile(pipe, buffer, DWORD(std::min<size_t>({ sizeof(buffer), available, budget })), &received, nullptr) || !received) break;
         text.append(buffer, received);
         if (text.size() > 16384) text.erase(0, text.size() - 16384);
         budget -= received;
      }
   }
   bool Wait(wxWindow *parent,
      const wxString &title = _("Export Video with Audio Mix"),
      const wxString &status = _("Writing video with the edited audio..."))
   {
      wxProgressDialog progress{ title, status,
         100, parent, wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME };
      while (true) {
         Drain(mOutputRead.get(), output); Drain(mErrorRead.get(), errors);
         const auto state = WaitForSingleObject(mProcess.get(), 20);
         if (!progress.Pulse()) return false;
         if (state == WAIT_OBJECT_0) break;
         if (state == WAIT_FAILED) throw WindowsError("Waiting for FFmpeg");
      }
      Drain(mOutputRead.get(), output); Drain(mErrorRead.get(), errors);
      DWORD exit{};
      if (!GetExitCodeProcess(mProcess.get(), &exit)) throw WindowsError("Reading the FFmpeg exit status");
      if (exit != 0)
         throw std::runtime_error("FFmpeg operation failed (exit " + std::to_string(exit) + ").\nCommand: " +
            Utf8(wxString(invocation)) + "\nstdout:\n" + DescribeProcessOutput(output) +
            "\nstderr:\n" + DescribeProcessOutput(errors) +
            "\nIf this video codec cannot be stored in MP4, try MKV.");
      return true;
   }
};

// FFprobe supplies structured source dimensions, including display rotation.
// Export uses the first visible original as its canvas, never its small proxy.
std::optional<std::pair<int, int>> ReadExportPictureSize(const fs::path &source, wxWindow *parent)
{
   const auto probe = wxFileName(wxStandardPaths::Get().GetExecutablePath())
      .GetPathWithSep() + "ffprobe.exe";
   if (!wxFileExists(probe)) throw std::runtime_error("The bundled ffprobe.exe is missing. Reinstall AuTopsy.");
   FfmpegProcess process{{probe.ToStdWstring(), L"-v", L"error", L"-select_streams", L"v:0",
      L"-show_entries", L"stream=width,height:stream_side_data=rotation:stream_tags=rotate",
      L"-of", L"json", source.wstring()}};
   if (!process.Wait(parent, _("Reading Video"), _("Reading original picture dimensions..."))) return {};
   rapidjson::Document data; data.Parse(process.output.c_str());
   if (data.HasParseError() || !data.IsObject() || !data.HasMember("streams") ||
       !data["streams"].IsArray() || data["streams"].Empty())
      throw std::runtime_error("FFprobe did not report a video stream.");
   const auto &stream = data["streams"][0];
   if (!stream.HasMember("width") || !stream["width"].IsInt() ||
       !stream.HasMember("height") || !stream["height"].IsInt())
      throw std::runtime_error("The video stream has no usable picture dimensions.");
   int width = stream["width"].GetInt(), height = stream["height"].GetInt(), rotation{};
   if (stream.HasMember("side_data_list") && stream["side_data_list"].IsArray())
      for (const auto &side : stream["side_data_list"].GetArray())
         if (side.IsObject() && side.HasMember("rotation") && side["rotation"].IsNumber())
            rotation = int(std::round(side["rotation"].GetDouble()));
   if (rotation == 0 && stream.HasMember("tags") && stream["tags"].IsObject() &&
       stream["tags"].HasMember("rotate") && stream["tags"]["rotate"].IsString())
      rotation = std::atoi(stream["tags"]["rotate"].GetString());
   if (std::abs(rotation) % 180 == 90) std::swap(width, height);
   if (width <= 0 || height <= 0 || width > 65536 || height > 65536)
      throw std::runtime_error("The original video dimensions are outside supported bounds.");
   return std::pair<int,int>{(width + 1) & ~1, (height + 1) & ~1};
}
} // namespace

// Called when the project attachments start, and before every export.
void CleanAbandonedWorkspace()
{
   const auto issue = CleanWorkspace();
   if (!issue.empty()) wxLogWarning("%s", issue);
}

bool RunBundledVideoTool(const std::vector<wxString> &arguments, wxWindow *parent,
   const wxString &title, const wxString &status)
{
   const auto executable = wxFileName(wxStandardPaths::Get().GetExecutablePath())
      .GetPathWithSep() + "ffmpeg.exe";
   if (!wxFileExists(executable))
      throw std::runtime_error("The bundled ffmpeg.exe is missing. Reinstall AuTopsy.");
   std::vector<std::wstring> native{executable.ToStdWstring()};
   for (const auto &argument : arguments) native.push_back(argument.ToStdWstring());
   FfmpegProcess process{native};
   return process.Wait(parent, title, status);
}

bool ExportMix(AudacityProject &project, const VideoTimeline::Timeline &timeline,
   const wxString &destination, bool overwrite, wxWindow *parent)
{
   VideoTimeline::RequireWellFormedTimeline(timeline);
   const auto duration = timeline.End();
   const auto copyPicture = VideoTimeline::KeepsOriginalPicture(timeline);
   const fs::path target = fs::absolute(fs::path(destination.ToStdWstring()));
   if (!std::isfinite(duration) || duration <= 0 || timeline.clips.empty() || destination.empty())
      throw std::runtime_error("Choose video clips and an output filename before exporting.");
   const auto extension = wxString(target.extension().wstring()).Lower();
   if (extension != ".mp4" && extension != ".mkv")
      throw std::runtime_error("Choose an .mp4 or .mkv output filename.");
   if (!fs::is_directory(target.parent_path()))
      throw std::runtime_error("The output folder is unavailable.");
   std::vector<fs::path> inputs;
   std::vector<size_t> clipInputs;
   const auto protectSource = [&](const fs::path &input) {
      if (wxString(input.wstring()).CmpNoCase(wxString(target.wstring())) == 0 ||
          (fs::exists(target) && fs::exists(input) && fs::equivalent(input, target)))
         throw std::runtime_error("The output must be different from every source video.");
   };
   for (const auto &path : timeline.protectedSourcePaths)
      protectSource(fs::absolute(fs::path(wxString::FromUTF8(path).ToStdWstring())));
   for (const auto &clip : timeline.clips) {
      const auto source = VideoTimeline::SourceFor(timeline, clip);
      const auto input = fs::absolute(fs::path(wxString::FromUTF8(source.path).ToStdWstring()));
      if (!fs::is_regular_file(input))
         throw std::runtime_error("A source video is unavailable: " + source.path);
      protectSource(input);
      const auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto &path) {
         return wxString(input.wstring()).CmpNoCase(wxString(path.wstring())) == 0;
      });
      clipInputs.push_back(size_t(found - inputs.begin()));
      if (found == inputs.end()) inputs.push_back(input);
   }
   if (fs::exists(target) && !overwrite)
      throw std::runtime_error("The output already exists. Choose another name or allow overwrite.");
   const auto executable = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPathWithSep() + "ffmpeg.exe";
   if (!wxFileExists(executable)) throw std::runtime_error("The bundled ffmpeg.exe is missing. Reinstall AuTopsy.");
   const auto [plugin, format] = ExportPluginRegistry::Get().FindFormat("WAV");
   if (!plugin) throw std::runtime_error("The WAV export module is unavailable.");

   std::optional<std::pair<int,int>> pictureSize;
   if (!copyPicture) {
      pictureSize = ReadExportPictureSize(inputs.front(), parent);
      if (!pictureSize) return false;
   }
   ExportWorkspace workspace{ target };
   const auto mix = workspace.path / "_mix.wav";
   const auto result = workspace.path / (extension == ".mp4" ? "_result.mp4" : "_result.mkv");
   {
      // A floating-point intermediate retains mix headroom. The existing mixer
      // supplies track/master effects, time stretching, mute/solo, gain and pan.
      auto task = ExportTaskBuilder{}.SetPlugin(plugin, format)
         .SetFileName(wxFileName(wxString(mix.wstring())))
         .SetRange(0.0, duration, false).SetNumChannels(2).SetSampleRate(48000)
         .SetParameters({ { SF_FORMAT_WAV, int(SF_FORMAT_FLOAT) } }).Build(project);
      MixProgress progress{ parent };
      auto outcome = task.get_future();
      task(progress);
      if (outcome.get() != ExportResult::Success) return false;
   }
   std::wostringstream seconds;
   seconds.imbue(std::locale::classic());
   seconds << std::setprecision(17) << duration;
   std::vector<std::wstring> arguments{
       executable.ToStdWstring(), L"-hide_banner", L"-loglevel", L"error", L"-nostdin", L"-nostats", L"-n",
       L"-progress", L"pipe:1"
    };
   for (const auto &input : inputs)
      arguments.insert(arguments.end(), {L"-i", input.wstring()});
   arguments.insert(arguments.end(), {L"-i", mix.wstring()});
   if (copyPicture) {
      arguments.insert(arguments.end(), {L"-map", L"0:v:0", L"-c:v", L"copy"});
   }
   else {
      // FFmpeg owns decoding, trimming, gap frames and concatenation. This
      // adapter translates only our value model, never Audacity track internals.
      struct Span { size_t input; double sourceStart, duration; bool gap; };
      std::vector<Span> spans;
      double end{};
      for (size_t clipIndex = 0; clipIndex < timeline.clips.size(); ++clipIndex) {
         const auto &clip = timeline.clips[clipIndex];
         if (clip.start > end + 1e-7) spans.push_back({0, 0, clip.start - end, true});
         spans.push_back({clipInputs[clipIndex], clip.sourceStart, clip.duration, false});
         end = clip.End();
      }
      std::ostringstream graph;
      graph.imbue(std::locale::classic()); graph << std::setprecision(17);
      for (size_t input = 0; input < inputs.size(); ++input) {
         const auto count = std::count_if(spans.begin(), spans.end(),
            [&](const auto &span) { return span.input == input; });
         if (!count) continue;
         graph << "[" << input << ":v:0]split=" << count;
         for (size_t i = 0; i < spans.size(); ++i)
            if (spans[i].input == input) graph << "[s" << i << "]";
         graph << ";\n";
      }
      for (size_t i = 0; i < spans.size(); ++i) {
         const auto &span = spans[i];
         graph << "[s" << i << "]";
         if (span.gap)
            graph << "trim=end_frame=1,setpts=PTS-STARTPTS,tpad=stop_mode=clone:stop_duration="
               << span.duration << ",trim=duration=" << span.duration << ",drawbox=color=black:t=fill";
         else graph << "trim=start=" << span.sourceStart << ":duration=" << span.duration
            << ",setpts=PTS-STARTPTS";
         const auto [width, height] = *pictureSize;
         graph << ",fps=" << timeline.frameRate
            << ",scale=" << width << ":" << height << ":force_original_aspect_ratio=decrease:force_divisible_by=2"
            << ",pad=" << width << ":" << height << ":(ow-iw)/2:(oh-ih)/2,setsar=1,format=yuv420p[v" << i << "];\n";
      }
      for (size_t i = 0; i < spans.size(); ++i) graph << "[v" << i << "]";
      graph << "concat=n=" << spans.size() << ":v=1:a=0[vout]\n";
      const auto filterPath = workspace.path / "_filters.txt";
      std::ofstream filterFile(filterPath, std::ios::binary);
      const auto script = graph.str();
      filterFile.write(script.data(), script.size()); filterFile.close();
      if (!filterFile) throw std::runtime_error("Could not write the temporary video rendering instructions.");
      std::wostringstream frameRate; frameRate.imbue(std::locale::classic());
      frameRate << std::setprecision(17) << timeline.frameRate;
      // MPEG-4 Part 2 is supplied by the bundled LGPL build without a GPU or
      // extra encoder installation. Unchanged pictures always use stream copy.
      arguments.insert(arguments.end(), {L"-filter_complex_script", filterPath.wstring(),
         L"-map", L"[vout]", L"-c:v", L"mpeg4", L"-q:v", L"2", L"-r", frameRate.str()});
   }
   const auto audioMap = std::to_wstring(inputs.size()) + L":a:0";
   arguments.insert(arguments.end(), {L"-map", audioMap, L"-map_metadata", L"0",
      L"-map_chapters", copyPicture ? L"0" : L"-1", L"-c:a", L"aac", L"-b:a", L"256k",
      L"-af", L"apad", L"-t", seconds.str()});
   if (extension == ".mp4") { arguments.push_back(L"-movflags"); arguments.push_back(L"+faststart"); }
   arguments.push_back(result.wstring());
   {
      FfmpegProcess encoder{ arguments };
      if (!encoder.Wait(parent)) return false;
   }
   if (!fs::is_regular_file(result) || fs::file_size(result) == 0)
      throw std::runtime_error("FFmpeg did not produce a video file.");
   // Staging is on the destination volume. Publishing is a single rename;
   // a failed encoder or a cancellation cannot overwrite a prior export.
   if (!MoveFileExW(result.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH |
      (overwrite ? MOVEFILE_REPLACE_EXISTING : 0))) throw WindowsError("Publishing the exported video");
   return true;
}
} // namespace VideoExport
