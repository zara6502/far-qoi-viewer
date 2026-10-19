#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

using string = std::wstring;

#include "plugin.hpp"
#include "qoi.h"

static PluginStartupInfo g_far{};
static FarStandardFunctions g_fsf{};

static const GUID PluginGuid = {
    0x6a6d1e35,
    0x7c7f,
    0x4b7b,
    {0x9f, 0x4b, 0x2e, 0x0d, 0x1d, 0x6f, 0x52, 0x91}};

static const GUID MenuGuid = {0x2b2c3d44,
                              0x5e6f,
                              0x47a1,
                              {0x91, 0x72, 0x3a, 0x8c, 0x4d, 0x51, 0x9e, 0x20}};

static const wchar_t *const kClassName = L"FarQoiViewerWindow";
enum class BackgroundMode { Checkers, Black, White };
static constexpr UINT WM_APP_IMAGE_LOADED = WM_APP + 17;

struct LoadContext {
  std::mutex mutex;

  bool done = false;
  bool success = false;

  QoiImage image;
  std::vector<uint8_t> bgra;
  QoiDecodeError error{};

  HBITMAP dib = nullptr;
  HDC dibDC = nullptr;
  HGDIOBJ previousDibObject = nullptr;

  ~LoadContext() {
    if (dibDC) {
      if (previousDibObject && previousDibObject != HGDI_ERROR) {
        SelectObject(dibDC, previousDibObject);
      }

      DeleteDC(dibDC);
      dibDC = nullptr;
    }

    if (dib) {
      DeleteObject(dib);
      dib = nullptr;
    }

    previousDibObject = nullptr;
  }
};

struct LoadJob {
  std::shared_ptr<LoadContext> context;
  std::wstring filename;
  HWND hwnd = nullptr;
};
struct ViewerState {
  bool loading = true;
  bool loadFailed = false;
  std::wstring loadError;
  std::shared_ptr<LoadContext> loadContext;
  QoiImage image;
  std::wstring filename;

  HWND hwnd = nullptr;
  HANDLE loadThread = nullptr;

  double zoom = 1.0;
  double panX = 0.0;
  double panY = 0.0;

  bool fitToWindow = true;
  BackgroundMode background = BackgroundMode::Checkers;

  /*
     QOI RGBA -> Windows BGRA.
     Prepared once after loading.
  */
  std::vector<uint8_t> bgra;
  HBITMAP dib = nullptr;
  HDC dibDC = nullptr;
  HGDIOBJ previousDibObject = nullptr;
};
static std::wstring format_qoi_error(const QoiDecodeError &error);
static ViewerState *g_state = nullptr;
static HBRUSH g_checkerBrush = nullptr;

// Защищает состояние окна, поток и флаг завершения.
static SRWLOCK g_stateLock = SRWLOCK_INIT;
static HANDLE g_viewerThread = nullptr;
static bool g_shuttingDown = false;

static HBRUSH get_checker_brush() {
  if (g_checkerBrush)
    return g_checkerBrush;

  constexpr int cs = 12;
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = cs * 2;
  bmi.bmiHeader.biHeight = -(cs * 2);
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *bits = nullptr;
  HBITMAP dib =
      CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits)
    return nullptr;

  auto *px = static_cast<uint32_t *>(bits);
  for (int y = 0; y < cs * 2; ++y) {
    for (int x = 0; x < cs * 2; ++x) {
      const bool dark = ((x / cs) + (y / cs)) & 1;
      px[y * (cs * 2) + x] = dark ? 0x00DCDCDC : 0x00F5F5F5;
    }
  }

  g_checkerBrush = CreatePatternBrush(dib);
  DeleteObject(dib);
  return g_checkerBrush;
}

static void release_gdi_resources(ViewerState &state) {
  // Сначала восстанавливаем предыдущий объект DC.
  if (state.dibDC) {
    if (state.previousDibObject && state.previousDibObject != HGDI_ERROR) {
      SelectObject(state.dibDC, state.previousDibObject);
    }

    DeleteDC(state.dibDC);
    state.dibDC = nullptr;
  }

  // DIB удаляется после того, как он перестал
  // быть выбранным в DC.
  if (state.dib) {
    DeleteObject(state.dib);
    state.dib = nullptr;
  }

  state.previousDibObject = nullptr;
}

// Вызывать только при захваченном g_stateLock.
static void reap_finished_viewer_locked() {
  if (g_viewerThread &&
      WaitForSingleObject(g_viewerThread, 0) == WAIT_OBJECT_0) {
    CloseHandle(g_viewerThread);
    g_viewerThread = nullptr;
    g_state = nullptr;
  }
}

static bool focus_existing_viewer_if_busy() {
  HWND hwnd = nullptr;
  bool busy = false;

  AcquireSRWLockExclusive(&g_stateLock);

  if (g_shuttingDown) {
    busy = true;
  } else {
    reap_finished_viewer_locked();

    if (g_viewerThread) {
      busy = true;

      if (g_state)
        hwnd = g_state->hwnd;
    }
  }

  ReleaseSRWLockExclusive(&g_stateLock);

  if (hwnd)
    SetForegroundWindow(hwnd);

  return busy;
}

static bool has_qoi_extension(const wchar_t *name) {
  if (!name)
    return false;

  const wchar_t *dot = wcsrchr(name, L'.');

  if (!dot)
    return false;

  return _wcsicmp(dot, L".qoi") == 0;
}

