#include "ShowVideoCrop.h"
#include "ExportVideoMix.h"
#include "RenderVideoComposition.h"
#include "PrepareGeneratedVideo.h"
#include "PrepareVideoEffects.h"
#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {
wxString Decimal(double value)
{
   std::ostringstream text; text.imbue(std::locale::classic()); text << std::setprecision(17) << value;
   return wxString::FromUTF8(text.str());
}
std::optional<wxImage> ReadCropPicture(wxWindow *parent, const VideoTimeline::Clip &clip,
   const VideoTimeline::MediaSource &fallback, double at, bool removeOuterCrop = true)
{
   auto uncropped = clip; if (removeOuterCrop) uncropped.crop = {}; uncropped.muted = false;
   at = std::clamp(at, clip.start, std::max(clip.start, clip.End() - 0.001));
   auto sample = VideoTimeline::SampleVideoContent(uncropped, fallback, uncropped.ContentTimeAtProjectTime(at));
   if (!sample) for (const auto edge : VideoTimeline::VideoContentEdges(uncropped)) {
      if (edge < clip.start || edge >= clip.End()) continue;
      sample = VideoTimeline::SampleVideoContent(uncropped, fallback, uncropped.ContentTimeAtProjectTime(edge));
      if (sample) break;
   }
   if (!sample) throw std::runtime_error("This clip has no visible source picture. Separate its retained pieces to crop them.");
   std::shared_ptr<const VideoPreview::PreparedMedia> generated;
   if (sample.generated) {
      const auto rate = sample.generated->frameRate;
      const auto from = std::floor(sample.sourceTime * rate + 1e-8) / rate;
      auto picture = VideoEditing::PrepareGeneratedVideo(*sample.generated, {0, 0, 540, rate, false}, parent,
         from, std::min(sample.generated->duration, from + 1 / rate));
      generated = std::move(picture.media);
      const auto path = generated->playbackPath.ToUTF8();
      sample.source.path.assign(path.data(), path.length()); sample.sourceTime = 0;
   }
   auto filter = VideoEditing::VideoCropFilterExpression(sample.crop);
   if (!filter.empty()) filter += ',';
   filter += "scale=960:540:force_original_aspect_ratio=decrease,setsar=1";
   std::string pixels;
   // Read one oriented picture directly into a bounded buffer. There is no
   // scratch image, cached failed diagnostic or child left after cancellation.
   if (!VideoExport::RunBundledVideoTool({"-v", "error", "-nostdin", "-ss", Decimal(sample.sourceTime),
         "-i", wxString::FromUTF8(sample.source.path), "-map", "0:v:0", "-frames:v", "1", "-vf",
         wxString::FromUTF8(filter), "-an", "-f", "image2pipe", "-c:v", "png", "pipe:1"},
         parent, _("Reading Crop Preview"), _("Reading a picture for crop handles..."), &pixels,
         VideoExport::VideoTool::Encoder, 4 * 1024 * 1024)) return {};
   wxMemoryInputStream input{pixels.data(), pixels.size()};
   wxImage picture{input, wxBITMAP_TYPE_PNG};
   if (!picture.IsOk()) throw std::runtime_error("The source picture could not be decoded for crop handles.");
   return picture;
}

