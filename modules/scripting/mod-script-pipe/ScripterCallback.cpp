// ScripterCallback.cpp : 
//
// A loadable module that connects a windows named pipe
// to a registered service function that is able to
// process a single command at a time.
//
// The service function is provided by the application
// and not by libscript.  mod_script_pipe was developed for
// Audacity.  Because it forwards commands
// rather than handling them itself it can be used in
// other projects too.  
//
// Enabling other programs to connect to Audacity via a pipe is a potential 
// security risk.  Use at your own risk.

#include <wx/wx.h>
#include "ScripterCallback.h"
#include "commands/ScriptCommandRelay.h"
#include "ModuleConstants.h"

#ifdef __WXMSW__
#include <wx/eventfilter.h>
#include <wx/msw/wrapwin.h>
#include <commctrl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>

namespace {
// This adapter belongs to the private scripting-test session. It uses native
// window contracts, without project types, captions, or upstream window layout.
// wx creates its top-level windows hidden. Attach before their first Show(),
// then constrain every placement before Windows applies it (including splash).
class TestDisplayGuard final : public wxEventFilter {
   wxString mDevice;
   std::set<HWND> mWindows;

   [[noreturn]] static void Fail(const char *operation)
   {
      std::fprintf(stderr, "AuTopsy test display guard: %s; Windows error %lu\n",
         operation, GetLastError());
      std::fflush(stderr);
      // Only the runner's already checked private app session reaches here.
      // Abort before showing unsafe UI; never display a failure dialog.
      std::_Exit(197);
   }

   RECT WorkArea() const
   {
      struct Displays {
         const wxString &device;
         RECT work{};
         unsigned count{}, matches{};
         DWORD queryError{};
      } displays{mDevice};
      const auto enumerated = EnumDisplayMonitors(nullptr, nullptr,
         [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto &found = *reinterpret_cast<Displays *>(data);
            MONITORINFOEXW info{}; info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO *>(&info))) {
               found.queryError = GetLastError();
               return FALSE;
            }
            ++found.count;
            if (found.device == info.szDevice) {
               ++found.matches;
               found.work = info.rcWork;
            }
            return TRUE;
         }, reinterpret_cast<LPARAM>(&displays));
      if (!enumerated || displays.matches != 1 ||
         (displays.count > 1 && mDevice == L"\\\\.\\DISPLAY1") ||
         displays.work.right - displays.work.left < 640 ||
         displays.work.bottom - displays.work.top < 480) {
         std::fprintf(stderr, "Test display enumeration: completed=%d count=%u matches=%u "
            "work=%ld,%ld,%ld,%ld query_error=%lu\n", enumerated, displays.count,
            displays.matches, displays.work.left, displays.work.top,
            displays.work.right, displays.work.bottom, displays.queryError);
         Fail("the selected test display is missing, forbidden, or too small");
      }
      return displays.work;
   }

   void Constrain(HWND window, WINDOWPOS &position) const
   {
      const auto work = WorkArea();
      RECT current{};
      if (!GetWindowRect(window, &current)) Fail("read test window placement");
      const int width = std::clamp<int>((position.flags & SWP_NOSIZE) ?
         current.right-current.left : position.cx, 1, work.right-work.left);
      const int height = std::clamp<int>((position.flags & SWP_NOSIZE) ?
         current.bottom-current.top : position.cy, 1, work.bottom-work.top);
      position.x = std::clamp<int>((position.flags & SWP_NOMOVE) ?
         current.left : position.x, work.left, work.right-width);
      position.y = std::clamp<int>((position.flags & SWP_NOMOVE) ?
         current.top : position.y, work.top, work.bottom-height);
      position.cx = width; position.cy = height;
      position.flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
   }

   static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
      WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
   {
      auto &guard = *reinterpret_cast<TestDisplayGuard *>(data);
      if (message == WM_NCDESTROY) {
         guard.mWindows.erase(window);
         RemoveWindowSubclass(window, WindowProcedure, id);
      }
      if (message == WM_DISPLAYCHANGE) guard.WorkArea();
      if (message == WM_WINDOWPOSCHANGING) {
         // Let the host enforce its size rules first; the test display is the
         // final bound, so a minimum size cannot push UI onto another screen.
         auto result = DefSubclassProc(window, message, wParam, lParam);
         guard.Constrain(window, *reinterpret_cast<WINDOWPOS *>(lParam));
         return result;
      }
      if (message == WM_SHOWWINDOW && wParam) {
         WINDOWPOS position{}; position.hwnd = window;
         position.flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER;
         guard.Constrain(window, position);
         if (!SetWindowPos(window, nullptr, position.x, position.y,
            position.cx, position.cy, position.flags)) Fail("place test window before showing");
         std::fprintf(stderr, "AUTOPSY_TEST_WINDOW show %llu %d %d %d %d\n",
            static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(window)),
            position.x, position.y, position.cx, position.cy);
         std::fflush(stderr);
      }
      return DefSubclassProc(window, message, wParam, lParam);
   }