static bool load_file(const wchar_t *name, QoiImage &image,
                      QoiDecodeError &error) {
  error = {};

  HANDLE h =
      CreateFileW(name, GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

  if (h == INVALID_HANDLE_VALUE) {
    error = {QoiErrorStage::File, QoiErrorCode::FileOpenFailed, 0, 0};
    return false;
  }

  LARGE_INTEGER li{};

  if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0) {
    CloseHandle(h);
    error = {QoiErrorStage::File, QoiErrorCode::FileSizeFailed, 0, 0};
    return false;
  }

  if (static_cast<unsigned long long>(li.QuadPart) > SIZE_MAX) {
    CloseHandle(h);
    error = {QoiErrorStage::File, QoiErrorCode::FileTooLarge, 0, 0};
    return false;
  }

  const size_t size = static_cast<size_t>(li.QuadPart);
  std::vector<uint8_t> data;

  try {
    data.resize(size);
  } catch (...) {
    CloseHandle(h);
    error = {QoiErrorStage::Memory, QoiErrorCode::AllocationFailed, 0, 0};
    return false;
  }

  size_t done = 0;

  while (done < size) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(size - done, 1u << 20));

    DWORD got = 0;

    if (!ReadFile(h, data.data() + done, chunk, &got, nullptr) || got == 0) {
      CloseHandle(h);
      error = {QoiErrorStage::File, QoiErrorCode::FileReadFailed, done, 0};
      return false;
    }

    done += got;
  }

  CloseHandle(h);

  return qoi_decode(data.data(), data.size(), image, &error);
}

