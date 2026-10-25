#include "GuardMediaDialogs.h"
#include <wx/app.h>
#include <cassert>
#include <utility>
#include <vector>

namespace {
unsigned dialogDepth{};
std::vector<std::function<void()>> pendingCommands;
}
namespace ClipActions {
MediaDialogCommandGuard::MediaDialogCommandGuard() { ++dialogDepth; }
MediaDialogCommandGuard::~MediaDialogCommandGuard()
{
   assert(dialogDepth);
   if (--dialogDepth) return;
   auto ready = std::move(pendingCommands); pendingCommands.clear();
   // Do not execute callbacks in a window destructor or resume saved steps.
   // They are complete fresh requests, handled by the normal app event queue.
   if (wxTheApp) for (auto &command : ready) wxTheApp->CallAfter(std::move(command));
}
bool QueueCommandDuringMediaDialog(std::function<void()> command)
{
   if (!dialogDepth) return false;
   pendingCommands.push_back(std::move(command)); return true;
}
}
