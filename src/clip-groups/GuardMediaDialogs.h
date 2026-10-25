// Keep external scripting requests outside a media dialog's native lifetime.
// Only the GUI thread uses this scope. Commands retain their own response and
// are posted normally after the outermost scope has destroyed its windows.
#pragma once
#include <functional>
namespace ClipActions {
class MediaDialogCommandGuard final {
public:
   MediaDialogCommandGuard();
   ~MediaDialogCommandGuard();
   MediaDialogCommandGuard(const MediaDialogCommandGuard&) = delete;
   MediaDialogCommandGuard &operator=(const MediaDialogCommandGuard&) = delete;
};
bool QueueCommandDuringMediaDialog(std::function<void()> command);
}