static bool prepare_bgra(ViewerState &s) {
  const uint32_t width = s.image.width;
  const uint32_t height = s.image.height;

  if (width == 0 || height == 0 || s.image.rgba.empty())
    return false;

  const uint64_t pixelCount = uint64_t(width) * uint64_t(height);

  if (pixelCount > std::numeric_limits<size_t>::max() / 4) {
    return false;
  }

  const size_t expectedBytes = static_cast<size_t>(pixelCount) * 4;

  if (s.image.rgba.size() != expectedBytes)
    return false;

  if (width > static_cast<uint32_t>(std::numeric_limits<LONG>::max()) ||
      height > static_cast<uint32_t>(std::numeric_limits<LONG>::max())) {
    return false;
  }

  release_gdi_resources(s);

  try {
    s.bgra.resize(expectedBytes);
  } catch (...) {
    return false;
  }

  // QOI RGBA -> premultiplied BGRA для AlphaBlend.
  for (size_t i = 0; i < expectedBytes; i += 4) {
    const uint8_t r = s.image.rgba[i + 0];
    const uint8_t g = s.image.rgba[i + 1];
    const uint8_t b = s.image.rgba[i + 2];
    const uint8_t a = s.image.rgba[i + 3];

    s.bgra[i + 0] = static_cast<uint8_t>((uint32_t(b) * a) / 255);

    s.bgra[i + 1] = static_cast<uint8_t>((uint32_t(g) * a) / 255);

    s.bgra[i + 2] = static_cast<uint8_t>((uint32_t(r) * a) / 255);

    s.bgra[i + 3] = a;
  }

  BITMAPINFO bmi{};

  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = static_cast<LONG>(width);
  bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  s.dibDC = CreateCompatibleDC(nullptr);

  if (!s.dibDC) {
    release_gdi_resources(s);
    return false;
  }

  void *bits = nullptr;

  s.dib = CreateDIBSection(s.dibDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

  if (!s.dib || !bits) {
    release_gdi_resources(s);
    return false;
  }

  HGDIOBJ previous = SelectObject(s.dibDC, s.dib);

  if (!previous || previous == HGDI_ERROR) {
    release_gdi_resources(s);
    return false;
  }

  s.previousDibObject = previous;

  std::memcpy(bits, s.bgra.data(), expectedBytes);

  return true;
}

static void publish_load_failure(ViewerState &state,
                                 const QoiDecodeError &error) {
  const auto context = state.loadContext;
  if (!context)
    return;

  {
    std::lock_guard<std::mutex> lock(context->mutex);
    context->success = false;
    context->error = error;
    context->done = true;
  }

  if (state.hwnd)
    PostMessageW(state.hwnd, WM_APP_IMAGE_LOADED, 0, 0);
}

static DWORD WINAPI image_load_thread(LPVOID param) {
  std::unique_ptr<LoadJob> job(static_cast<LoadJob *>(param));

  if (!job || !job->context)
    return 1;

  ViewerState temporary;
  QoiDecodeError error{};
  bool success = false;

  try {
    success = load_file(job->filename.c_str(), temporary.image, error);

    if (success) {
      success = prepare_bgra(temporary);

      if (!success) {
        error = {QoiErrorStage::Memory, QoiErrorCode::AllocationFailed, 0, 0};
      }
    }
  } catch (...) {
    success = false;

    error = {QoiErrorStage::Memory, QoiErrorCode::UnknownDecodeFailure, 0, 0};
  }

  if (!success && error.code == QoiErrorCode::None) {
    error = {QoiErrorStage::StandardDecode, QoiErrorCode::UnknownDecodeFailure,
             0, 0};
  }

  {
    std::lock_guard<std::mutex> lock(job->context->mutex);

    job->context->success = success;
    job->context->error = error;

    if (success) {
      job->context->image = std::move(temporary.image);

      job->context->bgra = std::move(temporary.bgra);

      // Передаём также GDI-объекты,
      // созданные внутри prepare_bgra().
      job->context->dib = temporary.dib;
      job->context->dibDC = temporary.dibDC;
      job->context->previousDibObject = temporary.previousDibObject;

      // Теперь этими объектами владеет LoadContext.
      temporary.dib = nullptr;
      temporary.dibDC = nullptr;
      temporary.previousDibObject = nullptr;
    }

    job->context->done = true;
  }

  // При ошибке очистит частично созданные GDI-ресурсы.
  // При успехе ресурсы уже переданы в LoadContext.
  release_gdi_resources(temporary);

  PostMessageW(job->hwnd, WM_APP_IMAGE_LOADED, 0, 0);

  return 0;
}

static void start_image_load(ViewerState &state) {
  LoadJob *job = nullptr;

  try {
    job = new LoadJob{state.loadContext, state.filename, state.hwnd};
  } catch (...) {
    publish_load_failure(state,
                         QoiDecodeError{QoiErrorStage::Memory,
                                        QoiErrorCode::AllocationFailed, 0, 0});
    return;
  }

  HANDLE thread = CreateThread(nullptr, 0, image_load_thread, job, 0, nullptr);

  if (!thread) {
    delete job;

    publish_load_failure(
        state, QoiDecodeError{QoiErrorStage::Memory,
                              QoiErrorCode::UnknownDecodeFailure, 0, 0});
    return;
  }

  state.loadThread = thread;
}

static void checker(HDC dc, const RECT &r) {
  HBRUSH brush = get_checker_brush();
  if (!brush) {
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
    return;
  }
  FillRect(dc, &r, brush);
}

static void draw_background(HDC dc, const RECT &r, BackgroundMode mode) {
  switch (mode) {
  case BackgroundMode::Black:
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    break;
  case BackgroundMode::White:
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    break;
  case BackgroundMode::Checkers:
  default:
    checker(dc, r);
    break;
  }
}

/*
   Calculate the exact rectangle occupied by the image.
*/
static bool get_image_rect(HWND hwnd, const ViewerState &s, RECT &out) {
  RECT rc{};

  GetClientRect(hwnd, &rc);

  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

  if (cw <= 0 || ch <= 0 || s.image.width == 0 || s.image.height == 0) {
    SetRectEmpty(&out);
    return false;
  }

  const double fit =
      std::min(static_cast<double>(cw) / static_cast<double>(s.image.width),
               static_cast<double>(ch) / static_cast<double>(s.image.height));

  if (fit <= 0.0) {
    SetRectEmpty(&out);
    return false;
  }

  const double scale = s.fitToWindow ? fit : s.zoom;

  const int dw = std::max(1, static_cast<int>(std::lround(
                                 static_cast<double>(s.image.width) * scale)));

  const int dh = std::max(1, static_cast<int>(std::lround(
                                 static_cast<double>(s.image.height) * scale)));

  const int x = static_cast<int>(std::lround((cw - dw) * 0.5 + s.panX));

  const int y = static_cast<int>(std::lround((ch - dh) * 0.5 + s.panY));

  out.left = x;
  out.top = y;
  out.right = x + dw;
  out.bottom = y + dh;

  return true;
}

static double get_fit_scale(HWND hwnd, const ViewerState &s) {
  RECT rc{};

  if (!GetClientRect(hwnd, &rc))
    return s.zoom > 0.0 ? s.zoom : 1.0;

  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

  if (cw <= 0 || ch <= 0 || s.image.width == 0 || s.image.height == 0) {
    return s.zoom > 0.0 ? s.zoom : 1.0;
  }

  return std::min(static_cast<double>(cw) / s.image.width,
                  static_cast<double>(ch) / s.image.height);
}

static double get_effective_scale(HWND hwnd, const ViewerState &s) {
  const double scale = s.fitToWindow ? get_fit_scale(hwnd, s) : s.zoom;

  return scale > 0.0 ? scale : 1.0;
}

/*
   Invalidate the complete area that can have changed.

   IMPORTANT:

   RGN_XOR is NOT correct for scaling because the
   overlapping area contains different pixels after
   resampling.

   We therefore invalidate the UNION:

       old image + new image

   No background erase is requested. The old image
   remains visible until the new image is actually
   painted over it.
*/
static void invalidate_image_change(HWND hwnd, const RECT &oldRect,
                                    const RECT &newRect) {
  if (EqualRect(&oldRect, &newRect))
    return;

  RECT changed{};

  if (IsRectEmpty(&oldRect)) {
    changed = newRect;
  } else if (IsRectEmpty(&newRect)) {
    changed = oldRect;
  } else {
    changed.left = std::min(oldRect.left, newRect.left);

    changed.top = std::min(oldRect.top, newRect.top);

    changed.right = std::max(oldRect.right, newRect.right);

    changed.bottom = std::max(oldRect.bottom, newRect.bottom);
  }

  InvalidateRect(hwnd, &changed, FALSE);
}

/*
   Draw only the current paint region.

   BeginPaint clips the DC to ps.rcPaint, so there is
   no need to redraw the whole client area.
*/
static void draw_image(HWND hwnd, HDC dc, ViewerState &s,
                       const RECT &paintRect) {
  if (s.loading || s.loadFailed) {
    draw_background(dc, paintRect, s.background);

    RECT textRect{};
    GetClientRect(hwnd, &textRect);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(40, 40, 40));

    const wchar_t *text =
        s.loading ? L"Loading QOI image..." : s.loadError.c_str();

    DrawTextW(dc, text, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    return;
  }

  RECT imageRect{};
  if (!get_image_rect(hwnd, s, imageRect)) {
    draw_background(dc, paintRect, s.background);
    return;
  }

  if (!s.dibDC || !s.dib) {
    draw_background(dc, paintRect, s.background);
    return;
  }

  const int dw = imageRect.right - imageRect.left;
  const int dh = imageRect.bottom - imageRect.top;

  BLENDFUNCTION bf{};
  bf.BlendOp = AC_SRC_OVER;
  bf.BlendFlags = 0;
  bf.SourceConstantAlpha = 255;
  bf.AlphaFormat = AC_SRC_ALPHA;

  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);

  // Keep the original main-branch path for opaque RGB QOI images.
  if (s.image.channels != 4) {
    HRGN paintRegion = CreateRectRgnIndirect(&paintRect);
    HRGN imageRegion = CreateRectRgnIndirect(&imageRect);
    HRGN backgroundRegion = CreateRectRgn(0, 0, 0, 0);
    HRGN imagePaintRegion = CreateRectRgn(0, 0, 0, 0);

    if (!paintRegion || !imageRegion || !backgroundRegion ||
        !imagePaintRegion) {
      if (paintRegion) DeleteObject(paintRegion);
      if (imageRegion) DeleteObject(imageRegion);
      if (backgroundRegion) DeleteObject(backgroundRegion);
      if (imagePaintRegion) DeleteObject(imagePaintRegion);
      return;
    }

    CombineRgn(backgroundRegion, paintRegion, imageRegion, RGN_DIFF);
    CombineRgn(imagePaintRegion, paintRegion, imageRegion, RGN_AND);

    SelectClipRgn(dc, backgroundRegion);
    draw_background(dc, paintRect, s.background);
    SelectClipRgn(dc, imagePaintRegion);

    AlphaBlend(dc, imageRect.left, imageRect.top, dw, dh, s.dibDC, 0, 0,
               static_cast<int>(s.image.width),
               static_cast<int>(s.image.height), bf);

    SelectClipRgn(dc, nullptr);
    DeleteObject(paintRegion);
    DeleteObject(imageRegion);
    DeleteObject(backgroundRegion);
    DeleteObject(imagePaintRegion);
    return;
  }

  // RGBA path: compose into an off-screen surface. The checkerboard and
  // alpha blend are completed in memory, then copied to the window once.
  // This prevents transparent pixels from blending with stale window pixels
  // and avoids a separate erase/clear pass that could visibly flicker.
  const int paintWidth = paintRect.right - paintRect.left;
  const int paintHeight = paintRect.bottom - paintRect.top;
  if (paintWidth <= 0 || paintHeight <= 0)
    return;

  HDC memoryDC = CreateCompatibleDC(dc);
  if (!memoryDC)
    return;

  HBITMAP memoryBitmap = CreateCompatibleBitmap(dc, paintWidth, paintHeight);
  if (!memoryBitmap) {
    DeleteDC(memoryDC);
    return;
  }

  HGDIOBJ oldBitmap = SelectObject(memoryDC, memoryBitmap);
  if (!oldBitmap || oldBitmap == HGDI_ERROR) {
    DeleteObject(memoryBitmap);
    DeleteDC(memoryDC);
    return;
  }

  // Match the checker pattern's origin to the window's client coordinates.
  SetBrushOrgEx(memoryDC, -paintRect.left, -paintRect.top, nullptr);
  RECT memoryRect{0, 0, paintWidth, paintHeight};
  draw_background(memoryDC, memoryRect, s.background);

  SetStretchBltMode(memoryDC, HALFTONE);
  SetBrushOrgEx(memoryDC, 0, 0, nullptr);

  AlphaBlend(memoryDC, imageRect.left - paintRect.left,
              imageRect.top - paintRect.top, dw, dh, s.dibDC, 0, 0,
              static_cast<int>(s.image.width),
              static_cast<int>(s.image.height), bf);

  // BeginPaint clips the destination DC to the invalidated paint region.
  BitBlt(dc, paintRect.left, paintRect.top, paintWidth, paintHeight,
         memoryDC, 0, 0, SRCCOPY);

  SelectObject(memoryDC, oldBitmap);
  DeleteObject(memoryBitmap);
  DeleteDC(memoryDC);
}