class CropPicture final : public wxPanel {
   wxImage mPicture;
   std::optional<wxImage> mOverlay;
   VideoEditing::VideoFrameCrop mCrop, mInitial;
   wxPoint mDragStart;
   unsigned mHandle{};
   std::function<void(const VideoEditing::VideoFrameCrop&)> mChanged;
   wxRect PictureBounds() const {
      const auto client = GetClientSize();
      const auto scale = std::max(0.001, std::min(double(std::max(1, client.x - 24)) / mPicture.GetWidth(),
         double(std::max(1, client.y - 24)) / mPicture.GetHeight()));
      const int width = std::max(1, int(mPicture.GetWidth() * scale)), height = std::max(1, int(mPicture.GetHeight() * scale));
      return {(client.x - width) / 2, (client.y - height) / 2, width, height};
   }
   wxRect CropBounds(const wxRect &picture) const {
      const int left = picture.x + int(std::round(picture.width * mCrop.left));
      const int top = picture.y + int(std::round(picture.height * mCrop.top));
      return {left, top, std::max(1, int(std::round(picture.width * (1 - mCrop.right))) + picture.x - left),
         std::max(1, int(std::round(picture.height * (1 - mCrop.bottom))) + picture.y - top)};
   }
   unsigned Hit(wxPoint point) const {
      const auto picture = PictureBounds(), crop = CropBounds(picture);
      if (!wxRect(crop.x - 8, crop.y - 8, crop.width + 16, crop.height + 16).Contains(point)) return 0;
      unsigned edges{};
      if (std::abs(point.x - crop.x) <= 8) edges |= VideoEditing::CropLeft;
      else if (std::abs(point.x - (crop.x + crop.width)) <= 8) edges |= VideoEditing::CropRight;
      if (std::abs(point.y - crop.y) <= 8) edges |= VideoEditing::CropTop;
      else if (std::abs(point.y - (crop.y + crop.height)) <= 8) edges |= VideoEditing::CropBottom;
      return edges ? edges : crop.Contains(point) ? VideoEditing::CropMove : 0;
   }
   void Paint(wxPaintEvent &) {
      wxAutoBufferedPaintDC dc{this}; dc.SetBackground(wxBrush{wxColour{30, 30, 30}}); dc.Clear();
      const auto picture = PictureBounds(), crop = CropBounds(picture);
      dc.DrawBitmap(wxBitmap{mPicture.Scale(picture.width, picture.height, wxIMAGE_QUALITY_HIGH)}, picture.x, picture.y);
      if (mOverlay) {
         const auto scale = std::min(double(crop.width) / mOverlay->GetWidth(), double(crop.height) / mOverlay->GetHeight());
         const auto width = std::max(1, int(std::round(scale * mOverlay->GetWidth())));
         const auto height = std::max(1, int(std::round(scale * mOverlay->GetHeight())));
         dc.DrawBitmap(wxBitmap{mOverlay->Scale(width, height, wxIMAGE_QUALITY_HIGH)},
            crop.x + (crop.width - width) / 2, crop.y + (crop.height - height) / 2, true);
      }
      else {
         dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush{wxColour{15, 15, 15}, wxBRUSHSTYLE_CROSSDIAG_HATCH});
         dc.DrawRectangle(picture.x, picture.y, picture.width, crop.y - picture.y);
         dc.DrawRectangle(picture.x, crop.y + crop.height, picture.width, picture.GetBottom() + 1 - crop.y - crop.height);
         dc.DrawRectangle(picture.x, crop.y, crop.x - picture.x, crop.height);
         dc.DrawRectangle(crop.x + crop.width, crop.y, picture.GetRight() + 1 - crop.x - crop.width, crop.height);
      }
      dc.SetBrush(*wxTRANSPARENT_BRUSH); dc.SetPen(wxPen{*wxWHITE, 2}); dc.DrawRectangle(crop);
      dc.SetBrush(*wxWHITE_BRUSH);
      for (const int x : {crop.x, crop.x + crop.width / 2, crop.x + crop.width})
         for (const int y : {crop.y, crop.y + crop.height / 2, crop.y + crop.height})
            if (x != crop.x + crop.width / 2 || y != crop.y + crop.height / 2)
               dc.DrawRectangle(x - 4, y - 4, 8, 8);
   }
   void Finish(bool restore) {
      if (!mHandle) return;
      mHandle = 0;
      if (restore) { mCrop = mInitial; mChanged(mCrop); Refresh(); }
      if (HasCapture()) ReleaseMouse();
   }
