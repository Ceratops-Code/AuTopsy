// Application-owned model cache and per-job files. Models are pinned multilingual
// weights; input WAVs and transcripts are private, disposable and never logged.
#pragma once
#include <memory>
#include <wx/string.h>
class wxWindow;
namespace VideoCaptions {
void CleanCaptionWorkspace();
class CaptionWorkspace final {
   struct State;
   std::unique_ptr<State> mState;
public:
   CaptionWorkspace();
   ~CaptionWorkspace();
   CaptionWorkspace(const CaptionWorkspace&) = delete;
   CaptionWorkspace &operator=(const CaptionWorkspace&) = delete;
   wxString File(const wxString &name) const;
   wxString PrepareModel(const std::string &quality, wxWindow *parent);
};
}