static void update_title(HWND hwnd, const ViewerState &s) {

  if (s.loading) {
    const std::wstring title = s.filename + L" - Loading...";
    SetWindowTextW(hwnd, title.c_str());
    return;
  }

  if (s.loadFailed) {
    const std::wstring title = s.filename + L" - Load failed";
    SetWindowTextW(hwnd, title.c_str());
    return;
  }

  wchar_t mode[64]{};

  if (s.fitToWindow) {
    wcscpy_s(mode, L"Fit");
  } else {
    const int pct = static_cast<int>(std::lround(s.zoom * 100.0));
    swprintf_s(mode, L"%d%%", pct);
  }

  wchar_t title[512]{};

  swprintf_s(title,
             L"%s - %ux%u  %s  |  %s  |  "
             L"1 100%%  0 fit  +/- zoom  Space/LMB switch  Esc close",
             s.filename.c_str(), s.image.width, s.image.height,
             s.image.channels == 4 ? L"RGBA" : L"RGB", mode);

  SetWindowTextW(hwnd, title);
}

static void change_zoom(HWND hwnd, ViewerState &s, double newZoom) {
  RECT oldRect{};
  get_image_rect(hwnd, s, oldRect);

  s.fitToWindow = false;
  s.zoom = std::clamp(newZoom, 0.01, 32.0);

  RECT newRect{};
  get_image_rect(hwnd, s, newRect);

  invalidate_image_change(hwnd, oldRect, newRect);

  update_title(hwnd, s);
}

static void set_fit_mode(HWND hwnd, ViewerState &s) {
  RECT oldRect{};
  get_image_rect(hwnd, s, oldRect);

  s.fitToWindow = true;
  s.panX = 0;
  s.panY = 0;

  RECT newRect{};
  get_image_rect(hwnd, s, newRect);

  invalidate_image_change(hwnd, oldRect, newRect);

  update_title(hwnd, s);
}

static void set_100_percent(HWND hwnd, ViewerState &s) {
  RECT oldRect{};
  get_image_rect(hwnd, s, oldRect);

  s.fitToWindow = false;
  s.zoom = 1.0;
  s.panX = 0;
  s.panY = 0;

  RECT newRect{};
  get_image_rect(hwnd, s, newRect);

  invalidate_image_change(hwnd, oldRect, newRect);

  update_title(hwnd, s);
}

