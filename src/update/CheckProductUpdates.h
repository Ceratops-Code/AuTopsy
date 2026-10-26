// Application adapter for the owned release policy. Checks fetch public release
// metadata only. Browser links require a user action; no installer is executed.
#pragma once
#include <memory>
#include <string>
namespace AuTopsy::Updates {
std::string CurrentProductVersion();
// The application's startup/shutdown boundary owns requests and notices.
void StartProductUpdateChecks();
void StopProductUpdateChecks();
}
