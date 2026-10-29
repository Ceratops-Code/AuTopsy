#include "ExportVideoMix.h"
#include "RenderVideoComposition.h"
#include "PlanVideoExport.h"
#include "PrepareVideoEffects.h"
#include "PrepareCaptionModels.h"
#include "Export.h"
#include "ExportPluginRegistry.h"
#include "Prefs.h"
#include "MemoryX.h"
#include <sndfile.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/progdlg.h>
#include <wx/generic/progdlgg.h>
#include <wx/stdpaths.h>
#include <wx/msw/wrapwin.h>
#include <rapidjson/document.h>
#include <optional>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <map>
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
struct CloseRecordingFile {
   void operator()(void *handle) const noexcept {
      if (!handle || handle == INVALID_HANDLE_VALUE) return;
      LARGE_INTEGER size{};
      // Mark the opened file itself, rather than deleting a potentially reused
      // pathname. Completed/partial media is user content and remains recoverable.
      if (GetFileSizeEx(handle, &size) && size.QuadPart == 0) {
         FILE_DISPOSITION_INFO remove{TRUE};
         SetFileInformationByHandle(handle, FileDispositionInfo, &remove, sizeof(remove));
      }
      else FlushFileBuffers(handle);
      CloseHandle(handle);
   }
};

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
class BundledMediaProcess final {
   NativeHandle mJob, mProcess, mOutputRead, mErrorRead;
   NativeHandle mInputWrite;
   std::unique_ptr<void, CloseRecordingFile> mRecordingFile;
   size_t mOutputLimit;
   bool mOutputOverflow{};
public:
   std::wstring invocation;
   std::string output, errors;
   explicit BundledMediaProcess(const std::vector<std::wstring> &arguments, size_t outputLimit = 16384,
      const std::wstring &recordingPath = {})
      : mOutputLimit(outputLimit)
   {
      if (!outputLimit || outputLimit > 8 * 1024 * 1024)
         throw std::runtime_error("The media tool output limit is outside supported bounds.");
      for (const auto &argument : arguments) {
         if (!invocation.empty()) invocation += L' ';
         invocation += QuoteArgument(argument);
      }
      SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
      HANDLE outputRead{}, outputWrite{}, errorRead{}, errorWrite{};
      NativeHandle outputWriter;
      if (recordingPath.empty()) {
         if (!CreatePipe(&outputRead, &outputWrite, &security, 0)) throw WindowsError("Creating FFmpeg output pipe");
         mOutputRead.reset(outputRead); outputWriter.reset(outputWrite);
         if (!SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0)) throw WindowsError("Protecting FFmpeg output pipe");
      }
      else {
         mRecordingFile.reset(CreateFileW(recordingPath.c_str(), GENERIC_WRITE | DELETE,
            FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
         if (mRecordingFile.get() == INVALID_HANDLE_VALUE)
            throw WindowsError("Creating a new recording file (existing files are never replaced)");
         outputWrite = mRecordingFile.get();
      }
      if (!CreatePipe(&errorRead, &errorWrite, &security, 0)) throw WindowsError("Creating FFmpeg error pipe");
      mErrorRead.reset(errorRead);
      NativeHandle errorWriter{ errorWrite };
      if (!SetHandleInformation(errorRead, HANDLE_FLAG_INHERIT, 0)) throw WindowsError("Protecting FFmpeg error pipe");
      NativeHandle input;
      if (mRecordingFile) {
         HANDLE reader{}, writer{};
         if (!CreatePipe(&reader, &writer, &security, 0)) throw WindowsError("Creating recording stop pipe");
         input.reset(reader); mInputWrite.reset(writer);
         if (!SetHandleInformation(writer, HANDLE_FLAG_INHERIT, 0)) throw WindowsError("Protecting recording stop pipe");
      }
      else {
         input.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
         if (input.get() == INVALID_HANDLE_VALUE) throw WindowsError("Opening FFmpeg input");
      }

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
   ~BundledMediaProcess()
   {
      mJob.reset();
      if (mProcess) WaitForSingleObject(mProcess.get(), INFINITE);
   }
   static bool Drain(HANDLE pipe, std::string &text, size_t limit = 16384)
   {
      DWORD available{};
      size_t budget = 256 * 1024;
      bool truncated{};
      while (budget && PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
         char buffer[4096]; DWORD received{};
         if (!ReadFile(pipe, buffer, DWORD(std::min<size_t>({ sizeof(buffer), available, budget })), &received, nullptr) || !received) break;
         text.append(buffer, received);
         if (text.size() > limit) { text.erase(0, text.size() - limit); truncated = true; }
         budget -= received;
      }
      return truncated;
   }
   bool Wait(wxWindow *parent,
      const wxString &title = _("Export Video with Audio Mix"),
      const wxString &status = _("Writing video with the edited audio..."), bool pollCancellationAtCompletion = false)
   {
      const auto style = wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME;
      // Windows' task-dialog implementation has a fixed Cancel caption. Capture
      // needs a truthful Stop action; use wx's supported generic implementation.
      std::unique_ptr<wxGenericProgressDialog> dialog;
      if (mRecordingFile) dialog = std::make_unique<wxGenericProgressDialog>(title, status, 100, parent, style);
      else dialog = std::make_unique<wxProgressDialog>(title, status, 100, parent, style);
      auto &progress = *dialog;
      if (mRecordingFile) if (auto *stop = progress.FindWindow(wxID_CANCEL)) stop->SetLabel(_("Stop and Add"));
      using Clock = std::chrono::steady_clock;
      auto lastFlush = Clock::now();
      std::optional<Clock::time_point> stopped;
      while (true) {
         if (mOutputRead) mOutputOverflow |= Drain(mOutputRead.get(), output, mOutputLimit);
         Drain(mErrorRead.get(), errors);
         const auto state = WaitForSingleObject(mProcess.get(), 20);
         if (state == WAIT_OBJECT_0) break;
         if (state == WAIT_FAILED) throw WindowsError("Waiting for FFmpeg");
         if (!progress.Pulse()) {
            if (!mRecordingFile) return false;
            if (!stopped) {
               DWORD sent{};
               if (!WriteFile(mInputWrite.get(), "q\n", 2, &sent, nullptr) || sent != 2)
                  throw WindowsError("Stopping video recording");
               stopped = Clock::now();
            }
            progress.Resume();
         }
         if (mRecordingFile && Clock::now()-lastFlush >= std::chrono::seconds(1)) {
            if (!FlushFileBuffers(mRecordingFile.get())) throw WindowsError("Flushing recorded video");
            lastFlush = Clock::now();
         }
         if (stopped && Clock::now()-*stopped > std::chrono::seconds(10))
            throw std::runtime_error("The recording device did not stop within ten seconds.\nCommand: " +
               Utf8(wxString(invocation)) + "\nstdout: <streamed to recording>\nstderr:\n" + DescribeProcessOutput(errors));
      }
      if (mOutputRead) mOutputOverflow |= Drain(mOutputRead.get(), output, mOutputLimit);
      Drain(mErrorRead.get(), errors);
      if (mRecordingFile && !FlushFileBuffers(mRecordingFile.get())) throw WindowsError("Flushing completed recording");
      DWORD exit{};
      if (!GetExitCodeProcess(mProcess.get(), &exit)) throw WindowsError("Reading the FFmpeg exit status");
      if (exit != 0)
         throw std::runtime_error("Media tool operation failed (exit " + std::to_string(exit) + ").\nCommand: " +
            Utf8(wxString(invocation)) + "\nstdout:\n" + (mRecordingFile ? "  <streamed to recording>" : DescribeProcessOutput(output)) +
            "\nstderr:\n" + DescribeProcessOutput(errors));
      if (mOutputLimit != 16384 && mOutputOverflow)
         throw std::runtime_error("The decoded picture exceeds the media tool's bounded output buffer.");
      // A fast speech worker can finish while a Cancel click is queued. Consume
      // that click before accepting its output; completed recording keeps its
      // separate Stop-and-Add contract.
      if (pollCancellationAtCompletion && !progress.Pulse()) return false;
      return true;
   }
};

// FFprobe supplies structured source dimensions, including display rotation.
// Export applies the first rendered picture's crop to these original dimensions.
std::optional<std::pair<int, int>> ReadExportPictureSize(const fs::path &source, wxWindow *parent)
{
   const auto probe = wxFileName(wxStandardPaths::Get().GetExecutablePath())
      .GetPathWithSep() + "ffprobe.exe";
   if (!wxFileExists(probe)) throw std::runtime_error("The bundled ffprobe.exe is missing. Reinstall AuTopsy.");
   BundledMediaProcess process{{probe.ToStdWstring(), L"-v", L"error", L"-select_streams", L"v:0",
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
   return std::pair<int,int>{width, height};
}
} // namespace

std::optional<std::pair<int, int>> ReadVideoSourcePictureSize(const wxString &source, wxWindow *parent)
{ return ReadExportPictureSize(fs::absolute(fs::path(source.ToStdWstring())), parent); }

// Called when the project attachments start, and before every export.
void CleanAbandonedWorkspace()
{
   VideoCaptions::CleanCaptionWorkspace();
   const auto issue = CleanWorkspace();
   if (!issue.empty()) wxLogWarning("%s", issue);
}

bool RunBundledVideoTool(const std::vector<wxString> &arguments, wxWindow *parent,
   const wxString &title, const wxString &status, std::string *output, VideoTool tool, size_t maximumOutputBytes)
{
   const auto name = tool == VideoTool::Transcriber ? "autopsy-whisper.exe" :
      tool == VideoTool::Probe ? "ffprobe.exe" : "ffmpeg.exe";
   const auto executable = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPathWithSep() + name;
   if (!wxFileExists(executable))
      throw std::runtime_error(std::string{"The bundled "} + name + " is missing. Reinstall AuTopsy.");
   std::vector<std::wstring> native{executable.ToStdWstring()};
   for (const auto &argument : arguments) native.push_back(argument.ToStdWstring());
   BundledMediaProcess process{native, maximumOutputBytes};
   if (!process.Wait(parent, title, status, tool == VideoTool::Transcriber)) return false;
   if (output) *output = std::move(process.output);
   return true;
}

void WriteMediaTextFile(const wxString &destination, const std::string &text, bool overwrite)
{
   const auto target = fs::absolute(fs::path(destination.ToStdWstring()));
   if (fs::is_regular_file(target)) {
      if (fs::file_size(target) == text.size()) {
         std::ifstream input(target, std::ios::binary);
         const std::string current{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
         if (!input.bad() && current == text) return;
      }
   }
   if (target.filename().empty() || !fs::is_directory(target.parent_path()) ||
       (fs::exists(target) && (!overwrite || !fs::is_regular_file(target))))
      throw std::runtime_error("Choose a writable subtitle filename and confirm replacement of an existing file.");
   ExportWorkspace workspace{target};
   const auto pending = workspace.path/L"_captions.txt";
   std::ofstream output(pending, std::ios::binary|std::ios::trunc);
   output.write(text.data(), std::streamsize(text.size())); output.close();
   if (!output) throw std::runtime_error("Cannot write the subtitle file. The previous export is unchanged.");
   if (!MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH|
         (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
      throw WindowsError("Publishing the subtitle file");
}

void RecordBundledVideoInput(const std::vector<wxString> &arguments,
   const wxString &destination, wxWindow *parent)
{
   const auto executable = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPathWithSep() + "ffmpeg.exe";
   if (!wxFileExists(executable)) throw std::runtime_error("The bundled ffmpeg.exe is missing. Reinstall AuTopsy.");
   std::vector<std::wstring> native{executable.ToStdWstring()};
   for (const auto &argument : arguments) native.push_back(argument.ToStdWstring());
   try {
      BundledMediaProcess process{native, 16384, destination.ToStdWstring()};
      process.Wait(parent, _("Record Video"), _("Recording video. Stop and Add places it at the edit cursor."));
   }
   catch (const std::exception &error) {
      throw std::runtime_error(Utf8(destination) + "\n" + error.what() +
         "\nAny nonempty recording has been kept at this location. Existing files are never replaced.");
   }
}

bool ExportMix(AudacityProject &project, const VideoTimeline::Timeline &inputTimeline,
   double projectVideoDuration, const wxString &destination, bool overwrite, wxWindow *parent,
   const ExportSettings &settings, double projectAudioStart)
{
   RequireUsableExportSettings(settings);
   // Clip-only exports keep their selected picture and its fades, while mixing
   // the audible project audio over the original project-time window.
   const auto prepared = VideoEditing::PrepareProjectVideoExport(project, {&inputTimeline}, projectVideoDuration,
      {settings.width, settings.height, 0, settings.frameRate > 0 ? settings.frameRate : inputTimeline.frameRate, true}, parent);
   const auto &timeline = prepared ? prepared->timeline : inputTimeline;
   VideoTimeline::RequireWellFormedTimeline(timeline, VideoTimeline::MaximumVideoRenderSegments);
   const auto duration = projectVideoDuration;
   if (!std::isfinite(projectAudioStart) || projectAudioStart < 0 ||
       !std::isfinite(projectAudioStart + duration))
      throw std::runtime_error("Choose a usable project audio range for this export.");
   const auto copyPicture = VideoTimeline::KeepsOriginalPicture(timeline) && AllowsOriginalPictureCopy(settings)
      && std::abs(duration - timeline.End()) <= 1e-7;
   const auto outputRate = settings.frameRate > 0 ? settings.frameRate : timeline.frameRate;
   const auto spans = copyPicture ? std::vector<VideoExportFrameSpan>{}
      : PlanVideoExportFrames(timeline, duration, outputRate);
   const fs::path target = fs::absolute(fs::path(destination.ToStdWstring()));
   if (!std::isfinite(duration) || duration <= 0 || duration + 1e-7 < timeline.End() ||
       timeline.sourcePath.empty() || destination.empty())
      throw std::runtime_error("Choose video clips and an output filename before exporting.");
   const auto extension = wxString(target.extension().wstring()).Lower();
   if (extension != ".mp4" && extension != ".mkv")
      throw std::runtime_error("Choose an .mp4 or .mkv output filename.");
   if (!fs::is_directory(target.parent_path()))
      throw std::runtime_error("The output folder is unavailable.");
   std::vector<fs::path> inputs;
   std::map<uint64_t, size_t> clipInputs;
   const auto protectSource = [&](const fs::path &input) {
      if (wxString(input.wstring()).CmpNoCase(wxString(target.wstring())) == 0 ||
          (fs::exists(target) && fs::exists(input) && fs::equivalent(input, target)))
         throw std::runtime_error("The output must be different from every source video.");
   };
   for (const auto &path : timeline.protectedSourcePaths)
      protectSource(fs::absolute(fs::path(wxString::FromUTF8(path).ToStdWstring())));
   for (const auto &path : inputTimeline.protectedSourcePaths)
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
      clipInputs.emplace(clip.id, size_t(found - inputs.begin()));
      if (found == inputs.end()) inputs.push_back(input);
   }
   if (fs::exists(target) && !overwrite)
      throw std::runtime_error("The output already exists. Choose another name or allow overwrite.");
   const auto executable = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPathWithSep() + "ffmpeg.exe";
   if (!wxFileExists(executable)) throw std::runtime_error("The bundled ffmpeg.exe is missing. Reinstall AuTopsy.");
   const auto [plugin, format] = ExportPluginRegistry::Get().FindFormat("WAV");
   if (!plugin) throw std::runtime_error("The WAV export module is unavailable.");

   std::optional<std::pair<int,int>> pictureSize;
   std::optional<size_t> autoCanvasInput;
   VideoEditing::VideoFrameCrop autoCanvasCrop;
   if (!copyPicture) {
      if (settings.width) pictureSize = std::pair{settings.width, settings.height};
      else if (inputs.empty()) {
         if (timeline.sourceWidth <= 0 || timeline.sourceHeight <= 0)
            throw std::runtime_error("Choose an output resolution for this hidden video; its older project has no saved canvas size.");
         pictureSize = std::pair{(timeline.sourceWidth + 1) / 2 * 2, (timeline.sourceHeight + 1) / 2 * 2};
      }
      else {
         const auto first = std::find_if(spans.begin(), spans.end(),
            [](const auto &span) { return span.clipId != 0; });
         const auto input = first == spans.end() ? size_t{} : clipInputs.at(first->clipId);
         pictureSize = ReadExportPictureSize(inputs.at(input), parent);
         if (pictureSize) {
            const auto crop = first == spans.end() ? VideoEditing::VideoFrameCrop{} : first->crop;
            pictureSize = CalculateVideoExportCanvas(pictureSize->first, pictureSize->second, crop);
            if (first != spans.end()) { autoCanvasInput = input; autoCanvasCrop = crop; }
         }
      }
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
         .SetRange(projectAudioStart, projectAudioStart + duration, false)
         .SetNumChannels(2).SetSampleRate(48000)
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
      std::ostringstream graph;
      graph.imbue(std::locale::classic()); graph << std::setprecision(17);
      for (size_t input = 0; input < inputs.size(); ++input) {
         const auto count = std::count_if(spans.begin(), spans.end(),
            [&](const auto &span) { return span.clipId && clipInputs.at(span.clipId) == input; });
         if (!count) continue;
         graph << "[" << input << ":v:0]split=" << count;
         for (size_t i = 0; i < spans.size(); ++i)
            if (spans[i].clipId && clipInputs.at(spans[i].clipId) == input) graph << "[s" << i << "]";
         graph << ";\n";
      }
      for (size_t i = 0; i < spans.size(); ++i) {
         const auto &span = spans[i];
         const auto [width, height] = *pictureSize;
         if (!span.clipId)
            graph << "color=c=black:s=" << width << "x" << height << ":r=" << outputRate
               << ":d=" << static_cast<double>(span.frameCount) / outputRate;
         else {
            // Keep pre-cut frames until fps samples the requested source time.
            // Resetting each trim's first decoded frame would shift its clock.
            graph << "[s" << i << "]trim=end=" << span.sourceEnd
               << ",setpts=(PTS-STARTPTS-" << span.sourceStart << "/TB)/" << span.playbackRate;
            const auto crop = VideoEditing::VideoCropFilterExpression(span.crop);
            if (!crop.empty()) graph << "," << crop;
         }
         graph << ",fps=" << outputRate
            << ":start_time=0:eof_action=pass,tpad=stop_mode=clone:stop=-1,trim=end_frame=" << span.frameCount
            << ",setpts=N/(" << outputRate << "*TB)"
            << ",scale=" << width << ":" << height;
         // Mod-2 encoder rounding must not reintroduce thin bars around the
         // very picture that chose the automatic canvas. Other aspect ratios
         // and explicit dimensions retain their existing fit-with-padding policy.
         const auto fillsCanvas = span.clipId && autoCanvasInput &&
            clipInputs.at(span.clipId) == *autoCanvasInput && span.crop == autoCanvasCrop;
         if (!fillsCanvas)
            graph << ":force_original_aspect_ratio=decrease:force_divisible_by=2,pad="
               << width << ":" << height << ":(ow-iw)/2:(oh-ih)/2";
         graph << ",setsar=1,format=yuv420p[v" << i << "];\n";
      }
      for (size_t i = 0; i < spans.size(); ++i) graph << "[v" << i << "]";
      graph << "concat=n=" << spans.size() << ":v=1:a=0,settb=expr=1/(" << outputRate << "),setpts=N[vout]\n";
      const auto filterPath = workspace.path / "_filters.txt";
      std::ofstream filterFile(filterPath, std::ios::binary);
      const auto script = graph.str();
      filterFile.write(script.data(), script.size()); filterFile.close();
      if (!filterFile) throw std::runtime_error("Could not write the temporary video rendering instructions.");
      std::wostringstream frameRate; frameRate.imbue(std::locale::classic());
      frameRate << std::setprecision(17) << outputRate;
      arguments.insert(arguments.end(), {L"-filter_complex_script", filterPath.wstring(),
         L"-map", L"[vout]", L"-r", frameRate.str()});
      for (const auto &argument : VideoEncoderArguments(settings))
         arguments.push_back(wxString::FromUTF8(argument).ToStdWstring());
   }
   const auto audioMap = std::to_wstring(inputs.size()) + L":a:0";
   arguments.insert(arguments.end(), {L"-map", audioMap, L"-map_metadata", L"0",
      L"-map_chapters", copyPicture ? L"0" : L"-1", L"-c:a", L"aac", L"-b:a", L"256k",
      L"-af", L"apad", L"-t", seconds.str()});
   if (extension == ".mp4") { arguments.push_back(L"-movflags"); arguments.push_back(L"+faststart"); }
   arguments.push_back(result.wstring());
   {
      BundledMediaProcess encoder{ arguments };
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