static void toggle_view_mode(HWND hwnd, ViewerState &s) {
  RECT oldRect{};
  get_image_rect(hwnd, s, oldRect);

  s.fitToWindow = !s.fitToWindow;

  s.panX = 0;
  s.panY = 0;

  RECT newRect{};
  get_image_rect(hwnd, s, newRect);

  invalidate_image_change(hwnd, oldRect, newRect);

  update_title(hwnd, s);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  ViewerState *s =
      reinterpret_cast<ViewerState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (msg) {
  case WM_CREATE: {
    auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);

    s = reinterpret_cast<ViewerState *>(cs->lpCreateParams);

    if (!s)
      return -1;

    AcquireSRWLockExclusive(&g_stateLock);

    // FAR уже завершает работу: не создаём новое окно.
    if (g_shuttingDown) {
      ReleaseSRWLockExclusive(&g_stateLock);
      return -1;
    }

    s->hwnd = hwnd;
    g_state = s;

    ReleaseSRWLockExclusive(&g_stateLock);

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

    update_title(hwnd, *s);

    return 0;
  }

  case WM_APP_IMAGE_LOADED: {
    if (!s || !s->loadContext)
      return 0;

    const auto context = s->loadContext;

    {
      std::lock_guard<std::mutex> lock(context->mutex);

      if (!context->done)
        return 0;

      s->loading = false;
      s->loadFailed = !context->success;

      if (context->success) {
        s->image = std::move(context->image);
        s->bgra = std::move(context->bgra);

        // Принимаем GDI-объекты от рабочего потока.
        s->dib = context->dib;
        s->dibDC = context->dibDC;
        s->previousDibObject = context->previousDibObject;

        // Теперь ресурсами владеет ViewerState.
        context->dib = nullptr;
        context->dibDC = nullptr;
        context->previousDibObject = nullptr;

        s->loadError.clear();
      } else {
        s->loadError = format_qoi_error(context->error);
      }
    }

    update_title(hwnd, *s);
    InvalidateRect(hwnd, nullptr, FALSE);

    return 0;
  }

  case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    if (s) {
      draw_image(hwnd, dc, *s, ps.rcPaint);
    } else {
      FillRect(dc, &ps.rcPaint,
               static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }

    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_ERASEBKGND:
    /*
       Never erase the background.
       This is what prevents the visible
       checkerboard -> image flicker.
    */
    return 1;

  case WM_SIZE:
    /*
       Resize changes the fit geometry.
       Let Windows repaint the affected area,
       but never erase it first.
    */
    InvalidateRect(hwnd, nullptr, FALSE);

    return 0;

  case WM_MOUSEWHEEL: {
    if (!s)
      return 0;

    const int delta = GET_WHEEL_DELTA_WPARAM(wp);

    if (delta == 0)
      return 0;

    const double currentScale = get_effective_scale(hwnd, *s);

    const double steps = static_cast<double>(delta) / WHEEL_DELTA;

    const double factor = std::pow(1.20, steps);

    change_zoom(hwnd, *s, currentScale * factor);

    return 0;
  }

  case WM_LBUTTONDOWN: {
    if (!s)
      return 0;

    /*
       No mouse dragging.
       Left click toggles Fit / explicit zoom.
    */
    toggle_view_mode(hwnd, *s);

    return 0;
  }

  case WM_KEYDOWN: {
    if (!s)
      break;

    switch (wp) {
    case VK_B:
      s->background = BackgroundMode::Black;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case VK_W:
      s->background = BackgroundMode::White;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case VK_C:
      s->background = BackgroundMode::Checkers;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case VK_ESCAPE:
      DestroyWindow(hwnd);
      return 0;

    case VK_SPACE:
      toggle_view_mode(hwnd, *s);
      return 0;

    case VK_HOME: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panX = 0;
      s->panY = 0;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }

    case '1':
      set_100_percent(hwnd, *s);
      return 0;

    case '0':
      set_fit_mode(hwnd, *s);
      return 0;

    case VK_ADD:
    case VK_OEM_PLUS: {
      const double currentScale = get_effective_scale(hwnd, *s);

      change_zoom(hwnd, *s, currentScale * 1.20);

      return 0;
    }

    case VK_SUBTRACT:
    case VK_OEM_MINUS: {
      const double currentScale = get_effective_scale(hwnd, *s);

      change_zoom(hwnd, *s, currentScale / 1.20);

      return 0;
    }

    case VK_LEFT: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panX += 32;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }

    case VK_RIGHT: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panX -= 32;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }

    case VK_UP: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panY += 32;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }

    case VK_DOWN: {
      RECT oldRect{};
      get_image_rect(hwnd, *s, oldRect);

      s->panY -= 32;

      RECT newRect{};
      get_image_rect(hwnd, *s, newRect);

      invalidate_image_change(hwnd, oldRect, newRect);

      return 0;
    }
    }

    break;
  }

  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY: {
    if (s) {
      AcquireSRWLockExclusive(&g_stateLock);

      if (s->hwnd == hwnd)
        s->hwnd = nullptr;

      ReleaseSRWLockExclusive(&g_stateLock);
    }

    PostQuitMessage(0);
    return 0;
  }
  }

  return DefWindowProcW(hwnd, msg, wp, lp);
}