public:
   explicit TestDisplayGuard(wxString device) : mDevice{std::move(device)}
   {
      WorkArea();
      wxEvtHandler::AddFilter(this);
   }
   ~TestDisplayGuard() override
   {
      wxEvtHandler::RemoveFilter(this);
      for (auto window : mWindows)
         RemoveWindowSubclass(window, WindowProcedure, 1);
   }
   int FilterEvent(wxEvent &event) override
   {
      if (event.GetEventType() == wxEVT_CREATE) {
         auto window = dynamic_cast<wxWindow *>(event.GetEventObject());
         if (window && window->IsTopLevel()) {
            const auto handle = static_cast<HWND>(window->GetHandle());
            if (IsWindowVisible(handle)) Fail("test window was shown before its display guard");
            if (!SetWindowSubclass(handle, WindowProcedure, 1,
               reinterpret_cast<DWORD_PTR>(this))) Fail("attach test window display guard");
            mWindows.insert(handle);
         }
      }
      return Event_Skip;
   }
};

std::unique_ptr<TestDisplayGuard> testDisplayGuard;

void InstallTestDisplayGuard()
{
   // The host checks the marked portable profile and private audio directory
   // before loading modules. Ordinary scripting users never enter this mode.
   if (!wxGetEnv("AUTOPSY_BEHAVIOR_ROOT", nullptr)) return;
   wxString device;
   if (!wxGetEnv("AUTOPSY_TEST_DISPLAY", &device)) {
      std::fprintf(stderr, "AuTopsy private tests require AUTOPSY_TEST_DISPLAY.\n");
      std::_Exit(197);
   }
   testDisplayGuard = std::make_unique<TestDisplayGuard>(std::move(device));
}
}

// The runner reads this exported data without executing or loading dependencies.
// Refuse older modules before launching an app that could show on screen 1.
extern "C" DLL_API const unsigned AuTopsyTestDisplayGuardVersion = 1;
#endif

/*
//#define ModuleDispatchName "ModuleDispatch"
See the example in this file.  It has several cases/options in it.
*/

extern void PipeServer();
typedef DLL_IMPORT int (*tpExecScriptServerFunc)( wxString * pIn, wxString * pOut);
static tpExecScriptServerFunc pScriptServerFn=NULL;


extern "C" {

// And here is our special registration function.
int DLL_API RegScriptServerFunc( tpExecScriptServerFunc pFn )
{
   if( pFn )
   {
      pScriptServerFn = pFn;
      PipeServer();
   }

   return 4;
}

DEFINE_VERSION_CHECK
extern "C" DLL_API int ModuleDispatch(ModuleDispatchTypes type)
{
   switch (type) {
#ifdef __WXMSW__
   case ModuleInitialize:
      InstallTestDisplayGuard();
      break;
   case ModuleTerminate:
      testDisplayGuard.reset();
      break;
#endif
   // Module loading precedes the first project. Publish the pipe only after
   // command handlers have a project to serve, rather than dropping early calls.
   case AppInitialized:
      ScriptCommandRelay::StartScriptServer(RegScriptServerFunc);
      break;
   default:
      break;
   }
   return 1;
}

wxString Str2;
wxArrayString aStr;
unsigned int currentLine;
size_t currentPosition;

// Send the received command to Audacity and build an array of response lines.
// The response lines can be retrieved by calling DoSrvMore repeatedly.
int DoSrv(char *pIn)
{
   // Interpret string as unicode.
   // wxWidgets (now) uses unicode internally.
   // Scripts must send unicode strings (if going beyond 7-bit ASCII).
   // Important for filenames in commands.
   wxString Str1(pIn, wxConvUTF8); 
   Str1.Replace( wxT("\r"), wxT(""));
   Str1.Replace( wxT("\n"), wxT(""));
   Str2 = wxEmptyString;
   (*pScriptServerFn)( &Str1 , &Str2);

   Str2 += wxT('\n');
   size_t outputLength = Str2.Length();
   aStr.Clear();
   size_t iStart = 0;
   size_t i;
   for(i = 0; i < outputLength; ++i)
   {
      if( Str2[i] == wxT('\n') )
      {
         aStr.Add( Str2.Mid( iStart, i-iStart) + wxT("\n") );
         iStart = i+1;
      }
   }

   currentLine     = 0;
   currentPosition = 0;

   return 1;
}

size_t smin(size_t a, size_t b) { return a < b ? a : b; }

// Write up to nMax characters of the prepared (by DoSrv) response lines.
// Returns the number of characters sent, including null.
// Zero returned if and only if there's nothing else to send.
int DoSrvMore(char *pOut, size_t nMax)
{
   wxASSERT(currentLine >= 0);
   wxASSERT(currentPosition >= 0);

   size_t totalLines = aStr.GetCount();
   while (currentLine < totalLines)
   {
      wxCharBuffer lineString    = aStr[currentLine].ToUTF8();
      size_t lineLength      = lineString.length();
      size_t charsLeftInLine = lineLength - currentPosition;

      wxASSERT(charsLeftInLine >= 0);

      if (charsLeftInLine == 0)
      {
         // Move to next line
         ++currentLine;
         currentPosition = 0;
      }
      else
      {
         // Write as much of the rest of the line as will fit in the buffer
         size_t charsToWrite = smin(charsLeftInLine, nMax - 1);
         memcpy(pOut, 
                &(lineString.data()[currentPosition]),
                charsToWrite);
         pOut[charsToWrite] = '\0';
         currentPosition    += charsToWrite;
         // Need to cast to prevent compiler warnings
         int charsWritten = static_cast<int>(charsToWrite + 1);
         // (Check cast was safe)
         wxASSERT(static_cast<size_t>(charsWritten) == charsToWrite + 1);
         return charsWritten;
      }
   }
   return 0;
}

} // End extern "C"