public:
   ~CropPicture() override { mHandle = 0; if (HasCapture()) ReleaseMouse(); }
   CropPicture(wxWindow *parent, wxImage picture, VideoEditing::VideoFrameCrop crop,
      std::function<void(const VideoEditing::VideoFrameCrop&)> changed, std::optional<wxImage> overlay = {})
      : wxPanel(parent), mPicture(std::move(picture)), mOverlay(std::move(overlay)), mCrop(crop), mChanged(std::move(changed))
   {
      SetName(_("Picture crop handles")); SetLabel(_("Picture crop handles")); SetMinSize({560, 315}); SetBackgroundStyle(wxBG_STYLE_PAINT);
      SetToolTip(_("Drag an edge or corner to crop. Drag inside the rectangle to move it. Percentages below give exact control."));
      Bind(wxEVT_PAINT, &CropPicture::Paint, this);
      Bind(wxEVT_SIZE, [this](wxSizeEvent &event) { Refresh(); event.Skip(); });
      Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
         mHandle = Hit(event.GetPosition());
         if (mHandle) { mInitial = mCrop; mDragStart = event.GetPosition(); SetFocus(); CaptureMouse(); }
      });
      Bind(wxEVT_MOTION, [this](wxMouseEvent &event) {
         if (!mHandle) return;
         const auto picture = PictureBounds();
         mCrop = VideoEditing::DragVideoCropHandle(mInitial, mHandle,
            double(event.GetX() - mDragStart.x) / picture.width, double(event.GetY() - mDragStart.y) / picture.height,
            1.0 / mPicture.GetWidth(), 1.0 / mPicture.GetHeight());
         mChanged(mCrop); Refresh();
      });
      Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { Finish(false); });
      Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &) { Finish(true); });
      Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) {
         if (event.GetKeyCode() == WXK_ESCAPE && mHandle) Finish(true); else event.Skip();
      });
   }
   void SetCrop(const VideoEditing::VideoFrameCrop &value) { mCrop = value; Refresh(); }
};
}

