#include "CheckProductUpdates.h"
#include "ChooseProductUpdate.h"
#include "Prefs.h"
#include "CommandContext.h"
#include "CommonCommandFlags.h"
#include "MenuRegistry.h"
#include "commands/AudacityCommand.h"
#include "commands/LoadCommands.h"
#include "commands/CommandDispatch.h"
#include "SettingsVisitor.h"
#include <windows.h>
#include <winhttp.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/notifmsg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>

namespace AuTopsy::Updates {
std::string CurrentProductVersion() { return AUTOPSY_PRODUCT_VERSION; }
namespace {
constexpr size_t MaximumResponse = 2 * 1024 * 1024;
constexpr auto RequestLifetime = std::chrono::seconds(20);
using Clock = std::chrono::steady_clock;
struct FeedResult { std::vector<ReleaseOffer> offers; std::string error; };
struct HttpHandle {
   HINTERNET value{};
   ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
struct HttpResponse {
   std::mutex mutex;
   std::recursive_mutex api; // Closing and callback API calls cannot overlap.
   std::condition_variable changed;
   std::string body;
   std::array<char, 65536> buffer{};
   DWORD status{}, error{};
   bool complete{}, active{true};
};
void Fail(HttpResponse &state, DWORD error) {
   { std::lock_guard<std::mutex> lock(state.mutex); state.error = error; state.complete = true; }
   state.changed.notify_all();
}
void CALLBACK Receive(HINTERNET request, DWORD_PTR opaque, DWORD status, void *data, DWORD count) {
   if (!opaque) return;
   auto *context = reinterpret_cast<std::shared_ptr<HttpResponse>*>(opaque);
   const auto state = *context;
   // WinHTTP's final HANDLE_CLOSING callback releases its own shared context.
   // The data stays alive after cancellation and until every callback returns.
   if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { delete context; return; }
   try {
   std::lock_guard<std::recursive_mutex> api(state->api);
   if (!state->active) return;
   const auto check = [&](BOOL success) {
      if (!success && GetLastError() != ERROR_IO_PENDING) Fail(*state, GetLastError());
   };
   if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE) check(WinHttpReceiveResponse(request, nullptr));
   else if (status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) {
      DWORD size = sizeof(state->status);
      if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &state->status, &size, WINHTTP_NO_HEADER_INDEX)) Fail(*state, GetLastError());
      else if (state->status != 200) Fail(*state, ERROR_WINHTTP_INVALID_SERVER_RESPONSE);
      else check(WinHttpReadData(request, state->buffer.data(), static_cast<DWORD>(state->buffer.size()), nullptr));
   }
   else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
      bool done{};
      {
         std::lock_guard<std::mutex> lock(state->mutex);
         if (count > MaximumResponse - state->body.size()) { state->error = ERROR_FILE_TOO_LARGE; done = true; }
         else if (count) state->body.append(static_cast<const char*>(data), count);
         else done = true;
         if (done) state->complete = true;
      }
      if (done) state->changed.notify_all();
      else check(WinHttpReadData(request, state->buffer.data(), static_cast<DWORD>(state->buffer.size()), nullptr));
   }
   else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
      Fail(*state, count >= sizeof(WINHTTP_ASYNC_RESULT) ? static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError : ERROR_WINHTTP_INTERNAL_ERROR);
   }
   catch (...) { Fail(*state, ERROR_NOT_ENOUGH_MEMORY); }
}
std::string ReadPage(int page, const std::atomic<bool> &cancelled, Clock::time_point deadline) {
   HttpHandle session{WinHttpOpen(L"AuTopsy update check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
   if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000))
      throw std::runtime_error("Cannot start the Windows HTTPS client.");
   HttpHandle connection{WinHttpConnect(session.value, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
   if (!connection.value) throw std::runtime_error("Cannot connect to GitHub.");
   const auto path = L"/repos/" + std::wstring(GitHubRepository.begin(), GitHubRepository.end()) +
      L"/releases?per_page=100&page=" + std::to_wstring(page);
   HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
   if (!request.value) throw std::runtime_error("Cannot create the GitHub update request.");
   DWORD credentials = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
   if (!WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &credentials, sizeof(credentials)))
      throw std::runtime_error("Cannot disable automatic credentials for the public update request.");
   const auto state = std::make_shared<HttpResponse>();
   auto *context = new std::shared_ptr<HttpResponse>(state);
   DWORD_PTR value = reinterpret_cast<DWORD_PTR>(context);
   if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &value, sizeof(value)) ||
       WinHttpSetStatusCallback(request.value, Receive,
          WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
      delete context; throw std::runtime_error("Cannot receive the GitHub update response.");
   }
   // All handle effects are serialized with callbacks. An asynchronous request
   // can be cancelled safely; synchronous WinHTTP requests cannot be closed
   // concurrently. No certificate checks are disabled and no credentials sent.
   {
      std::lock_guard<std::recursive_mutex> api(state->api);
      if (!WinHttpSendRequest(request.value,
            L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2026-03-10\r\n",
            static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, value) && GetLastError() != ERROR_IO_PENDING)
         Fail(*state, GetLastError());
   }
   {
      std::unique_lock<std::mutex> lock(state->mutex);
      while (!state->complete && !cancelled && Clock::now() < deadline)
         state->changed.wait_for(lock, std::chrono::milliseconds(50));
   }
   {
      std::lock_guard<std::recursive_mutex> api(state->api);
      state->active = false; WinHttpCloseHandle(request.value); request.value = nullptr;
   }
   std::lock_guard<std::mutex> lock(state->mutex);
   if (cancelled) throw std::runtime_error("Update check cancelled.");
   if (!state->complete) throw std::runtime_error("GitHub did not respond before the update-check time limit.");
   if (state->status == 403 || state->status == 429) throw std::runtime_error("GitHub has limited update requests. Try again later or open the release page.");
   if (state->error) throw std::runtime_error("Cannot read GitHub updates (Windows error " + std::to_string(state->error) + "). Editing is unaffected.");
   return state->body;
}
std::string Text(const rapidjson::Value &object, const char *name, size_t maximum = 65536) {
   const auto field = object.FindMember(name);
   if (field == object.MemberEnd() || !field->value.IsString()) return {};
   return {field->value.GetString(), std::min<size_t>(field->value.GetStringLength(), maximum)};
}
size_t ReadOffers(const std::string &body, std::vector<ReleaseOffer> &offers) {
   if (body.size() > MaximumResponse) throw std::runtime_error("The update response exceeds its size limit.");
   rapidjson::Document json; json.Parse(body.data(), body.size());
   if (json.HasParseError() || !json.IsArray() || json.Size() > 100)
      throw std::runtime_error("GitHub returned an unsupported update response.");
   for (const auto &release : json.GetArray()) {
      if (!release.IsObject() || !release.HasMember("draft") || !release["draft"].IsBool() ||
          !release.HasMember("prerelease") || !release["prerelease"].IsBool()) continue;
      ReleaseOffer offer{Text(release, "tag_name", 128), Text(release, "body", 16384), Text(release, "html_url", 1024), {},
         release["draft"].GetBool(), release["prerelease"].GetBool()};
      if (release.HasMember("assets") && release["assets"].IsArray()) for (const auto &asset : release["assets"].GetArray()) {
         if (!asset.IsObject() || !IsWindowsX64Installer(Text(asset, "name", 256)) || Text(asset, "state") != "uploaded" ||
             !asset.HasMember("size") || !asset["size"].IsUint64() || !asset["size"].GetUint64()) continue;
         offer.download = Text(asset, "browser_download_url", 2048); break;
      }
      offers.push_back(std::move(offer));
   }
   return json.Size();
}
std::optional<std::string> TestFeed() {
   wxString root, selected;
   if (!wxGetEnv("AUTOPSY_BEHAVIOR_ROOT", &root) || !wxGetEnv("AUTOPSY_TEST_UPDATES_JSON", &selected) ||
       root.empty() || selected.empty()) return {};
   namespace fs = std::filesystem;
   const auto owner = fs::canonical(fs::path(root.ToStdWstring()));
   const auto path = fs::canonical(fs::path(selected.ToStdWstring()));
   std::ifstream marker(path.parent_path() / "_owner"); std::string signature; std::getline(marker, signature);
   const auto relative = path.lexically_relative(owner);
   if (signature != "AuTopsy behavior tests 1" || relative.empty() || *relative.begin() == ".." ||
       !fs::is_regular_file(path) || fs::file_size(path) > MaximumResponse)
      throw std::runtime_error("The private update fixture is outside its owned test directory.");
   std::ifstream file(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>{file}, {});
}
FeedResult ReadFeed(const std::atomic<bool> &cancelled, const std::optional<std::string> &fixture) {
   FeedResult result;
   try {
      if (fixture) ReadOffers(*fixture, result.offers);
      else {
         const auto deadline = Clock::now() + RequestLifetime;
         bool complete{};
         for (int page = 1; page <= 5 && !complete; ++page)
            complete = ReadOffers(ReadPage(page, cancelled, deadline), result.offers) < 100;
         if (!complete) throw std::runtime_error("More than 500 releases are available. Browse GitHub to check the complete release list.");
      }
   }
   catch (const std::exception &error) { result.error = error.what(); result.offers.clear(); }
   return result;
}
std::int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::int64_t LastUpdateAttemptTime() {
   wxLongLong_t value{}; gPrefs->Read("/AuTopsy/Updates/LastAttempt", "0").ToLongLong(&value); return value;
}
class Monitor final : public wxEvtHandler {
   wxTimer timer;
   std::shared_ptr<std::atomic<bool>> cancelled;
   std::future<FeedResult> request;
   bool automaticRequest{};
   std::unique_ptr<wxNotificationMessage> notice;
public:
   std::optional<FeedResult> latest;
   unsigned generation{};
   int dialogs{};
   Monitor() {
      timer.SetOwner(this); Bind(wxEVT_TIMER, [this](wxTimerEvent &) { Poll();
         if (ShouldCheckProductUpdates(gPrefs->ReadBool("/AuTopsy/Updates/Automatic", false), Now(), LastUpdateAttemptTime(),
               gPrefs->ReadLong("/AuTopsy/Updates/Frequency", 0) == 1 ? 604800 : 86400, request.valid()) && !dialogs)
            Start(true);
      }); timer.Start(60000);
   }
   ~Monitor() override { timer.Stop(); if (cancelled) *cancelled = true; if (request.valid()) request.wait(); }
   bool Busy() const { return request.valid(); }
   void Start(bool automatic = false) {
      if (Busy()) return;
      latest.reset(); automaticRequest = automatic;
      cancelled = std::make_shared<std::atomic<bool>>(false);
      const auto stop = cancelled;
      std::optional<std::string> fixture;
      try { fixture = TestFeed(); }
      catch (const std::exception &error) { latest = FeedResult{{}, error.what()}; ++generation; return; }
      request = std::async(std::launch::async, [stop, fixture] { return ReadFeed(*stop, fixture); });
      // Frequency bounds attempts too: an offline start must not retry every
      // timer tick. A manual check remains available at any time.
      gPrefs->Write("/AuTopsy/Updates/LastAttempt", wxString::Format("%lld", Now())); gPrefs->Flush();
   }
   void Poll() {
      if (!Busy() || request.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
      latest = request.get(); ++generation;
      if (!latest->error.empty()) return;
      const auto current = ParseReleaseVersion(CurrentProductVersion());
      const auto offer = current ? ChooseProductUpdate(latest->offers, *current,
         gPrefs->ReadBool("/AuTopsy/Updates/Previews", true)) : std::optional<ReleaseOffer>{};
      if (automaticRequest && !dialogs && offer && gPrefs->Read("/AuTopsy/Updates/LastNotice", "") != wxString::FromUTF8(offer->tag)) {
         // A passive notice never stops editing or starts a download. A later
         // manual check offers the same release if Windows suppresses balloons.
         notice = std::make_unique<wxNotificationMessage>(_("AuTopsy update available"),
            wxString::FromUTF8(offer->tag) + _(" is available on GitHub. Use Help > Check for AuTopsy Updates."),
            wxTheApp->GetTopWindow());
         notice->Show(10);
         gPrefs->Write("/AuTopsy/Updates/LastNotice", wxString::FromUTF8(offer->tag)); gPrefs->Flush();
      }
   }
};
std::shared_ptr<Monitor> GetMonitor() {
   static std::weak_ptr<Monitor> current;
   auto result = current.lock(); if (!result) { result = std::make_shared<Monitor>(); current = result; } return result;
}
std::shared_ptr<Monitor> applicationMonitor;
void ShowUpdates(const CommandContext &) {
   const auto monitor = GetMonitor(); ++monitor->dialogs;
   struct DialogLease { Monitor &monitor; ~DialogLease() { --monitor.dialogs; } } lease{*monitor};
   wxDialog dialog{wxTheApp->GetTopWindow(), wxID_ANY, _("AuTopsy Updates"), wxDefaultPosition, {620, 500},
      wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER};
   auto *layout = new wxBoxSizer(wxVERTICAL);
   layout->Add(new wxStaticText(&dialog, wxID_ANY, _("This build: ") + wxString::FromUTF8(CurrentProductVersion())), 0, wxALL, 10);
   auto *status = new wxStaticText(&dialog, wxID_ANY, _("Checking GitHub...")); status->SetName("AuTopsy update status");
   status->Wrap(580); layout->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
   auto *notes = new wxTextCtrl(&dialog, wxID_ANY, {}, wxDefaultPosition, {580, 200}, wxTE_MULTILINE | wxTE_READONLY);
   notes->SetName("AuTopsy release notes"); layout->Add(notes, 1, wxEXPAND | wxALL, 10);
   auto *previews = new wxCheckBox(&dialog, wxID_ANY, _("Include preview versions"));
   previews->SetValue(gPrefs->ReadBool("/AuTopsy/Updates/Previews", true)); layout->Add(previews, 0, wxALL, 6);
   auto *automatic = new wxCheckBox(&dialog, wxID_ANY, _("Check automatically (public release metadata only)"));
   automatic->SetValue(gPrefs->ReadBool("/AuTopsy/Updates/Automatic", false)); layout->Add(automatic, 0, wxALL, 6);
   auto *frequency = new wxChoice(&dialog, wxID_ANY); frequency->Append(_("Daily")); frequency->Append(_("Weekly"));
   frequency->SetSelection(gPrefs->ReadLong("/AuTopsy/Updates/Frequency", 0) == 1 ? 1 : 0); layout->Add(frequency, 0, wxALL, 6);
   auto *buttons = new wxBoxSizer(wxHORIZONTAL);
   auto *page = new wxButton(&dialog, wxID_ANY, _("Open Release Page"));
   auto *download = new wxButton(&dialog, wxID_ANY, _("Download Installer"));
   auto *close = new wxButton(&dialog, wxID_CLOSE, _("Close"));
   for (auto *button : {page, download, close}) buttons->Add(button, 0, wxALL, 5);
   layout->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 5);
   layout->Add(new wxStaticText(&dialog, wxID_ANY, _("Downloads open in your browser. Installation is your choice; open projects are never closed automatically.")),
      0, wxEXPAND | wxALL, 10);
   dialog.SetSizer(layout); layout->SetSizeHints(&dialog);
   std::optional<ReleaseOffer> selected;
   const auto refresh = [&] {
      download->Enable(false);
      if (!monitor->latest) { status->SetLabel(_("Checking GitHub...")); return; }
      if (!monitor->latest->error.empty()) { status->SetLabel(wxString::FromUTF8(monitor->latest->error)); notes->Clear(); return; }
      const auto current = ParseReleaseVersion(CurrentProductVersion());
      selected = current ? ChooseProductUpdate(monitor->latest->offers, *current, previews->GetValue()) : std::optional<ReleaseOffer>{};
      status->SetLabel(selected ? _("New release: ") + wxString::FromUTF8(selected->tag)
         : _("No newer Windows x64 installer is available in the selected channel."));
      notes->ChangeValue(selected ? wxString::FromUTF8(selected->notes) : wxString{}); download->Enable(bool(selected));
      status->Wrap(580); dialog.Layout();
   };
   previews->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent &) { gPrefs->Write("/AuTopsy/Updates/Previews", previews->GetValue()); gPrefs->Flush(); refresh(); });
   automatic->Bind(wxEVT_CHECKBOX, [&](wxCommandEvent &) { gPrefs->Write("/AuTopsy/Updates/Automatic", automatic->GetValue()); gPrefs->Flush(); });
   frequency->Bind(wxEVT_CHOICE, [&](wxCommandEvent &) { gPrefs->Write("/AuTopsy/Updates/Frequency", frequency->GetSelection()); gPrefs->Flush(); });
   page->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { wxLaunchDefaultBrowser(wxString::FromUTF8(selected ? selected->page : ProductReleasesPage())); });
   download->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { if (selected) wxLaunchDefaultBrowser(wxString::FromUTF8(selected->download)); });
   close->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { dialog.EndModal(wxID_CLOSE); });
   monitor->Start(); refresh(); unsigned seen = monitor->generation;
   wxTimer timer{&dialog}; dialog.Bind(wxEVT_TIMER, [&](wxTimerEvent &) {
      monitor->Poll(); if (seen != monitor->generation) { seen = monitor->generation; refresh(); }
   }); timer.Start(100); dialog.ShowModal(); timer.Stop();
}
class GetProductUpdatesCommand final : public AudacityCommand {
   bool previews{true};
public:
   static const ComponentInterfaceSymbol Symbol;
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override { return XO("Read newer AuTopsy Windows releases from GitHub without downloading or installing anything."); }
   template<bool Const> bool Settings(SettingsVisitorBase<Const> &v) { v.Define(previews, wxT("Previews"), true); return true; }
   bool VisitSettings(SettingsVisitor &v) override { return Settings(v); }
   bool VisitSettings(ConstSettingsVisitor &v) override { return Settings(v); }
   bool Apply(const CommandContext &context) override {
      const std::atomic<bool> cancelled{false}; FeedResult feed;
      try { feed = ReadFeed(cancelled, TestFeed()); }
      catch (const std::exception &error) { feed.error = error.what(); }
      const auto current = ParseReleaseVersion(CurrentProductVersion());
      const auto selected = current && feed.error.empty() ? ChooseProductUpdate(feed.offers, *current, previews) : std::optional<ReleaseOffer>{};
      rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
      out.StartObject(); out.Key("current"); out.String(CurrentProductVersion().c_str()); out.Key("error"); out.String(feed.error.c_str());
      out.Key("available"); out.Bool(bool(selected));
      if (selected) { out.Key("tag"); out.String(selected->tag.c_str()); out.Key("page"); out.String(selected->page.c_str());
         out.Key("download"); out.String(selected->download.c_str()); }
      out.EndObject(); context.Status(wxString::FromUTF8(buffer.GetString())); return true;
   }
};
const ComponentInterfaceSymbol GetProductUpdatesCommand::Symbol{XO("GetAuTopsyUpdates")};
BuiltinCommandsModule::Registration<GetProductUpdatesCommand> registered;
MenuRegistry::AttachedItem updatesMenu{MenuRegistry::Command("CheckAuTopsyUpdates", XXO("Check for AuTopsy &Updates..."),
   ShowUpdates, AlwaysEnabledFlag), wxT("Help/Extra")};
MenuRegistry::AttachedItem updatesScript{MenuRegistry::Command("GetAuTopsyUpdates", XXO("Get AuTopsy Updates"),
   CommandDispatch::OnAudacityCommand, AlwaysEnabledFlag), wxT("Optional/Extra/Part2/Scriptables2")};
}
void StartProductUpdateChecks() { if (!applicationMonitor) applicationMonitor = GetMonitor(); }
void StopProductUpdateChecks() { applicationMonitor.reset(); }
}