static void finish_viewer_thread(ViewerState &state, HWND hwnd,
                                 HINSTANCE instance, bool unregisterClass) {
  // Вызывать не удерживая g_stateLock:
  // DestroyWindow отправляет WM_DESTROY.
  if (hwnd && IsWindow(hwnd))
    DestroyWindow(hwnd);
  
  // Дожидаемся фоновой загрузки до завершения потока плагина.
  if (state.loadThread) {
    WaitForSingleObject(state.loadThread, INFINITE);
    CloseHandle(state.loadThread);
    state.loadThread = nullptr;
  }

  release_gdi_resources(state);

  if (unregisterClass)
    UnregisterClassW(kClassName, instance);

  AcquireSRWLockExclusive(&g_stateLock);

  state.hwnd = nullptr;

  if (g_state == &state)
    g_state = nullptr;

  ReleaseSRWLockExclusive(&g_stateLock);
}

static DWORD WINAPI viewer_thread(LPVOID param) {
  std::unique_ptr<ViewerState> state(reinterpret_cast<ViewerState *>(param));

  const HINSTANCE instance = GetModuleHandleW(nullptr);

  AcquireSRWLockExclusive(&g_stateLock);

  if (g_shuttingDown) {
    ReleaseSRWLockExclusive(&g_stateLock);
    release_gdi_resources(*state);
    return 1;
  }

  g_state = state.get();

  ReleaseSRWLockExclusive(&g_stateLock);

  WNDCLASSW wc{};
  wc.lpfnWndProc = wndproc;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;

  bool registeredClass = false;

  WNDCLASSW existing{};

  if (GetClassInfoW(instance, kClassName, &existing)) {
    // Не используем чужой класс с тем же именем.
    if (existing.lpfnWndProc != wndproc) {
      finish_viewer_thread(*state, nullptr, instance, false);
      return 1;
    }
  } else if (RegisterClassW(&wc)) {
    registeredClass = true;
  } else {
    const DWORD error = GetLastError();

    // Допускаем уже зарегистрированный экземпляр
    // только при условии, что он принадлежит этому коду.
    if (error != ERROR_CLASS_ALREADY_EXISTS ||
        !GetClassInfoW(instance, kClassName, &existing) ||
        existing.lpfnWndProc != wndproc) {
      finish_viewer_thread(*state, nullptr, instance, false);
      return 1;
    }
  }

  RECT workArea{};

  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
    workArea.left = 0;
    workArea.top = 0;
    workArea.right = GetSystemMetrics(SM_CXSCREEN);
    workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
  }

  const int workWidth = workArea.right - workArea.left;
  const int workHeight = workArea.bottom - workArea.top;

  const int width = std::max(1, std::min(workWidth, 1200));
  const int height = std::max(1, std::min(workHeight, 900));

  const int x = workArea.left + (workWidth - width) / 2;
  const int y = workArea.top + (workHeight - height) / 2;

  HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"QOI",
                              WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr,
                              nullptr, instance, state.get());

  if (!hwnd) {
    finish_viewer_thread(*state, nullptr, instance, registeredClass);
    return 1;
  }

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  start_image_load(*state);
  // Попытка передать фокус окну просмотрщика.
  HWND foreground = GetForegroundWindow();

  const DWORD farThreadId =
      foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;

  const DWORD ourThreadId = GetCurrentThreadId();

  if (farThreadId && farThreadId != ourThreadId)
    AttachThreadInput(farThreadId, ourThreadId, TRUE);

  BringWindowToTop(hwnd);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);

  if (farThreadId && farThreadId != ourThreadId)
    AttachThreadInput(farThreadId, ourThreadId, FALSE);

  MSG msg{};
  BOOL messageResult = 0;

  for (;;) {
    messageResult = GetMessageW(&msg, nullptr, 0, 0);

    if (messageResult <= 0)
      break;

    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  finish_viewer_thread(*state, hwnd, instance, registeredClass);

  return messageResult == -1 ? 1 : 0;
}

/*
   Get the currently selected file from the active FAR panel.

   Current FAR API:
       FCTL_GETPANELINFO
       FCTL_GETCURRENTPANELITEM
       FCTL_GETPANELDIRECTORY

   FarPanelDirectory uses pointers to strings inside the
   caller-provided buffer. Param1 is the buffer size.
*/

static bool get_current_filename(std::wstring &path) {
  if (!g_far.PanelControl)
    return false;

  PanelInfo pi{};

  pi.StructSize = sizeof(pi);

  if (!g_far.PanelControl(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, &pi)) {
    return false;
  }

  if (pi.PanelType != PTYPE_FILEPANEL)
    return false;

  FarGetPluginPanelItem item{};

  item.StructSize = sizeof(item);

  const intptr_t required =
      g_far.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, &item);

  if (required <= 0)
    return false;

  std::vector<unsigned char> itemBuffer(static_cast<size_t>(required));

  item.StructSize = sizeof(item);
  item.Size = itemBuffer.size();
  item.Item = reinterpret_cast<PluginPanelItem *>(itemBuffer.data());

  if (!g_far.PanelControl(PANEL_ACTIVE, FCTL_GETCURRENTPANELITEM, 0, &item)) {
    return false;
  }

  if (!item.Item || !item.Item->FileName || !item.Item->FileName[0]) {
    return false;
  }

  const std::wstring filename = item.Item->FileName;

  /*
     First ask FAR for the required directory
     buffer size.
  */

  const intptr_t dirSize =
      g_far.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, 0, nullptr);

  if (dirSize <= 0)
    return false;

  std::vector<unsigned char> dirBuffer(static_cast<size_t>(dirSize));

  auto *dir = reinterpret_cast<FarPanelDirectory *>(dirBuffer.data());

  dir->StructSize = sizeof(FarPanelDirectory);

  if (!g_far.PanelControl(PANEL_ACTIVE, FCTL_GETPANELDIRECTORY, dirSize, dir)) {
    return false;
  }

  if (!dir->Name || !dir->Name[0])
    return false;

  path = dir->Name;

  if (!path.empty() && path.back() != L'\\') {
    path.push_back(L'\\');
  }

  path += filename;

  return true;
}