namespace VideoEditing {
void ShowVideoPictureCrop(wxWindow *parent, const VideoTimeline::Clip &clip,
   const VideoTimeline::MediaSource &fallback, double projectTime,
   const std::function<void(const VideoFrameCrop&)> &apply)
{
   auto picture = ReadCropPicture(parent, clip, fallback, projectTime);
   if (!picture) return;
   auto crop = clip.crop;
   wxDialog dialog{parent, wxID_ANY, _("Crop Video Picture"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER};
   auto *layout = new wxBoxSizer(wxVERTICAL);
   layout->Add(new wxStaticText(&dialog, wxID_ANY,
      _("Drag the picture edges or corners. The remaining picture keeps its proportions.\nDrag inside to move the crop; use percentages for exact margins.")), 0, wxALL, 10);
   std::array<wxSpinCtrlDouble*, 4> fields{};
   std::array<double, 4> shown{};
   std::array<bool, 4> valid{true, true, true, true};
   bool updating{};
   const auto show = [&](const VideoFrameCrop &value) {
      crop = value; updating = true;
      const double values[]{crop.left, crop.top, crop.right, crop.bottom};
      for (size_t i{}; i < fields.size(); ++i) if (fields[i]) {
         fields[i]->SetValue(values[i] * 100); shown[i] = fields[i]->GetValue(); valid[i] = true;
      }
      updating = false;
   };
   auto *canvas = new CropPicture(&dialog, std::move(*picture), crop, show);
   layout->Add(canvas, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
   auto *grid = new wxFlexGridSizer(2, 8, 12);
   const wxString names[]{_("Left"), _("Top"), _("Right"), _("Bottom")};
   const double values[]{crop.left, crop.top, crop.right, crop.bottom};
   for (size_t i{}; i < fields.size(); ++i) {
      grid->Add(new wxStaticText(&dialog, wxID_ANY, names[i]), 0, wxALIGN_CENTER_VERTICAL);
      fields[i] = new wxSpinCtrlDouble(&dialog, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 99.99, values[i] * 100, .1);
      fields[i]->SetDigits(2); fields[i]->SetName(names[i]); fields[i]->SetValue(values[i] * 100);
      shown[i] = fields[i]->GetValue();
      fields[i]->Bind(wxEVT_TEXT, [&, i](wxCommandEvent &event) {
         if (updating) return;
         double value{}; valid[i] = event.GetString().ToDouble(&value) && std::isfinite(value) && value >= 0 && value < 100;
         if (!valid[i]) return;
         auto preview = crop;
         double *margins[]{&preview.left, &preview.top, &preview.right, &preview.bottom};
         for (size_t n{}; n < fields.size(); ++n) if (fields[n] && fields[n]->GetValue() != shown[n])
            *margins[n] = fields[n]->GetValue() / 100;
         *margins[i] = value / 100;
         try { RequireUsableVideoFrameCrop(preview); canvas->SetCrop(preview); } catch (const std::runtime_error &) { /* Keep the last drawable draft until Apply reports the invalid margins. */ }
      });
      grid->Add(fields[i], 0, wxEXPAND);
   }
   layout->Add(grid, 0, wxALL, 10);
   auto *reset = new wxButton(&dialog, wxID_ANY, _("Reset Crop"));
   reset->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { show({}); canvas->SetCrop(crop); });
   layout->Add(reset, 0, wxLEFT | wxRIGHT, 10);
   layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
   dialog.SetSizerAndFit(layout);
   dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
      try {
         if (std::any_of(valid.begin(), valid.end(), [](bool good) { return !good; }))
            throw std::runtime_error("Enter finite picture margins from 0 to less than 100 percent.");
         auto selected = crop;
         double *margins[]{&selected.left, &selected.top, &selected.right, &selected.bottom};
         // Commit from current controls, even when a native text notification
         // has not arrived. Untouched fields keep exact saved/dragged values.
         for (size_t i{}; i < fields.size(); ++i)
            if (fields[i]->GetValue() != shown[i]) *margins[i] = fields[i]->GetValue() / 100;
         RequireUsableVideoFrameCrop(selected);
         // Closing a native modal window can dispatch pending events during
         // teardown. Publish the edit first so no observer sees a closed Apply
         // dialog whose project still contains the previous picture settings.
         apply(selected); dialog.EndModal(wxID_OK);
      }
      catch (const std::exception &error) { wxMessageBox(wxString::FromUTF8(error.what()), _("Picture Crop"), wxOK | wxICON_ERROR, &dialog); }
   }, wxID_OK);
   dialog.ShowModal();
}
}


