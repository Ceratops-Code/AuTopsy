#include "PrepareCaptionModels.h"
#include "PrepareVideoPreview.h"
#include "FileNames.h"
#include <wx/progdlg.h>
#include <wx/msw/wrapwin.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>
#include <thread>

namespace VideoCaptions {
namespace {
namespace fs = std::filesystem;
struct Model {
   const char *quality, *digest;
   uint64_t bytes;
};
constexpr Model Models[] = {
   {"tiny", "be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21", 77691713},
   {"base", "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe", 147951465},
   {"small", "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b", 487601967}
};
constexpr auto ModelRevision = "5359861c739e955e79d9a303bcbc70fb988958b1";
fs::path Root() { return fs::path(FileNames::DataDir().ToStdWstring()) / L"_captions"; }
struct Handle {
   HANDLE value{INVALID_HANDLE_VALUE};
   ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void CleanOwnedChildren(const fs::path &root)
{
   // The exclusive _lease handle excludes every live producer. Only exact owned
   // child names are cleaned; installed weights and unrelated files are retained.
   const auto work = root/L"_work";
   if (fs::is_symlink(work)) throw std::runtime_error("The caption workspace cannot be a symbolic link.");
   if (fs::exists(work)) fs::remove_all(work);
   for (const auto &model : Models) fs::remove(root/(std::string{"_"}+model.quality+".part"));
}
std::string HashModel(const fs::path &path)
{
   BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
   if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
      throw std::runtime_error("Windows cannot start the caption model integrity check.");
   struct Close { BCRYPT_ALG_HANDLE &a; BCRYPT_HASH_HANDLE &h;
      ~Close() { if (h) BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(a, 0); } } close{algorithm, hash};
   if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
      throw std::runtime_error("Windows cannot create the caption model hash.");
   std::ifstream file(path, std::ios::binary); std::array<unsigned char, 65536> buffer;
   if (!file) throw std::runtime_error("Cannot read the local caption model.");
   while (file) {
      file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
      if (file.gcount() && BCryptHashData(hash, buffer.data(), ULONG(file.gcount()), 0) < 0)
         throw std::runtime_error("Windows cannot hash the caption model.");
   }
   if (!file.eof()) throw std::runtime_error("Cannot finish reading the caption model.");
   std::array<unsigned char, 32> digest;
   if (BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) < 0)
      throw std::runtime_error("Windows cannot finish the caption model hash.");
   std::ostringstream result; result << std::hex << std::setfill('0');
   for (auto byte : digest) result << std::setw(2) << int(byte);
   return result.str();
}
struct HttpHandle {
   HINTERNET value{};
   ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
void RequireHttp(bool success, const char *operation)
{
   if (!success) throw std::runtime_error(std::string{operation}+" (Windows error "+std::to_string(GetLastError())+").");
}
bool DownloadModel(const Model &model, const fs::path &pending,
   std::atomic<bool> &cancelled, std::atomic<uint64_t> &received)
{
   // Windows' HTTPS client keeps this worker independent of optional host
   // networking modules. Bounded request timeouts also bound cancellation waits.
   HttpHandle session{WinHttpOpen(L"AuTopsy caption models", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
   RequireHttp(session.value != nullptr, "Starting the caption download");
   RequireHttp(WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000), "Setting caption download timeouts");
   HttpHandle connection{WinHttpConnect(session.value, L"huggingface.co", INTERNET_DEFAULT_HTTPS_PORT, 0)};
   RequireHttp(connection.value != nullptr, "Connecting to the caption model source");
   const auto path = wxString::FromUTF8(std::string{"/ggerganov/whisper.cpp/resolve/"}+ModelRevision+"/ggml-"+model.quality+".bin");
   HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", path.wc_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
   RequireHttp(request.value != nullptr, "Opening the caption model request");
   DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
   RequireHttp(WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)), "Protecting caption model redirects");
   DWORD credentials = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
   RequireHttp(WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &credentials, sizeof(credentials)), "Disabling caption download credentials");
   if (cancelled) return false;
   RequireHttp(WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
      WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "Requesting the caption model");
   RequireHttp(WinHttpReceiveResponse(request.value, nullptr), "Receiving the caption model");
   DWORD status{}, size = sizeof(status);
   RequireHttp(WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX), "Reading the caption download status");
   if (status != 200) throw std::runtime_error("Caption model download failed (HTTP "+std::to_string(status)+").");
   std::ofstream output(pending, std::ios::binary|std::ios::trunc);
   if (!output) throw std::runtime_error("Cannot create the caption model download.");
   std::array<char, 65536> buffer;
   while (!cancelled) {
      DWORD count{};
      RequireHttp(WinHttpReadData(request.value, buffer.data(), DWORD(buffer.size()), &count), "Reading the caption model");
      if (!count) break;
      if (received+count > model.bytes) throw std::runtime_error("The caption model download exceeds its declared size.");
      output.write(buffer.data(), count);
      if (!output) throw std::runtime_error("There is not enough writable space for the caption model.");
      received += count;
   }
   output.close();
   if (cancelled) return false;
   if (!output || received != model.bytes || HashModel(pending) != model.digest)
      throw std::runtime_error("The downloaded caption model does not match its published size and SHA-256.");
   return true;
}
}
struct CaptionWorkspace::State {
   fs::path root{Root()};
   Handle lease;
   State() {
      fs::create_directories(root);
      if (fs::is_symlink(root)) throw std::runtime_error("The caption cache cannot be a symbolic link.");
      lease.value = CreateFileW((root/L"_lease").c_str(), GENERIC_READ|GENERIC_WRITE,
         0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (lease.value == INVALID_HANDLE_VALUE)
         throw std::runtime_error("Another AuTopsy caption job is running. Finish or cancel it first.");
      CleanOwnedChildren(root); fs::create_directory(root/L"_work");
   }
   ~State() { try { CleanOwnedChildren(root); } catch (...) {} }
};
void CleanCaptionWorkspace()
{
   try {
      const auto root = Root(); if (!fs::is_directory(root) || fs::is_symlink(root)) return;
      Handle lease;
      lease.value = CreateFileW((root/L"_lease").c_str(), GENERIC_READ|GENERIC_WRITE,
         0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (lease.value != INVALID_HANDLE_VALUE) CleanOwnedChildren(root);
   } catch (...) { /* A later normal caption call reports any persistent error. */ }
}
CaptionWorkspace::CaptionWorkspace() : mState{std::make_unique<State>()} {}
CaptionWorkspace::~CaptionWorkspace() = default;
wxString CaptionWorkspace::File(const wxString &name) const
{
   const fs::path child{name.ToStdWstring()};
   if (child.has_parent_path() || child.empty()) throw std::runtime_error("Use a simple caption work-file name.");
   return wxString((mState->root/L"_work"/child).wstring());
}
wxString CaptionWorkspace::PrepareModel(const std::string &quality, wxWindow *parent)
{
   const auto found = std::find_if(std::begin(Models), std::end(Models),
      [&](const auto &model) { return quality == model.quality; });
   if (found == std::end(Models)) throw std::runtime_error("Choose tiny, base or small caption quality.");
   const auto &model = *found;
   const auto final = mState->root/(std::string{model.quality}+"-"+model.digest+".bin");
   if (fs::is_regular_file(final) && fs::file_size(final) == model.bytes && HashModel(final) == model.digest)
      return wxString(final.wstring());
   if (fs::exists(final)) fs::remove(final);
   // Each quality is one bounded version group. Unknown files are never removed.
   const std::string prefix = std::string{model.quality}+"-";
   for (const auto &entry : fs::directory_iterator(mState->root)) {
      const auto name = entry.path().filename().string();
      if (entry.is_regular_file() && name.size() == prefix.size()+68 && name.rfind(prefix, 0) == 0 &&
          name.substr(name.size()-4) == ".bin" && name.substr(prefix.size(), 64).find_first_not_of("0123456789abcdef") == std::string::npos)
         fs::remove(entry.path());
   }
   const auto pending = mState->root/(std::string{"_"}+quality+".part");
   wxProgressDialog progress{_("Download Caption Model"),
      _("Downloading the multilingual model once. Your audio stays on this computer."), 1000,
      parent, wxPD_APP_MODAL|wxPD_CAN_ABORT|wxPD_AUTO_HIDE|wxPD_ELAPSED_TIME};
   std::atomic<bool> cancelled{}; std::atomic<uint64_t> received{};
   auto worker = std::async(std::launch::async, [&] { return DownloadModel(model, pending, cancelled, received); });
   while (worker.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
      if (!progress.Update(int(received*1000/model.bytes))) cancelled = true;
   if (!worker.get()) throw VideoPreview::PreviewPreparationCancelled{};
   if (!MoveFileExW(pending.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Cannot publish the completed caption model.");
   return wxString(final.wstring());
}
}
