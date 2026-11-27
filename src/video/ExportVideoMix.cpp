#include "ExportVideoMix.h"
#include "RenderVideoComposition.h"
#include "PlanVideoExport.h"
#include "PrepareVideoEffects.h"
#include "PrepareCaptionModels.h"
#include "../clip-groups/GuardMediaDialogs.h"
#include "Export.h"
#include "ExportPluginHelpers.h"
#include "FileFormats.h"
#include "Mix.h"
#include "Prefs.h"
#include "MemoryX.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}
#include <sndfile.h>
#include <wx/file.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/progdlg.h>
#include <wx/generic/progdlgg.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#ifdef _WIN32
#include <wx/msw/wrapwin.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <optional>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <locale>
#include <limits>
#include <memory>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
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

#ifdef _WIN32
struct CloseNativeHandle {
   void operator()(void *handle) const noexcept
   { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using NativeHandle = std::unique_ptr<void, CloseNativeHandle>;
#endif
struct CloseRecordingFile {
#ifdef _WIN32
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
#else
   fs::path path;
   void operator()(wxFile *file) const noexcept
   {
      if (!file) return;
      if (file->IsOpened()) {
         struct stat opened{}, named{};
         // Retain every nonempty recording and refuse an unrelated replacement.
         if (::fstat(file->fd(), &opened) == 0 && opened.st_size == 0 &&
             ::lstat(path.c_str(), &named) == 0 && opened.st_dev == named.st_dev &&
             opened.st_ino == named.st_ino)
            ::unlink(path.c_str());
         else
            file->Flush();
      }
      delete file;
   }
#endif
};

#ifdef _WIN32
std::runtime_error WindowsError(const std::string &operation)
{
   return std::runtime_error(operation + " (Windows error " +
      std::to_string(GetLastError()) + ").");
}
#endif

// There is at most one pending workspace per settings profile. The owner file
// is exclusively locked while live. Cleanup removes only the named files,
// never an arbitrary tree or an output supplied by the user.
wxString CleanWorkspace()
{
   wxString stored;
   if (!gPrefs->Read(WorkspaceKey, &stored) || stored.empty()) return {};
   const auto path = fs::u8path(Utf8(stored));
   std::error_code error;
   const auto clear = [] {
      gPrefs->DeleteEntry(WorkspaceKey);
      gPrefs->Flush();
   };
   if (!fs::exists(path, error) && !error) { clear(); return {}; }
   const auto name = wxString(path.filename().native());
#ifdef _WIN32
   const auto attributes = GetFileAttributesW(path.c_str());
   const bool plain = attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
   const bool plain = fs::is_directory(fs::symlink_status(path, error));
#endif
   if (error || !path.is_absolute() || !name.StartsWith("_autopsy-video-export-") || !plain)
      return _("The pending video export workspace could not be safely opened: ") + stored;

   const auto ownerPath = path / "_owner";
#ifdef _WIN32
   NativeHandle owner{ CreateFileW(ownerPath.c_str(), GENERIC_READ, 0, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
   const bool opened = owner.get() != INVALID_HANDLE_VALUE;
   const bool missing = !opened && GetLastError() == ERROR_FILE_NOT_FOUND;
#else
   wxFile owner{::open(ownerPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
   const bool missing = !owner.IsOpened() && errno == ENOENT;
   struct stat status{};
   const bool opened = owner.IsOpened() && ::fstat(owner.fd(), &status) == 0 &&
      S_ISREG(status.st_mode) && ::flock(owner.fd(), LOCK_EX | LOCK_NB) == 0;
#endif
   if (!opened) {
      if (missing && fs::is_empty(path, error) && !error) {
         fs::remove(path, error);
         if (!error) { clear(); return {}; }
      }
      return _("Another export is using its workspace, or cleanup was denied: ") + stored;
   }
   char signature[64]{};
#ifdef _WIN32
   DWORD read{};
   const bool readOwner = ReadFile(owner.get(), signature, sizeof(signature) - 1, &read, nullptr);
#else
   const auto read = owner.Read(signature, sizeof(signature) - 1);
   const bool readOwner = read >= 0;
#endif
   if (!readOwner || std::string_view(signature, static_cast<size_t>(read)) != OwnerSignature)
      return _("The pending export workspace has an unknown owner: ") + stored;
   for (const auto *file : { "_mix.wav", "_filters.txt", "_result.mp4", "_result.mkv", "_captions.txt" }) {
      fs::remove(path / file, error);
      if (error) return _("Could not remove a temporary video export file: ") + stored;
   }
#ifdef _WIN32
   owner.reset();
#endif
   fs::remove(ownerPath, error);
   if (!error) fs::remove(path, error); // Refuse a nonempty directory.
   if (error) return _("Could not finish video export cleanup: ") + stored;
   clear();
   return {};
}

class ExportWorkspace final {
#ifdef _WIN32
   NativeHandle mOwner;
#else
   wxFile mOwner;
#endif
public:
   fs::path path;
   explicit ExportWorkspace(const fs::path &destination)
   {
      try {
      const auto issue = CleanWorkspace();
      if (!issue.empty()) throw std::runtime_error(Utf8(issue));
      std::random_device random;
      bool created{};
      for (unsigned attempt = 0; attempt < 32; ++attempt) {
         std::ostringstream name;
         name << "_autopsy-video-export-" << std::hex << random() << random();
         path = destination.parent_path() / name.str();
         if (fs::exists(path)) continue;
         // Record ownership before the first filesystem effect, so a new run
         // can remove an interrupted creation using current filesystem state.
         if (!gPrefs->Write(WorkspaceKey, wxString(path.native())) || !gPrefs->Flush())
            throw std::runtime_error("Could not record the video export workspace.");
#ifdef _WIN32
         created = CreateDirectoryW(path.c_str(), nullptr);
         if (created) break;
         if (GetLastError() != ERROR_ALREADY_EXISTS)
            throw WindowsError("Creating the video export workspace");
#else
         created = ::mkdir(path.c_str(), 0700) == 0;
         if (created) break;
         if (errno != EEXIST)
            throw std::system_error(errno, std::generic_category(), "Creating the video export workspace");
#endif
         if (!gPrefs->DeleteEntry(WorkspaceKey) || !gPrefs->Flush())
            throw std::runtime_error("Could not release a conflicting video export workspace entry.");
      }
      if (!created) throw std::runtime_error("Could not reserve a video export workspace.");
#ifdef _WIN32
      mOwner.reset(CreateFileW((path / "_owner").c_str(), GENERIC_READ | GENERIC_WRITE,
         0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
      if (mOwner.get() == INVALID_HANDLE_VALUE) throw WindowsError("Locking the video export workspace");
      DWORD written{};
      if (!WriteFile(mOwner.get(), OwnerSignature, DWORD(strlen(OwnerSignature)), &written, nullptr) ||
         written != strlen(OwnerSignature) || !FlushFileBuffers(mOwner.get()))
         throw WindowsError("Marking the video export workspace");
#else
      const int owner = ::open((path / "_owner").c_str(),
         O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (owner < 0)
         throw std::system_error(errno, std::generic_category(), "Opening the video export workspace");
      mOwner.Attach(owner);
      if (::flock(mOwner.fd(), LOCK_EX | LOCK_NB) != 0)
         throw std::system_error(errno, std::generic_category(), "Locking the video export workspace");
      if (mOwner.Write(OwnerSignature, strlen(OwnerSignature)) != strlen(OwnerSignature) || !mOwner.Flush())
         throw std::runtime_error("Could not mark the video export workspace.");
#endif
      }
      catch (...) {
#ifdef _WIN32
         mOwner.reset();
#else
         mOwner.Close();
#endif
         CleanWorkspace();
         throw;
      }
   }
   ~ExportWorkspace()
   {
#ifdef _WIN32
      mOwner.reset();
#else
      mOwner.Close();
#endif
      CleanWorkspace();
   }
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

// The native mixer owns all track/master effects, stretching, mute, solo and
// gain. This adapter owns only the fixed-length, interleaved stereo intermediate.
// An exhausted mixer produces explicit zeros: an empty floating WAV from the
// general audio exporter is not a usable input for video-only or late ranges.
bool WriteProjectAudioRange(AudacityProject &project, const fs::path &path,
   double start, double duration, wxWindow *parent)
{
   constexpr size_t BlockFrames = 8192;
   constexpr double Rate = 48000;
   const auto frameValue = std::round(duration * Rate);
   if (!std::isfinite(frameValue) || frameValue < 1 || frameValue >= double(std::numeric_limits<sf_count_t>::max()))
      throw std::runtime_error("The export audio range cannot fit a sample-frame count.");
   const auto frames = static_cast<sf_count_t>(frameValue);
   auto mixer = ExportPluginHelpers::CreateMixer(project, false, start, start + duration,
      2, BlockFrames, true, Rate, floatSample, nullptr);
   SF_INFO info{}; info.channels = 2; info.samplerate = int(Rate);
   // RF64 keeps the same WAV/float decoder contract without a 4 GB size limit.
   info.format = SF_FORMAT_RF64 | SF_FORMAT_FLOAT;
   wxFile output{wxString(path.native()), wxFile::write};
   if (!output.IsOpened()) throw std::runtime_error("Could not create the video export's audio intermediate.");
   SFFile file;
   // Match the native audio export's Unicode-safe descriptor boundary. The
   // wxFile keeps ownership until libsndfile has finished its container header.
   file.reset(SFCall<SNDFILE*>(sf_open_fd, output.fd(), SFM_WRITE, &info, SF_FALSE));
   if (!file) throw std::runtime_error("Could not create the video export's audio intermediate.");
   std::vector<float> block(BlockFrames * 2);
   MixProgress progress{parent};
   for (sf_count_t written{}; written < frames;) {
      const auto count = size_t(std::min<sf_count_t>(BlockFrames, frames - written));
      std::fill(block.begin(), block.end(), 0.f);
      const auto mixed = mixer->Process(count);
      if (mixed > count) throw std::runtime_error("The audio mixer exceeded its requested frame count.");
      if (mixed) std::memcpy(block.data(), mixer->GetBuffer(), mixed * 2 * sizeof(float));
      if (!std::all_of(block.begin(), block.begin() + count * 2, [](float value) { return std::isfinite(value); }))
         throw std::runtime_error("The project audio mix contains a non-finite sample.");
      if (SFCall<sf_count_t>(sf_writef_float, file.get(), block.data(), sf_count_t(count)) != sf_count_t(count))
         throw std::runtime_error("Could not write the video export's audio intermediate (disk full or write error).");
      written += count;
      progress.OnProgress(double(written) / frames);
      if (progress.IsCancelled()) return false;
   }
   if (file.close() != 0) throw std::runtime_error("Could not finish the video export's audio intermediate.");
   if (!output.Close()) throw std::runtime_error("Could not close the video export's audio intermediate.");
   return true;
}

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

// Child lifetime is tied to the editor before the tool can run. Cancellation
// and exceptions terminate and reap the child before releasing its output file.
class BundledMediaProcess final {
#ifdef _WIN32
   NativeHandle mJob, mProcess, mOutputRead, mErrorRead;
   NativeHandle mInputWrite;
   std::unique_ptr<void, CloseRecordingFile> mRecordingFile;
#else
   pid_t mChild{-1};
   wxFile mOutputRead, mErrorRead;
   std::unique_ptr<wxFile, CloseRecordingFile> mRecordingFile;
#endif
   bool mRecording;
   size_t mOutputLimit;
   bool mOutputOverflow{};
public:
   std::wstring invocation;
   std::string output, errors;
   explicit BundledMediaProcess(const std::vector<wxString> &arguments, size_t outputLimit = 16384,
      const wxString &recordingPath = {})
      : mRecording(!recordingPath.empty()), mOutputLimit(outputLimit)
   {
      if (!outputLimit || outputLimit > 8 * 1024 * 1024)
         throw std::runtime_error("The media tool output limit is outside supported bounds.");
      for (const auto &argument : arguments) {
         if (!invocation.empty()) invocation += L' ';
         invocation += QuoteArgument(argument.ToStdWstring());
      }
#ifdef _WIN32
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
      if (!CreateProcessW(arguments.front().wc_str(), commandLine.data(), nullptr, nullptr,
         TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
         nullptr, nullptr, &startup.StartupInfo, &process)) throw WindowsError("Starting bundled FFmpeg");
      mProcess.reset(process.hProcess);
      NativeHandle thread{ process.hThread };
      if (!AssignProcessToJobObject(mJob.get(), mProcess.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
         TerminateProcess(mProcess.get(), 1);
         WaitForSingleObject(mProcess.get(), INFINITE);
         throw WindowsError("Assigning the FFmpeg process job");
      }
#else
      auto command = std::vector<std::string>{};
      command.reserve(arguments.size());
      for (const auto &argument : arguments) command.push_back(Utf8(argument));
      std::vector<char *> argv;
      argv.reserve(command.size() + 1);
      for (auto &argument : command) argv.push_back(argument.data());
      argv.push_back(nullptr);

      int pipe[2];
      if (::pipe2(pipe, O_CLOEXEC) != 0)
         throw std::system_error(errno, std::generic_category(), "Creating the media tool error pipe");
      mErrorRead.Attach(pipe[0]);
      wxFile errorWrite{pipe[1]}, outputWrite;
      if (::fcntl(mErrorRead.fd(), F_SETFL, O_NONBLOCK) != 0)
         throw std::system_error(errno, std::generic_category(), "Setting up the media tool error pipe");
      if (!mRecording) {
         if (::pipe2(pipe, O_CLOEXEC) != 0)
            throw std::system_error(errno, std::generic_category(), "Creating the media tool output pipe");
         mOutputRead.Attach(pipe[0]);
         outputWrite.Attach(pipe[1]);
         if (::fcntl(mOutputRead.fd(), F_SETFL, O_NONBLOCK) != 0)
            throw std::system_error(errno, std::generic_category(), "Setting up the media tool output pipe");
      }
      const auto inputFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
      if (inputFd < 0)
         throw std::system_error(errno, std::generic_category(), "Opening the media tool input");
      wxFile input{inputFd};
      if (mRecording) {
         auto &path = mRecordingFile.get_deleter().path;
         path = fs::u8path(Utf8(recordingPath));
         mRecordingFile.reset(new wxFile);
         const auto file = ::open(path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
         if (file < 0)
            throw std::system_error(errno, std::generic_category(), "Creating a new recording file");
         mRecordingFile->Attach(file);
      }
      const int descriptors[] = {input.fd(), mRecording ? mRecordingFile->fd() : outputWrite.fd(), errorWrite.fd()};
      const auto parent = ::getpid();
      mChild = ::fork();
      if (mChild < 0) {
         const auto error = errno;
         throw std::system_error(error, std::generic_category(), "Starting the media tool");
      }
      if (mChild == 0) {
         // Only system calls and precomputed arguments are used between fork and exec.
         if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent) ::_exit(126);
         int streams[3];
         for (int destination = 0; destination < 3; ++destination) {
            streams[destination] = ::fcntl(descriptors[destination], F_DUPFD_CLOEXEC, 3);
            if (streams[destination] < 0) ::_exit(126);
         }
         for (int destination = 0; destination < 3; ++destination)
            if (::dup2(streams[destination], destination) < 0) ::_exit(126);
         if (::close_range(3, ~0U, 0) != 0) ::_exit(126);
         ::execve(argv[0], argv.data(), ::environ);
         constexpr char message[] = "Could not execute the bundled media tool.\n";
         ::write(STDERR_FILENO, message, sizeof(message) - 1);
         ::_exit(127);
      }
#endif
   }
   ~BundledMediaProcess()
   {
#ifdef _WIN32
      mJob.reset();
      if (mProcess) WaitForSingleObject(mProcess.get(), INFINITE);
#else
      if (mChild > 0) {
         ::kill(mChild, SIGKILL);
         int status{};
         while (::waitpid(mChild, &status, 0) == -1 && errno == EINTR) {}
      }
#endif
   }
#ifdef _WIN32
   static bool Drain(HANDLE pipe, std::string &text, size_t limit = 16384)
#else
   static bool Drain(int pipe, std::string &text, size_t limit = 16384)
#endif
   {
      size_t budget = 256 * 1024;
      bool truncated{};
#ifdef _WIN32
      DWORD available{};
      while (budget && PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
         char buffer[4096]; DWORD received{};
         if (!ReadFile(pipe, buffer, DWORD(std::min<size_t>({ sizeof(buffer), available, budget })), &received, nullptr) || !received) break;
#else
      while (budget) {
         char buffer[4096];
         const auto received = ::read(pipe, buffer, std::min(sizeof(buffer), budget));
         if (received < 0 && errno == EINTR) continue;
         if (received <= 0) break;
#endif
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
      if (mRecording) dialog = std::make_unique<wxGenericProgressDialog>(title, status, 100, parent, style);
      else dialog = std::make_unique<wxProgressDialog>(title, status, 100, parent, style);
      auto &progress = *dialog;
      if (mRecording) if (auto *stop = progress.FindWindow(wxID_CANCEL)) stop->SetLabel(_("Stop and Add"));
      using Clock = std::chrono::steady_clock;
      auto lastFlush = Clock::now();
      std::optional<Clock::time_point> stopped;
#ifndef _WIN32
      int childStatus{};
#endif
      while (true) {
#ifdef _WIN32
         if (mOutputRead) mOutputOverflow |= Drain(mOutputRead.get(), output, mOutputLimit);
         Drain(mErrorRead.get(), errors);
         const auto state = WaitForSingleObject(mProcess.get(), 20);
         if (state == WAIT_OBJECT_0) break;
         if (state == WAIT_FAILED) throw WindowsError("Waiting for FFmpeg");
#else
         if (mOutputRead.IsOpened()) mOutputOverflow |= Drain(mOutputRead.fd(), output, mOutputLimit);
         Drain(mErrorRead.fd(), errors);
         const auto state = ::waitpid(mChild, &childStatus, WNOHANG);
         if (state == mChild) { mChild = -1; break; }
         if (state < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) mChild = -1;
            throw std::system_error(errno, std::generic_category(), "Waiting for the media tool");
         }
         wxMilliSleep(20);
#endif
         if (!progress.Pulse()) {
            if (!mRecording) return false;
            if (!stopped) {
#ifdef _WIN32
               DWORD sent{};
               if (!WriteFile(mInputWrite.get(), "q\n", 2, &sent, nullptr) || sent != 2)
                  throw WindowsError("Stopping video recording");
#else
               // FFmpeg handles its first interrupt by writing the trailer.
               if (::kill(mChild, SIGINT) != 0 && errno != ESRCH)
                  throw std::system_error(errno, std::generic_category(), "Stopping video recording");
#endif
               stopped = Clock::now();
            }
            progress.Resume();
         }
         if (mRecording && Clock::now()-lastFlush >= std::chrono::seconds(1)) {
#ifdef _WIN32
            if (!FlushFileBuffers(mRecordingFile.get())) throw WindowsError("Flushing recorded video");
#else
            if (!mRecordingFile->Flush()) throw std::runtime_error("Could not flush recorded video.");
#endif
            lastFlush = Clock::now();
         }
         if (stopped && Clock::now()-*stopped > std::chrono::seconds(10))
            throw std::runtime_error("The recording device did not stop within ten seconds.\nCommand: " +
               Utf8(wxString(invocation)) + "\nstdout: <streamed to recording>\nstderr:\n" + DescribeProcessOutput(errors));
      }
#ifdef _WIN32
      if (mOutputRead) mOutputOverflow |= Drain(mOutputRead.get(), output, mOutputLimit);
      Drain(mErrorRead.get(), errors);
      if (mRecordingFile && !FlushFileBuffers(mRecordingFile.get())) throw WindowsError("Flushing completed recording");
      DWORD exit{};
      if (!GetExitCodeProcess(mProcess.get(), &exit)) throw WindowsError("Reading the FFmpeg exit status");
#else
      if (mOutputRead.IsOpened()) mOutputOverflow |= Drain(mOutputRead.fd(), output, mOutputLimit);
      Drain(mErrorRead.fd(), errors);
      if (mRecording && !mRecordingFile->Flush()) throw std::runtime_error("Could not flush completed recording.");
      const auto exitStatus = WIFEXITED(childStatus) ? WEXITSTATUS(childStatus) : 128 + WTERMSIG(childStatus);
      const auto exit = stopped && exitStatus == 255 && mRecordingFile->Length() > 0 ? 0 : exitStatus;
#endif
      if (exit != 0)
         throw std::runtime_error("Media tool operation failed (exit " + std::to_string(exit) + ").\nCommand: " +
            Utf8(wxString(invocation)) + "\nstdout:\n" + (mRecording ? "  <streamed to recording>" : DescribeProcessOutput(output)) +
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
struct VideoProbeProgress
{
   wxProgressDialog &dialog;
   std::chrono::steady_clock::time_point lastUpdate{};
   bool cancelled{};
};

int CheckVideoProbeCancellation(void *opaque)
{
   auto &progress = *static_cast<VideoProbeProgress *>(opaque);
   const auto now = std::chrono::steady_clock::now();
   if (!progress.cancelled && now - progress.lastUpdate >= std::chrono::milliseconds(20)) {
      progress.lastUpdate = now;
      progress.cancelled = !progress.dialog.Pulse();
   }
   return progress.cancelled;
}

std::pair<int, int> ReadVideoStreamDisplaySize(const AVStream &stream)
{
   const auto &parameters = *stream.codecpar;
   int width = parameters.width, height = parameters.height;
   double rotation{};
   const auto *matrix = av_packet_side_data_get(
      parameters.coded_side_data, parameters.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
   if (matrix && matrix->size >= 9 * sizeof(int32_t))
      rotation = av_display_rotation_get(reinterpret_cast<const int32_t *>(matrix->data));
   if (!std::isfinite(rotation) || rotation == 0) {
      const auto *tag = av_dict_get(stream.metadata, "rotate", nullptr, 0);
      rotation = tag ? std::strtod(tag->value, nullptr) : 0.0;
   }
   if (std::isfinite(rotation) && std::lround(std::abs(std::remainder(rotation, 180.0))) == 90)
      std::swap(width, height);
   if (width <= 0 || height <= 0 || width > 65536 || height > 65536)
      throw std::runtime_error("The original video dimensions are outside supported bounds.");
   return {width, height};
}

template<auto Release>
struct ReleaseFFmpegHandle
{
   template<typename T>
   void operator()(T *value) const noexcept
   {
      if (value) Release(&value);
   }
};

template<typename T, auto Release>
using VideoExportHandle = std::unique_ptr<T, ReleaseFFmpegHandle<Release>>;

struct VideoExportDecoder
{
   VideoExportHandle<AVFormatContext, avformat_close_input> format;
   VideoExportHandle<AVCodecContext, avcodec_free_context> codec;
   VideoExportHandle<AVFrame, av_frame_free> frame;
   const AVStream *stream{};
   AVFilterContext *filterInput{};
   int64_t nextPresentationTime{AV_NOPTS_VALUE};
   bool draining{};
};

VideoExportDecoder OpenVideoExportDecoder(const fs::path &path, AVMediaType mediaType,
   bool decode, VideoProbeProgress &progress)
{
   auto *input = avformat_alloc_context();
   if (!input) throw std::runtime_error("FFmpeg could not allocate an export input.");
   input->interrupt_callback = {CheckVideoProbeCancellation, &progress};
   const auto filename = path.u8string();
   const auto openResult = avformat_open_input(
      &input, reinterpret_cast<const char *>(filename.c_str()), nullptr, nullptr);
   VideoExportDecoder result;
   result.format.reset(input);
   if (progress.cancelled) return {};
   if (openResult < 0)
      throw std::runtime_error("FFmpeg could not open an export input (" + std::to_string(openResult) + ").");
   const auto streamResult = avformat_find_stream_info(result.format.get(), nullptr);
   if (progress.cancelled) return {};
   if (streamResult < 0)
      throw std::runtime_error("FFmpeg could not read export input streams (" + std::to_string(streamResult) + ").");
   for (unsigned index = 0; index < result.format->nb_streams; ++index) {
      const auto *stream = result.format->streams[index];
      if (stream->codecpar->codec_type == mediaType) {
         result.stream = stream;
         break;
      }
   }
   if (!result.stream) throw std::runtime_error("The export input has no stream of the requested type.");
   if (result.stream->time_base.num <= 0 || result.stream->time_base.den <= 0)
      throw std::runtime_error("The export input has no usable stream clock.");
   if (!decode) return result;

   const auto *decoder = avcodec_find_decoder(result.stream->codecpar->codec_id);
   if (!decoder) throw std::runtime_error("FFmpeg has no decoder for the export input.");
   result.codec.reset(avcodec_alloc_context3(decoder));
   result.frame.reset(av_frame_alloc());
   if (!result.codec || !result.frame)
      throw std::runtime_error("FFmpeg could not allocate an export decoder or frame.");
   const auto parameters = avcodec_parameters_to_context(result.codec.get(), result.stream->codecpar);
   if (parameters < 0)
      throw std::runtime_error("FFmpeg could not read the export codec parameters (" + std::to_string(parameters) + ").");
   result.codec->pkt_timebase = result.stream->time_base;
   const auto codecResult = avcodec_open2(result.codec.get(), decoder, nullptr);
   if (codecResult < 0)
      throw std::runtime_error("FFmpeg could not open the export decoder (" + std::to_string(codecResult) + ").");
   return result;
}

bool ReadVideoExportPacket(VideoExportDecoder &input, AVPacket &packet,
   const VideoProbeProgress &progress)
{
   av_packet_unref(&packet);
   int result;
   while ((result = av_read_frame(input.format.get(), &packet)) >= 0) {
      if (packet.stream_index == input.stream->index) return true;
      av_packet_unref(&packet);
   }
   if (result == AVERROR_EOF || progress.cancelled) return false;
   throw std::runtime_error("FFmpeg could not read an export packet (" + std::to_string(result) + ").");
}

bool ReadVideoExportFrame(VideoExportDecoder &input, AVPacket &packet,
   const VideoProbeProgress &progress)
{
   av_frame_unref(input.frame.get());
   while (!progress.cancelled) {
      const auto decoded = avcodec_receive_frame(input.codec.get(), input.frame.get());
      if (decoded >= 0) {
         auto &frame = *input.frame;
         if (frame.best_effort_timestamp != AV_NOPTS_VALUE)
            frame.pts = frame.best_effort_timestamp;
         if (frame.pts == AV_NOPTS_VALUE) {
            auto start = input.stream->start_time;
            if (start == AV_NOPTS_VALUE) start = 0;
            frame.pts = input.nextPresentationTime == AV_NOPTS_VALUE ? start : input.nextPresentationTime;
         }
         auto step = frame.duration;
         if (step <= 0 && frame.nb_samples > 0 && frame.sample_rate > 0)
            step = av_rescale_q(frame.nb_samples, AVRational{1, frame.sample_rate}, input.stream->time_base);
         if (step <= 0 && input.stream->avg_frame_rate.num > 0)
            step = av_rescale_q(1, av_inv_q(input.stream->avg_frame_rate), input.stream->time_base);
         step = std::max<int64_t>(step, 1);
         if (frame.pts <= std::numeric_limits<int64_t>::max() - step)
            input.nextPresentationTime = frame.pts + step;
         return true;
      }
      if (decoded == AVERROR_EOF) return false;
      if (decoded != AVERROR(EAGAIN) || input.draining)
         throw std::runtime_error("FFmpeg could not decode an export frame (" + std::to_string(decoded) + ").");

      const auto hasPacket = ReadVideoExportPacket(input, packet, progress);
      if (progress.cancelled) return false;
      const auto sent = avcodec_send_packet(input.codec.get(), hasPacket ? &packet : nullptr);
      av_packet_unref(&packet);
      if (sent < 0)
         throw std::runtime_error("FFmpeg could not submit an export packet (" + std::to_string(sent) + ").");
      input.draining = !hasPacket;
   }
   return false;
}

void DrainVideoExportEncoder(AVFormatContext &output, const AVStream &stream,
   AVCodecContext &encoder, AVFrame *frame, AVRational frameClock, AVPacket &packet)
{
   if (frame && frame->pts != AV_NOPTS_VALUE)
      frame->pts = av_rescale_q(frame->pts, frameClock, encoder.time_base);
   const auto sent = avcodec_send_frame(&encoder, frame);
   if (sent < 0)
      throw std::runtime_error("FFmpeg could not submit a frame for encoding (" + std::to_string(sent) + ").");

   for (;;) {
      const auto received = avcodec_receive_packet(&encoder, &packet);
      if (received == AVERROR_EOF || received == AVERROR(EAGAIN)) break;
      if (received < 0)
         throw std::runtime_error("FFmpeg could not encode an output packet (" + std::to_string(received) + ").");
      av_packet_rescale_ts(&packet, encoder.time_base, stream.time_base);
      packet.stream_index = stream.index;
      const auto written = av_interleaved_write_frame(&output, &packet);
      if (written < 0)
         throw std::runtime_error("FFmpeg could not write an output packet (" + std::to_string(written) + ").");
   }
}


std::pair<AVFilterContext *, AVFilterContext *> CreateVideoExportFilters(
   AVFilterGraph &graph, std::vector<VideoExportDecoder> &pictures, VideoExportDecoder &audio,
   const std::string &picturePlan, double duration)
{
   std::ostringstream specification;
   specification.imbue(std::locale::classic());
   specification << std::setprecision(17);
   std::map<std::string, AVFilterContext *> sources;
   for (size_t index = 0; index <= pictures.size(); ++index) {
      const bool isAudio = index == pictures.size();
      auto &input = isAudio ? audio : pictures[index];
      if (!input.codec) continue; // A copied stream goes directly to the container.
      const auto name = isAudio ? "audio_input" : "picture_input_" + std::to_string(index);
      const auto *buffer = avfilter_get_by_name(isAudio ? "abuffer" : "buffer");
      input.filterInput = buffer ? avfilter_graph_alloc_filter(&graph, buffer, name.c_str()) : nullptr;
      if (!input.filterInput)
         throw std::runtime_error("FFmpeg could not allocate an export filter input.");
      sources.emplace(name, input.filterInput);

      const auto &frame = *input.frame;
      AVBufferSrcParameters parameters{};
      parameters.format = frame.format;
      parameters.time_base = input.stream->time_base;
      if (isAudio) {
         parameters.sample_rate = frame.sample_rate;
         parameters.ch_layout = frame.ch_layout;
      }
      else {
         parameters.width = frame.width;
         parameters.height = frame.height;
         parameters.sample_aspect_ratio = frame.sample_aspect_ratio.num ? frame.sample_aspect_ratio : AVRational{1, 1};
         parameters.frame_rate = input.stream->avg_frame_rate;
         parameters.color_space = frame.colorspace;
         parameters.color_range = frame.color_range;
      }
      const auto configured = av_buffersrc_parameters_set(input.filterInput, &parameters);
      const auto started = configured < 0 ? configured : avfilter_init_str(input.filterInput, nullptr);
      if (started < 0)
         throw std::runtime_error("FFmpeg could not initialize an export filter input (" + std::to_string(started) + ").");

      if (!isAudio) {
         const auto &codec = *input.stream->codecpar;
         auto *side = av_packet_side_data_get(codec.coded_side_data, codec.nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
         const auto *matrix = side && side->size >= 9 * sizeof(int32_t) ? reinterpret_cast<const int32_t *>(side->data) : nullptr;
         const auto *tag = av_dict_get(input.stream->metadata, "rotate", nullptr, 0);
         const auto reported = matrix ? av_display_rotation_get(matrix) : tag ? std::strtod(tag->value, nullptr) : 0.0;
         const auto angle = std::isfinite(reported) ? std::fmod(360.0 - std::fmod(reported, 360.0), 360.0) : 0.0;

         specification << "[" << name << "]";
         if (std::abs(angle - 90) < 1)
            specification << (matrix && matrix[3] > 0 ? "transpose=cclock_flip" : "transpose=clock");
         else if (std::abs(angle - 270) < 1)
            specification << (matrix && matrix[3] < 0 ? "transpose=clock_flip" : "transpose=cclock");
         else if (std::abs(angle - 180) < 1) {
            const bool flipX = !matrix || matrix[0] < 0;
            const bool flipY = !matrix || matrix[4] < 0;
            if (flipX && flipY) specification << "hflip,vflip";
            else if (flipX) specification << "hflip";
            else if (flipY) specification << "vflip";
            else specification << "null";
         }
         else if (angle > 1 && angle < 359)
            specification << "rotate=" << angle << "*PI/180";
         else
            specification << (matrix && matrix[4] < 0 ? "vflip" : "null");
         specification << "[picture" << index << "];";
      }
   }

   if (!picturePlan.empty()) specification << picturePlan << ";";
   specification << "[audio_input]asetpts=PTS-STARTPTS,apad,atrim=duration=" << duration
      << ",aformat=sample_fmts=fltp[audio_out]";
   AVFilterInOut *inputLinks{}, *outputLinks{};
   const auto freeInputs = finally(std::bind(avfilter_inout_free, &inputLinks));
   const auto freeOutputs = finally(std::bind(avfilter_inout_free, &outputLinks));
   const auto parsed = avfilter_graph_parse2(&graph, specification.str().c_str(), &inputLinks, &outputLinks);
   if (parsed < 0)
      throw std::runtime_error("FFmpeg could not parse the export filters (" + std::to_string(parsed) + ").");

   for (auto *link = inputLinks; link; link = link->next) {
      const auto source = sources.find(link->name ? link->name : "");
      if (source == sources.end() || avfilter_link(source->second, 0, link->filter_ctx, link->pad_idx) < 0)
         throw std::runtime_error("FFmpeg could not connect an export filter input.");
      sources.erase(source);
   }
   if (!sources.empty()) throw std::runtime_error("An export input was unused by the filter graph.");

   AVFilterContext *pictureOutput{}, *audioOutput{};
   for (auto *link = outputLinks; link; link = link->next) {
      const std::string name = link->name ? link->name : "";
      const bool isAudio = name == "audio_out";
      if (!isAudio && name != "vout")
         throw std::runtime_error("The export filter graph contains an unexpected output.");
      auto *&output = isAudio ? audioOutput : pictureOutput;
      const auto *sink = avfilter_get_by_name(isAudio ? "abuffersink" : "buffersink");
      if (output || !sink || avfilter_graph_create_filter(&output, sink, name.c_str(), nullptr, nullptr, &graph) < 0 ||
          avfilter_link(link->filter_ctx, link->pad_idx, output, 0) < 0)
         throw std::runtime_error("FFmpeg could not connect an export filter output.");
   }
   if (!audioOutput || (!picturePlan.empty() && !pictureOutput))
      throw std::runtime_error("The export filter graph is missing an output.");
   const auto configured = avfilter_graph_config(&graph, nullptr);
   if (configured < 0)
      throw std::runtime_error("FFmpeg could not configure the export filters (" + std::to_string(configured) + ").");
   return {pictureOutput, audioOutput};
}


bool EncodeVideoExport(const std::vector<fs::path> &inputs, const std::vector<bool> &renderedInputs,
   const fs::path &mix, const fs::path &result, const std::string &picturePlan,
   const ExportSettings &settings, double frameRate, double duration, wxWindow *parent)
{
   wxProgressDialog dialog{_("Export Video with Audio Mix"), _("Writing video with the edited audio..."),
      100, parent, wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME};
   VideoProbeProgress progress{dialog};
   try {
      const bool copyPicture = picturePlan.empty();
      if (copyPicture && inputs.empty()) throw std::runtime_error("There is no source stream to copy.");
      const auto pictureRate = av_d2q(frameRate, 1000000);
      VideoExportHandle<AVPacket, av_packet_free> inputPacket{av_packet_alloc()};
      VideoExportHandle<AVPacket, av_packet_free> outputPacket{av_packet_alloc()};
      VideoExportHandle<AVPacket, av_packet_free> copiedPacket{av_packet_alloc()};
      VideoExportHandle<AVFrame, av_frame_free> nextAudio{av_frame_alloc()};
      VideoExportHandle<AVFrame, av_frame_free> nextPicture{av_frame_alloc()};
      VideoExportHandle<AVFilterGraph, avfilter_graph_free> filters{avfilter_graph_alloc()};
      if (!inputPacket || !outputPacket || !copiedPacket || !nextAudio || !nextPicture || !filters)
         throw std::runtime_error("FFmpeg could not allocate export resources.");

      std::vector<VideoExportDecoder> pictures;
      pictures.reserve(inputs.size());
      for (size_t index = 0; index < inputs.size(); ++index) {
         pictures.push_back(OpenVideoExportDecoder(inputs[index], AVMEDIA_TYPE_VIDEO,
            !copyPicture && renderedInputs[index], progress));
         if (progress.cancelled) return false;
         auto &input = pictures.back();
         if (input.codec && !ReadVideoExportFrame(input, *inputPacket, progress)) {
            if (progress.cancelled) return false;
            throw std::runtime_error("The source has no decodable video frames.");
         }
      }
      auto audio = OpenVideoExportDecoder(mix, AVMEDIA_TYPE_AUDIO, true, progress);
      if (progress.cancelled) return false;
      if (!ReadVideoExportFrame(audio, *inputPacket, progress)) {
         if (progress.cancelled) return false;
         throw std::runtime_error("The project mix has no decodable audio.");
      }

      const auto [pictureSink, audioSink] = CreateVideoExportFilters(*filters, pictures, audio, picturePlan, duration);
      const auto filename = result.u8string();
      AVFormatContext *rawOutput{};
      const auto allocated = avformat_alloc_output_context2(&rawOutput, nullptr, nullptr,
         reinterpret_cast<const char *>(filename.c_str()));
      std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> output{rawOutput, avformat_free_context};
      if (allocated < 0 || !output)
         throw std::runtime_error("FFmpeg could not create the selected output format.");
      const auto closeOutput = finally(std::bind(avio_closep, &output->pb));
      auto *videoStream = avformat_new_stream(output.get(), nullptr);
      auto *audioStream = avformat_new_stream(output.get(), nullptr);
      if (!videoStream || !audioStream)
         throw std::runtime_error("FFmpeg could not allocate output streams.");

      const auto &metadata = pictures.empty() ? *audio.format : *pictures.front().format;
      if (av_dict_copy(&output->metadata, metadata.metadata, 0) < 0)
         throw std::runtime_error("FFmpeg could not copy the source metadata.");
      const auto globalHeader = output->oformat->flags & AVFMT_GLOBALHEADER ? AV_CODEC_FLAG_GLOBAL_HEADER : 0;
      VideoExportHandle<AVCodecContext, avcodec_free_context> pictureEncoder;
      if (copyPicture) {
         const auto &source = *pictures.front().stream;
         if (avcodec_parameters_copy(videoStream->codecpar, source.codecpar) < 0 ||
             av_dict_copy(&videoStream->metadata, source.metadata, 0) < 0)
            throw std::runtime_error("FFmpeg could not copy the original video stream.");
         videoStream->codecpar->codec_tag = 0;
         videoStream->time_base = source.time_base;
         videoStream->avg_frame_rate = source.avg_frame_rate;
         videoStream->disposition = source.disposition;
         if (metadata.nb_chapters) {
            output->chapters = static_cast<AVChapter **>(av_calloc(metadata.nb_chapters, sizeof(AVChapter *)));
            if (!output->chapters) throw std::runtime_error("FFmpeg could not allocate chapter metadata.");
            for (unsigned index = 0; index < metadata.nb_chapters; ++index) {
               auto *chapter = static_cast<AVChapter *>(av_mallocz(sizeof(AVChapter)));
               if (!chapter) throw std::runtime_error("FFmpeg could not allocate a chapter.");
               output->chapters[output->nb_chapters++] = chapter;
               *chapter = *metadata.chapters[index];
               chapter->metadata = nullptr;
               if (av_dict_copy(&chapter->metadata, metadata.chapters[index]->metadata, 0) < 0)
                  throw std::runtime_error("FFmpeg could not copy a chapter.");
            }
         }
      }
      else {
         const auto encoderArguments = VideoEncoderArguments(settings);
         const auto &encoderName = encoderArguments.at(1);
         const auto *encoder = avcodec_find_encoder_by_name(encoderName.c_str());
         if (!encoder) throw std::runtime_error("FFmpeg encoder " + encoderName + " is unavailable.");
         pictureEncoder.reset(avcodec_alloc_context3(encoder));
         if (!pictureEncoder) throw std::runtime_error("FFmpeg could not allocate the video encoder.");
         pictureEncoder->width = av_buffersink_get_w(pictureSink);
         pictureEncoder->height = av_buffersink_get_h(pictureSink);
         pictureEncoder->pix_fmt = static_cast<AVPixelFormat>(av_buffersink_get_format(pictureSink));
         pictureEncoder->time_base = av_inv_q(pictureRate);
         pictureEncoder->framerate = pictureRate;
         pictureEncoder->sample_aspect_ratio = {1, 1};
         pictureEncoder->flags |= globalHeader;
         AVDictionary *encoderSettings{};
         const auto freeEncoderSettings = finally(std::bind(av_dict_free, &encoderSettings));
         for (size_t index = 2; index + 1 < encoderArguments.size(); index += 2) {
            const auto &key = encoderArguments[index], &value = encoderArguments[index + 1];
            if ((key == "-crf" || key == "-preset") &&
                av_dict_set(&encoderSettings, key.c_str() + 1, value.c_str(), 0) < 0)
               throw std::runtime_error("FFmpeg could not store the requested video quality.");
         }
         if (avcodec_open2(pictureEncoder.get(), encoder, &encoderSettings) < 0 ||
             avcodec_parameters_from_context(videoStream->codecpar, pictureEncoder.get()) < 0)
            throw std::runtime_error("FFmpeg could not initialize the selected video encoder.");
         videoStream->time_base = pictureEncoder->time_base;
         videoStream->avg_frame_rate = pictureRate;
         if (pictureEncoder->codec_id == AV_CODEC_ID_HEVC && result.extension() == ".mp4")
            videoStream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
      }

      const auto *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
      if (!aac) throw std::runtime_error("The AAC audio encoder is unavailable.");
      VideoExportHandle<AVCodecContext, avcodec_free_context> audioEncoder{avcodec_alloc_context3(aac)};
      if (!audioEncoder) throw std::runtime_error("FFmpeg could not allocate the audio encoder.");
      audioEncoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
      audioEncoder->sample_rate = av_buffersink_get_sample_rate(audioSink);
      audioEncoder->time_base = {1, audioEncoder->sample_rate};
      audioEncoder->bit_rate = 256000;
      audioEncoder->flags |= globalHeader;
      if (av_buffersink_get_ch_layout(audioSink, &audioEncoder->ch_layout) < 0 ||
          avcodec_open2(audioEncoder.get(), aac, nullptr) < 0 ||
          avcodec_parameters_from_context(audioStream->codecpar, audioEncoder.get()) < 0)
         throw std::runtime_error("FFmpeg could not initialize the AAC audio encoder.");
      audioStream->time_base = audioEncoder->time_base;
      av_buffersink_set_frame_size(audioSink, audioEncoder->frame_size);

      AVIOInterruptCB interrupt{CheckVideoProbeCancellation, &progress};
      const auto opened = avio_open2(&output->pb, reinterpret_cast<const char *>(filename.c_str()),
         AVIO_FLAG_WRITE, &interrupt, nullptr);
      if (progress.cancelled) return false;
      if (opened < 0) throw std::runtime_error("FFmpeg could not open the staged export for writing.");
      AVDictionary *muxSettings{};
      const auto freeMuxSettings = finally(std::bind(av_dict_free, &muxSettings));
      if (result.extension() == ".mp4" && av_dict_set(&muxSettings, "movflags", "+faststart", 0) < 0)
         throw std::runtime_error("FFmpeg could not enable fast start.");
      if (avformat_write_header(output.get(), &muxSettings) < 0) {
         if (progress.cancelled) return false;
         throw std::runtime_error("FFmpeg could not start writing the selected container.");
      }

      std::vector<VideoExportDecoder *> filterInputs;
      for (auto &input : pictures) {
         if (input.filterInput) filterInputs.push_back(&input);
      }
      filterInputs.push_back(&audio);
      for (auto *input : filterInputs) {
         if (input != &audio) av_frame_remove_side_data(input->frame.get(), AV_FRAME_DATA_DISPLAYMATRIX);
         if (av_buffersrc_add_frame_flags(input->filterInput, input->frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
            throw std::runtime_error("FFmpeg could not start the export filters.");
      }

      const auto audioClock = av_buffersink_get_time_base(audioSink);
      const auto videoClock = copyPicture ? pictures.front().stream->time_base : av_buffersink_get_time_base(pictureSink);
      const auto pictureStart = copyPicture && pictures.front().stream->start_time != AV_NOPTS_VALUE
         ? pictures.front().stream->start_time : 0;
      const auto ticks = std::ceil(duration / av_q2d(videoClock));
      if (!(ticks < double(std::numeric_limits<int64_t>::max())))
         throw std::runtime_error("The export duration exceeds its stream clock.");
      const auto pictureLimit = static_cast<int64_t>(ticks);
      bool audioReady{}, pictureReady{}, audioDone{}, pictureDone{};
      while (!audioDone || !pictureDone || audioReady || pictureReady) {
         if (CheckVideoProbeCancellation(&progress)) return false;
         if (!audioDone && !audioReady) {
            const auto status = av_buffersink_get_frame(audioSink, nextAudio.get());
            audioDone = status == AVERROR_EOF;
            audioReady = status >= 0;
            if (!audioDone && !audioReady && status != AVERROR(EAGAIN))
               throw std::runtime_error("FFmpeg could not filter the exported audio.");
         }
         if (!pictureDone && !pictureReady) {
            if (copyPicture) {
               pictureReady = ReadVideoExportPacket(pictures.front(), *copiedPacket, progress);
               if (progress.cancelled) return false;
               if (pictureReady) {
                  if (copiedPacket->pts != AV_NOPTS_VALUE) copiedPacket->pts -= pictureStart;
                  if (copiedPacket->dts != AV_NOPTS_VALUE) copiedPacket->dts -= pictureStart;
                  const auto timestamp = copiedPacket->dts != AV_NOPTS_VALUE ? copiedPacket->dts : copiedPacket->pts;
                  if (timestamp != AV_NOPTS_VALUE && timestamp >= pictureLimit) {
                     av_packet_unref(copiedPacket.get());
                     pictureReady = false;
                  }
               }
               pictureDone = !pictureReady;
            }
            else {
               const auto status = av_buffersink_get_frame(pictureSink, nextPicture.get());
               pictureDone = status == AVERROR_EOF;
               pictureReady = status >= 0;
               if (!pictureDone && !pictureReady && status != AVERROR(EAGAIN))
                  throw std::runtime_error("FFmpeg could not render an exported picture.");
            }
         }

         if ((!audioReady && !audioDone) || (!pictureReady && !pictureDone)) {
            bool fed{};
            for (auto *input : filterInputs) {
               if (!input->filterInput || !av_buffersrc_get_nb_failed_requests(input->filterInput)) continue;
               const auto decoded = ReadVideoExportFrame(*input, *inputPacket, progress);
               if (progress.cancelled) return false;
               if (decoded && input != &audio) av_frame_remove_side_data(input->frame.get(), AV_FRAME_DATA_DISPLAYMATRIX);
               if (av_buffersrc_add_frame_flags(input->filterInput, decoded ? input->frame.get() : nullptr,
                     AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
                  throw std::runtime_error("FFmpeg could not feed an export filter.");
               if (!decoded) input->filterInput = nullptr;
               fed = true;
            }
            if (fed) continue;
         }

         if (!audioReady && !pictureReady) {
            if (audioDone && pictureDone) break;
            throw std::runtime_error("FFmpeg stopped requesting export input before its outputs completed.");
         }
         const auto pictureTime = copyPicture
            ? (copiedPacket->dts != AV_NOPTS_VALUE ? copiedPacket->dts : copiedPacket->pts) : nextPicture->pts;
         if (audioReady && (!pictureReady || av_compare_ts(nextAudio->pts, audioClock, pictureTime, videoClock) <= 0)) {
            DrainVideoExportEncoder(*output, *audioStream, *audioEncoder, nextAudio.get(), audioClock, *outputPacket);
            av_frame_unref(nextAudio.get());
            audioReady = false;
            continue;
         }
         if (copyPicture) {
            av_packet_rescale_ts(copiedPacket.get(), videoClock, videoStream->time_base);
            copiedPacket->stream_index = videoStream->index;
            if (av_interleaved_write_frame(output.get(), copiedPacket.get()) < 0) {
               if (progress.cancelled) return false;
               throw std::runtime_error("FFmpeg could not copy an original video packet.");
            }
         }
         else {
            DrainVideoExportEncoder(*output, *videoStream, *pictureEncoder, nextPicture.get(), videoClock, *outputPacket);
            av_frame_unref(nextPicture.get());
         }
         pictureReady = false;
      }
      if (pictureEncoder)
         DrainVideoExportEncoder(*output, *videoStream, *pictureEncoder, nullptr, videoClock, *outputPacket);
      DrainVideoExportEncoder(*output, *audioStream, *audioEncoder, nullptr, audioClock, *outputPacket);
      const auto finished = av_write_trailer(output.get());
      const auto closed = avio_closep(&output->pb);
      if (progress.cancelled) return false;
      if (finished < 0 || closed < 0) throw std::runtime_error("FFmpeg could not finish the staged export.");
      return true;
   }
   catch (...) {
      if (progress.cancelled) return false;
      throw;
   }
}

std::optional<std::pair<int, int>> ReadExportPictureSize(const fs::path &source, wxWindow *parent)
{
   wxProgressDialog dialog{_("Reading Video"), _("Reading original picture dimensions..."),
      100, parent, wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE | wxPD_ELAPSED_TIME};
   VideoProbeProgress progress{dialog};
   const auto input = OpenVideoExportDecoder(source, AVMEDIA_TYPE_VIDEO, false, progress);
   if (progress.cancelled) return {};
   return ReadVideoStreamDisplaySize(*input.stream);
}
} // namespace

std::optional<std::pair<int, int>> ReadVideoSourcePictureSize(const wxString &source, wxWindow *parent)
{ return ReadExportPictureSize(fs::absolute(fs::u8path(Utf8(source))), parent); }

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
#ifdef _WIN32
   const auto name = tool == VideoTool::Transcriber ? "autopsy-whisper.exe" :
      tool == VideoTool::Probe ? "ffprobe.exe" : "ffmpeg.exe";
#else
   const auto name = tool == VideoTool::Transcriber ? "autopsy-whisper" :
      tool == VideoTool::Probe ? "ffprobe" : "ffmpeg";
#endif
   auto directory = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPathWithSep();
#ifndef _WIN32
   directory += "../" AUTOPSY_MEDIA_RUNTIME_DIRECTORY "/";
#endif
   const auto executable = directory + name;
   if (!wxFileExists(executable))
      throw std::runtime_error(std::string{"The bundled "} + name + " is missing. Reinstall AuTopsy.");
   std::vector<wxString> native{executable};
   native.insert(native.end(), arguments.begin(), arguments.end());
   BundledMediaProcess process{native, maximumOutputBytes};
   if (!process.Wait(parent, title, status, tool == VideoTool::Transcriber)) return false;
   if (output) *output = std::move(process.output);
   return true;
}

void WriteMediaTextFile(const wxString &destination, const std::string &text, bool overwrite)
{
   const auto target = fs::absolute(fs::u8path(Utf8(destination)));
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
   const auto pending = workspace.path / "_captions.txt";
   std::ofstream output(pending, std::ios::binary|std::ios::trunc);
   output.write(text.data(), std::streamsize(text.size())); output.close();
   if (!output) throw std::runtime_error("Cannot write the subtitle file. The previous export is unchanged.");
#ifdef _WIN32
   if (!MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH|
         (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
      throw WindowsError("Publishing the subtitle file");
#else
   if (::renameat2(AT_FDCWD, pending.c_str(), AT_FDCWD, target.c_str(),
         overwrite ? 0 : RENAME_NOREPLACE) != 0)
      throw std::system_error(errno, std::generic_category(), "Publishing the subtitle file");
#endif
}

void RecordBundledVideoInput(const std::vector<wxString> &arguments,
   const wxString &destination, wxWindow *parent)
{
   auto directory = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPathWithSep();
#ifdef _WIN32
   const auto executable = directory + "ffmpeg.exe";
#else
   const auto executable = directory + "../" AUTOPSY_MEDIA_RUNTIME_DIRECTORY "/ffmpeg";
#endif
   if (!wxFileExists(executable)) throw std::runtime_error("The bundled FFmpeg is missing. Reinstall AuTopsy.");
   std::vector<wxString> native{executable};
   native.insert(native.end(), arguments.begin(), arguments.end());
   try {
      BundledMediaProcess process{native, 16384, destination};
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
   ClipActions::MediaDialogCommandGuard commands;
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
   const auto target = fs::absolute(fs::u8path(Utf8(destination)));
   if (!std::isfinite(duration) || duration <= 0 || duration + 1e-7 < timeline.End() ||
       (!timeline.clips.empty() && timeline.sourcePath.empty()) || destination.empty())
      throw std::runtime_error("Choose video clips and an output filename before exporting.");
   const auto extension = wxString(target.extension().native()).Lower();
   if (extension != ".mp4" && extension != ".mkv")
      throw std::runtime_error("Choose an .mp4 or .mkv output filename.");
   if (!fs::is_directory(target.parent_path()))
      throw std::runtime_error("The output folder is unavailable.");
   std::vector<fs::path> inputs;
   std::map<uint64_t, size_t> clipInputs;
   const auto protectSource = [&](const fs::path &input) {
#ifdef _WIN32
      const auto sameName = wxString(input.native()).CmpNoCase(wxString(target.native())) == 0;
#else
      const auto sameName = input == target;
#endif
      if (sameName ||
          (fs::exists(target) && fs::exists(input) && fs::equivalent(input, target)))
         throw std::runtime_error("The output must be different from every source video.");
   };
   for (const auto &path : timeline.protectedSourcePaths)
      protectSource(fs::absolute(fs::u8path(path)));
   for (const auto &path : inputTimeline.protectedSourcePaths)
      protectSource(fs::absolute(fs::u8path(path)));
   for (const auto &clip : timeline.clips) {
      const auto source = VideoTimeline::SourceFor(timeline, clip);
      const auto input = fs::absolute(fs::u8path(source.path));
      if (!fs::is_regular_file(input))
         throw std::runtime_error("A source video is unavailable: " + source.path);
      protectSource(input);
      const auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto &path) {
#ifdef _WIN32
         return wxString(input.native()).CmpNoCase(wxString(path.native())) == 0;
#else
         return input == path;
#endif
      });
      clipInputs.emplace(clip.id, size_t(found - inputs.begin()));
      if (found == inputs.end()) inputs.push_back(input);
   }
   if (fs::exists(target) && !overwrite)
      throw std::runtime_error("The output already exists. Choose another name or allow overwrite.");

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
   if (!WriteProjectAudioRange(project, mix, projectAudioStart, duration, parent)) return false;
   std::string picturePlan;
   std::vector<bool> renderedInputs(inputs.size());
   if (!copyPicture) {
      // FFmpeg owns decoding, trimming, gap frames and concatenation. This
      // adapter translates only our value model, never Audacity track internals.
      std::ostringstream graph;
      graph.imbue(std::locale::classic()); graph << std::setprecision(17);
      for (size_t input = 0; input < inputs.size(); ++input) {
         const auto count = std::count_if(spans.begin(), spans.end(),
            [&](const auto &span) { return span.clipId && clipInputs.at(span.clipId) == input; });
         if (!count) continue;
         renderedInputs[input] = true;
         graph << "[picture" << input << "]split=" << count;
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
      graph << "concat=n=" << spans.size() << ":v=1:a=0,settb=expr=1/(" << outputRate << "),setpts=N[vout]";
      picturePlan = graph.str();
   }
   if (!EncodeVideoExport(inputs, renderedInputs, mix, result, picturePlan, settings, outputRate, duration, parent))
      return false;
   if (!fs::is_regular_file(result) || fs::file_size(result) == 0)
      throw std::runtime_error("FFmpeg did not produce a video file.");
   // Staging is on the destination volume. Publishing is a single rename;
   // a failed encoder or a cancellation cannot overwrite a prior export.
#ifdef _WIN32
   if (!MoveFileExW(result.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH |
      (overwrite ? MOVEFILE_REPLACE_EXISTING : 0))) throw WindowsError("Publishing the exported video");
#else
   if (::renameat2(AT_FDCWD, result.c_str(), AT_FDCWD, target.c_str(),
         overwrite ? 0 : RENAME_NOREPLACE) != 0)
      throw std::system_error(errno, std::generic_category(), "Publishing the exported video");
#endif
   return true;
}
} // namespace VideoExport