namespace VideoEditing {
void ShowVideoPicturePlacement(wxWindow *parent,
   const std::optional<VideoPlacementPicture> &child,
   const std::optional<VideoPlacementPicture> &background, double at,
   const VideoFrameRectangle &initial, const std::function<void(const VideoFrameRectangle&)> &apply)
{
   const auto read = [&](const std::optional<VideoPlacementPicture> &input) -> std::optional<wxImage> {
      if (input) return ReadCropPicture(parent, input->clip, input->source, at, false);
      wxImage blank(640, 360); blank.SetRGB(wxRect{0, 0, 640, 360}, 0, 0, 0); return blank;
   };
   auto base = read(background); if (!base) return;
   auto overlay = read(child); if (!overlay) return;
   wxDialog dialog{parent, wxID_ANY, _("Picture-in-Picture Placement"), wxDefaultPosition, wxDefaultSize,
      wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER};
   auto *layout = new wxBoxSizer(wxVERTICAL);
   layout->Add(new wxStaticText(&dialog, wxID_ANY,
      _("Drag inside the small picture to move it. Drag its edges or corners to resize.\n"
        "This placement applies to every clip on the embedded video track.")), 0, wxALL, 10);
   auto rectangle = initial;
   std::array<wxSpinCtrlDouble*, 4> fields{};
   std::array<double, 4> shown{}, entered{};
   std::array<bool, 4> valid{true, true, true, true};
   bool updating{};
   const auto show = [&](const VideoFrameCrop &value) {
      rectangle = {value.left, value.top, 1 - value.left - value.right, 1 - value.top - value.bottom};
      updating = true;
      const double values[]{rectangle.x, rectangle.y, rectangle.width, rectangle.height};
      for (size_t i{}; i < fields.size(); ++i) if (fields[i]) {
         fields[i]->SetValue(values[i] * 100); shown[i] = entered[i] = fields[i]->GetValue(); valid[i] = true;
      }
      updating = false;
   };
   const auto asCrop = [](const VideoFrameRectangle &r) { return VideoFrameCrop{r.x, r.y, std::max(0.0, 1-r.x-r.width), std::max(0.0, 1-r.y-r.height)}; };
   auto *canvas = new CropPicture(&dialog, std::move(*base), asCrop(initial), show, std::move(overlay));
   canvas->SetName(_("Picture-in-picture handles")); canvas->SetLabel(_("Picture-in-picture handles"));
   canvas->SetToolTip(_("Drag the picture or its handles. Escape cancels the current drag; Cancel leaves the project unchanged."));
   layout->Add(canvas, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);
   auto *grid = new wxFlexGridSizer(2, 8, 12);
   const wxString names[]{_("Horizontal position (%)"), _("Vertical position (%)"), _("Width (%)"), _("Height (%)")};
   const double values[]{initial.x, initial.y, initial.width, initial.height};
   const auto selected = [&]() {
      if (std::any_of(valid.begin(), valid.end(), [](bool good) { return !good; }))
         throw std::runtime_error("Enter finite picture positions and positive dimensions in percent.");
      auto r = rectangle;
      double *values[]{&r.x, &r.y, &r.width, &r.height};
      for (size_t i{}; i < fields.size(); ++i)
         if (entered[i] != shown[i]) *values[i] = entered[i] / 100;
      RequireUsableVideoPlacement({{}, {}, r}); return r;
   };
   for (size_t i{}; i < fields.size(); ++i) {
      grid->Add(new wxStaticText(&dialog, wxID_ANY, names[i]), 0, wxALIGN_CENTER_VERTICAL);
      fields[i] = new wxSpinCtrlDouble(&dialog, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
         wxSP_ARROW_KEYS, i < 2 ? 0 : .01, 100, values[i]*100, .1);
      fields[i]->SetDigits(2); fields[i]->SetName(names[i]); shown[i] = entered[i] = fields[i]->GetValue();
      fields[i]->Bind(wxEVT_TEXT, [&, i](wxCommandEvent &event) {
         if (updating || !fields.back()) return;
         double value{}; valid[i] = event.GetString().ToDouble(&value) && std::isfinite(value) &&
            value >= (i < 2 ? 0 : .01) && value <= 100;
         if (!valid[i]) return;
         // SpinCtrlDouble can defer its numeric value until focus changes. Text
         // events own the draft so Apply also sees the last field still focused.
         entered[i] = value;
         try {
            auto r = selected();
            RequireUsableVideoPlacement({{}, {}, r}); canvas->SetCrop(asCrop(r));
         }
         catch (const std::exception &) { /* Invalid intermediate text cannot move the draft picture. */ }
      });
      grid->Add(fields[i], 0, wxEXPAND);
   }
   layout->Add(grid, 0, wxALL, 10);
   layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
   dialog.SetSizerAndFit(layout);
   dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
      try { rectangle = selected(); apply(rectangle); dialog.EndModal(wxID_OK); }
      catch (const std::exception &error) {
         wxMessageBox(wxString::FromUTF8(error.what()), _("Picture-in-Picture"), wxOK | wxICON_ERROR, &dialog);
      }
   }, wxID_OK);
   dialog.ShowModal();
}
}