static void show_error(const wchar_t *text) {
  if (!g_far.Message)
    return;

  const wchar_t *items[] = {L"FarQoiViewer", L"", text, L"", L"OK"};

  g_far.Message(&PluginGuid, nullptr, FMSG_ERRORTYPE | FMSG_MB_OK, nullptr,
                items, 5, 1);
}

static const wchar_t *qoi_stage_text(QoiErrorStage stage) {
  switch (stage) {
  case QoiErrorStage::File:
    return L"File I/O";
  case QoiErrorStage::Header:
    return L"Header";
  case QoiErrorStage::Trailer:
    return L"Trailer";
  case QoiErrorStage::StripScan:
    return L"Strip scan";
  case QoiErrorStage::StripDecode:
    return L"Strip decode";
  case QoiErrorStage::StandardDecode:
    return L"Standard QOI decode";
  case QoiErrorStage::Memory:
    return L"Memory";
  default:
    return L"Unknown";
  }
}

static const wchar_t *qoi_code_text(QoiErrorCode code) {
  switch (code) {
  case QoiErrorCode::FileOpenFailed:
    return L"Cannot open file";
  case QoiErrorCode::FileSizeFailed:
    return L"Cannot get file size";
  case QoiErrorCode::FileReadFailed:
    return L"File read failed";
  case QoiErrorCode::FileTooLarge:
    return L"File is too large";
  case QoiErrorCode::TooShort:
    return L"File is too short";
  case QoiErrorCode::BadMagic:
    return L"Invalid QOI signature";
  case QoiErrorCode::InvalidDimensions:
    return L"Invalid image dimensions";
  case QoiErrorCode::InvalidChannels:
    return L"Invalid channel count";
  case QoiErrorCode::InvalidColorSpace:
    return L"Invalid colorspace";
  case QoiErrorCode::PixelLimitExceeded:
    return L"Pixel limit exceeded";
  case QoiErrorCode::InvalidEndMarker:
    return L"Invalid end marker/trailer";
  case QoiErrorCode::InvalidStripHeight:
    return L"Invalid strip height";
  case QoiErrorCode::UnexpectedEnd:
    return L"Unexpected end of data";
  case QoiErrorCode::TruncatedRgb:
    return L"Truncated RGB command";
  case QoiErrorCode::TruncatedRgba:
    return L"Truncated RGBA command";
  case QoiErrorCode::TruncatedLuma:
    return L"Truncated LUMA command";
  case QoiErrorCode::InvalidOpcode:
    return L"Invalid opcode";
  case QoiErrorCode::RunExceedsImage:
    return L"RUN exceeds image bounds";
  case QoiErrorCode::RunCrossesStripBoundary:
    return L"RUN crosses strip boundary";
  case QoiErrorCode::StripMissingAbsolutePixel:
    return L"Strip lacks RGB/RGBA start";
  case QoiErrorCode::ExtraEncodedBytes:
    return L"Unexpected extra encoded bytes";
  case QoiErrorCode::WrongStripCount:
    return L"Wrong number of strips";
  case QoiErrorCode::InvalidDecodeRange:
    return L"Invalid decoder range";
  case QoiErrorCode::PixelCountOverflow:
    return L"Pixel count overflow";
  case QoiErrorCode::StripDecodeFailed:
    return L"Strip decode failed";
  case QoiErrorCode::AllocationFailed:
    return L"Memory allocation failed";
  case QoiErrorCode::UnknownDecodeFailure:
    return L"Unknown decode failure";
  default:
    return L"Unspecified error";
  }
}

static std::wstring format_qoi_error(const QoiDecodeError &error) {
  std::wstring message = L"Stage: ";
  message += qoi_stage_text(error.stage);

  if (error.strip != 0) {
    message += L"\nStrip: ";
    message += std::to_wstring(error.strip);
  }

  message += L"\nByte offset: ";
  message += std::to_wstring(static_cast<unsigned long long>(error.offset));

  message += L"\nReason: ";
  message += qoi_code_text(error.code);

  return message;
}

static bool show_qoi_file(const std::wstring &path) {
  if (!has_qoi_extension(path.c_str()))
    return false;

  if (focus_existing_viewer_if_busy())
    return true;

  ViewerState *state = nullptr;

  try {
    state = new ViewerState;
    state->filename = path;
    state->loadContext = std::make_shared<LoadContext>();
    state->loading = true;
    state->loadFailed = false;
  } catch (...) {
    delete state;

    show_error(L"Unable to allocate viewer state.");
    return true;
  }

  state->filename = path;

  HWND existingHwnd = nullptr;
  HANDLE thread = nullptr;
  bool busy = false;

  AcquireSRWLockExclusive(&g_stateLock);

  if (g_shuttingDown) {
    busy = true;
  } else {
    reap_finished_viewer_locked();

    if (g_viewerThread) {
      busy = true;

      if (g_state)
        existingHwnd = g_state->hwnd;
    } else {
      // Поток может стартовать сразу, но сначала ему
      // понадобится этот же lock. Поэтому дескриптор
      // будет сохранён до начала его работы.
      thread = CreateThread(nullptr, 0, viewer_thread, state, 0, nullptr);

      if (thread)
        g_viewerThread = thread;
    }
  }

  ReleaseSRWLockExclusive(&g_stateLock);

  if (busy) {
    release_gdi_resources(*state);
    delete state;

    if (existingHwnd)
      SetForegroundWindow(existingHwnd);

    return true;
  }

  if (!thread) {
    release_gdi_resources(*state);
    delete state;

    show_error(L"Unable to start viewer.");
    return true;
  }

  // Владение state передано viewer_thread().
  return true;
}

static bool show_qoi_for_current_file() {
  std::wstring path;

  if (!get_current_filename(path))
    return false;

  return show_qoi_file(path);
}

/*
   FAR plugin initialization.
*/

extern "C" __declspec(dllexport) void WINAPI GetGlobalInfoW(GlobalInfo *info) {
  if (!info)
    return;

  info->StructSize = sizeof(*info);

  info->MinFarVersion =
      MAKEFARVERSION(FARMANAGERVERSION_MAJOR, FARMANAGERVERSION_MINOR,
                     FARMANAGERVERSION_REVISION, FARMANAGERVERSION_BUILD,
                     FARMANAGERVERSION_STAGE);

  info->Version = MAKEFARVERSION(1, 0, 0, 1, VS_RELEASE);

  info->Guid = PluginGuid;

  info->Title = L"QOI Viewer";

  info->Description = L"Fast QOI image viewer for Far Manager";

  info->Author = L"QOI Viewer";

  info->Instance = nullptr;
}

extern "C" __declspec(dllexport) void WINAPI
SetStartupInfoW(const PluginStartupInfo *info) {
  if (!info)
    return;

  g_far = *info;

  if (info->FSF)
    g_fsf = *info->FSF;

  g_far.FSF = &g_fsf;
}

/*
   F11 -> Plugins -> QOI Viewer
*/

extern "C" __declspec(dllexport) void WINAPI GetPluginInfoW(PluginInfo *info) {
  if (!info)
    return;

  static const wchar_t *const menuStrings[] = {L"QOI Viewer"};

  static const UUID menuGuids[] = {MenuGuid};

  info->StructSize = sizeof(*info);

  info->Flags = PF_PRELOAD;

  info->DiskMenu.Guids = nullptr;
  info->DiskMenu.Strings = nullptr;
  info->DiskMenu.Count = 0;

  info->PluginMenu.Guids = menuGuids;
  info->PluginMenu.Strings = menuStrings;
  info->PluginMenu.Count = 1;

  info->PluginConfig.Guids = nullptr;
  info->PluginConfig.Strings = nullptr;
  info->PluginConfig.Count = 0;

  info->CommandPrefix = L"qoi";

  info->Instance = nullptr;
}

extern "C" __declspec(dllexport) HANDLE WINAPI OpenW(const OpenInfo *info) {
  if (!info)
    return nullptr;

  if (info->OpenFrom == OPEN_PLUGINSMENU) {
    if (!info->Guid || *info->Guid != MenuGuid)
      return nullptr;

    show_qoi_for_current_file();
    return INVALID_HANDLE_VALUE;
  }

  if (info->OpenFrom == OPEN_COMMANDLINE) {
    const auto *c = reinterpret_cast<const OpenCommandLineInfo *>(info->Data);

    if (!c || !c->CommandLine || !c->CommandLine[0]) {
      show_error(L"Usage: qoi: \"C:\\path\\image.qoi\"");
      return INVALID_HANDLE_VALUE;
    }

    std::wstring path = c->CommandLine;

    while (!path.empty() && iswspace(path.front())) {
      path.erase(path.begin());
    }

    while (!path.empty() && iswspace(path.back())) {
      path.pop_back();
    }

    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
      path = path.substr(1, path.size() - 2);
    }

    if (path.empty()) {
      show_error(L"QOI file path is empty.");
      return INVALID_HANDLE_VALUE;
    }

    show_qoi_file(path);
    return INVALID_HANDLE_VALUE;
  }

  return nullptr;
}

extern "C" __declspec(dllexport) intptr_t WINAPI
ProcessConsoleInputW(ProcessConsoleInputInfo *info) {
  if (!info)
    return 0;

  if (info->StructSize < sizeof(ProcessConsoleInputInfo)) {
    return 0;
  }

  if (info->Rec.EventType != KEY_EVENT)
    return 0;

  const KEY_EVENT_RECORD &key = info->Rec.Event.KeyEvent;

  if (!key.bKeyDown)
    return 0;

  if (key.wVirtualKeyCode != VK_F3)
    return 0;

  if (key.dwControlKeyState &
      (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED |
       RIGHT_CTRL_PRESSED | SHIFT_PRESSED)) {
    return 0;
  }

  std::wstring path;

  if (!get_current_filename(path))
    return 0;

  if (!has_qoi_extension(path.c_str()))
    return 0;

  show_qoi_file(path);

  return 1;
}

extern "C" __declspec(dllexport) void WINAPI
ExitFARW(const ExitInfo * /*info*/) {
  HWND hwnd = nullptr;

  AcquireSRWLockExclusive(&g_stateLock);

  g_shuttingDown = true;

  if (g_state)
    hwnd = g_state->hwnd;

  HANDLE thread = g_viewerThread;

  ReleaseSRWLockExclusive(&g_stateLock);

  if (hwnd && !PostMessageW(hwnd, WM_CLOSE, 0, 0)) {
    // Резервный способ завершить цикл сообщений.
    const DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);

    if (threadId)
      PostThreadMessageW(threadId, WM_QUIT, 0, 0);
  }

  // Не удаляем блокировки и не выгружаем состояние,
  // пока рабочий поток использует код плагина.
  if (thread)
    WaitForSingleObject(thread, INFINITE);

  AcquireSRWLockExclusive(&g_stateLock);

  if (g_viewerThread) {
    CloseHandle(g_viewerThread);
    g_viewerThread = nullptr;
  }

  g_state = nullptr;

  ReleaseSRWLockExclusive(&g_stateLock);

  if (g_checkerBrush) {
    DeleteObject(g_checkerBrush);
    g_checkerBrush = nullptr;
  }
}