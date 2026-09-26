#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using string = std::wstring;

#include "plugin.hpp"
#include "qoi.h"

static PluginStartupInfo g_far{};
static FarStandardFunctions g_fsf{};

static const GUID PluginGuid =
{
    0x6a6d1e35,
    0x7c7f,
    0x4b7b,
    { 0x9f, 0x4b, 0x2e, 0x0d, 0x1d, 0x6f, 0x52, 0x91 }
};

static const GUID MenuGuid =
{
    0x2b2c3d44,
    0x5e6f,
    0x47a1,
    { 0x91, 0x72, 0x3a, 0x8c, 0x4d, 0x51, 0x9e, 0x20 }
};

static const wchar_t* const kClassName = L"FarQoiViewerWindow";
enum class BackgroundMode
{
    Checkers,
    Black,
    White
};
struct ViewerState
{
    QoiImage image;
    std::wstring filename;

    HWND hwnd = nullptr;

    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;

    bool fitToWindow = true;

    /*
       QOI RGBA -> Windows BGRA.
       Prepared once after loading.
    */
    std::vector<uint8_t> bgra;
    HBITMAP dib = nullptr;
    HDC     dibDC = nullptr;
};

static ViewerState* g_state = nullptr;

static HBRUSH g_checkerBrush = nullptr;

static HBRUSH get_checker_brush()
{
    if (g_checkerBrush)
        return g_checkerBrush;

    constexpr int cs = 12;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = cs * 2;
    bmi.bmiHeader.biHeight      = -(cs * 2);
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(
        nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits)
        return nullptr;

    auto* px = static_cast<uint32_t*>(bits);
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

static CRITICAL_SECTION g_cs;
static bool g_cs_ready = false;
static HANDLE g_viewerThread = nullptr;


static bool has_qoi_extension(const wchar_t* name)
{
    if (!name)
        return false;

    const wchar_t* dot = wcsrchr(name, L'.');

    if (!dot)
        return false;

    return _wcsicmp(dot, L".qoi") == 0;
}


static bool load_file(const wchar_t* name, QoiImage& image)
{
    HANDLE h = CreateFileW(
        name,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER li{};

    if (!GetFileSizeEx(h, &li) ||
        li.QuadPart <= 0 ||
        static_cast<unsigned long long>(li.QuadPart) > SIZE_MAX)
    {
        CloseHandle(h);
        return false;
    }

    const size_t size = static_cast<size_t>(li.QuadPart);

    std::vector<uint8_t> data(size);

    size_t done = 0;

    while (done < size)
    {
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(size - done, 1u << 20));

        DWORD got = 0;

        if (!ReadFile(
                h,
                data.data() + done,
                chunk,
                &got,
                nullptr) ||
            got == 0)
        {
            CloseHandle(h);
            return false;
        }

        done += got;
    }

    CloseHandle(h);

    return qoi_decode(data.data(), data.size(), image);
}


static bool prepare_bgra(ViewerState& s)
{
    if (s.image.rgba.empty())
        return false;

    s.bgra.resize(s.image.rgba.size());

    for (size_t i = 0; i < s.bgra.size(); i += 4) {
        const uint8_t r = s.image.rgba[i + 0];
        const uint8_t g = s.image.rgba[i + 1];
        const uint8_t b = s.image.rgba[i + 2];
        const uint8_t a = s.image.rgba[i + 3];

        // Premultiplied BGRA for AlphaBlend
        s.bgra[i + 0] = static_cast<uint8_t>((b * a) / 255);
        s.bgra[i + 1] = static_cast<uint8_t>((g * a) / 255);
        s.bgra[i + 2] = static_cast<uint8_t>((r * a) / 255);
        s.bgra[i + 3] = a;
    }
    if (s.dib) {
        DeleteObject(s.dib);
        s.dib = nullptr;
    }
    if (s.dibDC) {
        DeleteDC(s.dibDC);
        s.dibDC = nullptr;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(s.image.width);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(s.image.height);
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s.dibDC = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    s.dib = CreateDIBSection(
        s.dibDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

    if (!s.dib || !bits) {
        if (s.dibDC) {
            DeleteDC(s.dibDC);
            s.dibDC = nullptr;
        }
        return false;
    }

    memcpy(bits, s.bgra.data(), s.bgra.size());
    SelectObject(s.dibDC, s.dib);

    return true;
}

static void checker(
    HDC dc,
    const RECT& r)
{
    HBRUSH brush = get_checker_brush();
    if (!brush) {
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
        return;
    }
    FillRect(dc, &r, brush);
}

/*
   Calculate the exact rectangle occupied by the image.
*/
static bool get_image_rect(
    HWND hwnd,
    const ViewerState& s,
    RECT& out)
{
    RECT rc{};

    GetClientRect(hwnd, &rc);

    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    if (cw <= 0 ||
        ch <= 0 ||
        s.image.width == 0 ||
        s.image.height == 0)
    {
        SetRectEmpty(&out);
        return false;
    }

    const double fit = std::min(
        static_cast<double>(cw) /
            static_cast<double>(s.image.width),
        static_cast<double>(ch) /
            static_cast<double>(s.image.height));

    if (fit <= 0.0)
    {
        SetRectEmpty(&out);
        return false;
    }

    const double scale =
        s.fitToWindow
            ? fit
            : s.zoom;

    const int dw = std::max(
        1,
        static_cast<int>(std::lround(
            static_cast<double>(s.image.width) * scale)));

    const int dh = std::max(
        1,
        static_cast<int>(std::lround(
            static_cast<double>(s.image.height) * scale)));

    const int x = static_cast<int>(
        std::lround((cw - dw) * 0.5 + s.panX));

    const int y = static_cast<int>(
        std::lround((ch - dh) * 0.5 + s.panY));

    out.left = x;
    out.top = y;
    out.right = x + dw;
    out.bottom = y + dh;

    return true;
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
static void invalidate_image_change(
    HWND hwnd,
    const RECT& oldRect,
    const RECT& newRect)
{
    if (EqualRect(&oldRect, &newRect))
        return;

    RECT changed{};

    if (IsRectEmpty(&oldRect))
    {
        changed = newRect;
    }
    else if (IsRectEmpty(&newRect))
    {
        changed = oldRect;
    }
    else
    {
        changed.left =
            std::min(oldRect.left, newRect.left);

        changed.top =
            std::min(oldRect.top, newRect.top);

        changed.right =
            std::max(oldRect.right, newRect.right);

        changed.bottom =
            std::max(oldRect.bottom, newRect.bottom);
    }

    InvalidateRect(
        hwnd,
        &changed,
        FALSE);
}


/*
   Draw only the current paint region.

   BeginPaint clips the DC to ps.rcPaint, so there is
   no need to redraw the whole client area.
*/
static void draw_image(
    HWND hwnd,
    HDC dc,
    ViewerState& s,
    const RECT& paintRect)
{
    RECT imageRect{};

    if (!get_image_rect(hwnd, s, imageRect))
    {
        checker(dc, paintRect);
        return;
    }

    HRGN paintRegion =
        CreateRectRgnIndirect(&paintRect);

    HRGN imageRegion =
        CreateRectRgnIndirect(&imageRect);

    HRGN backgroundRegion =
        CreateRectRgn(0, 0, 0, 0);

    HRGN imagePaintRegion =
        CreateRectRgn(0, 0, 0, 0);

    if (!paintRegion ||
        !imageRegion ||
        !backgroundRegion ||
        !imagePaintRegion)
    {
        if (paintRegion)
            DeleteObject(paintRegion);

        if (imageRegion)
            DeleteObject(imageRegion);

        if (backgroundRegion)
            DeleteObject(backgroundRegion);

        if (imagePaintRegion)
            DeleteObject(imagePaintRegion);

        return;
    }

    /*
       Background:
           paintRegion - imageRegion
    */
    CombineRgn(
        backgroundRegion,
        paintRegion,
        imageRegion,
        RGN_DIFF);

    /*
       Image:
           paintRegion ∩ imageRegion
    */
    CombineRgn(
        imagePaintRegion,
        paintRegion,
        imageRegion,
        RGN_AND);

    /*
       1. Draw checkerboard ONLY where the current
          image does not exist.
    */
    SelectClipRgn(dc, paintRegion);
    checker(dc, paintRect);

    SelectClipRgn(dc, imagePaintRegion);

    if (s.dibDC && s.dib) {
        const int dw = imageRect.right - imageRect.left;
        const int dh = imageRect.bottom - imageRect.top;

        BLENDFUNCTION bf{};
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat         = AC_SRC_ALPHA;

        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);

        AlphaBlend(
            dc,
            imageRect.left,
            imageRect.top,
            dw,
            dh,
            s.dibDC,
            0,
            0,
            static_cast<int>(s.image.width),
            static_cast<int>(s.image.height),
            bf);
    }

    SelectClipRgn(dc, nullptr);

    DeleteObject(paintRegion);
    DeleteObject(imageRegion);
    DeleteObject(backgroundRegion);
    DeleteObject(imagePaintRegion);
}

static void update_title(
    HWND hwnd,
    const ViewerState& s)
{
    wchar_t mode[64]{};

    if (s.fitToWindow) {
        wcscpy_s(mode, L"Fit");
    } else {
        const int pct = static_cast<int>(
            std::lround(s.zoom * 100.0));
        swprintf_s(mode, L"%d%%", pct);
    }

    wchar_t title[512]{};

    swprintf_s(
        title,
        L"%s - %ux%u  %s  |  %s  |  "
        L"1 100%%  0 fit  +/- zoom  Space/LMB switch  Esc close",
        s.filename.c_str(),
        s.image.width,
        s.image.height,
        s.image.channels == 4 ? L"RGBA" : L"RGB",
        mode);

    SetWindowTextW(hwnd, title);
}

static void change_zoom(
    HWND hwnd,
    ViewerState& s,
    double newZoom)
{
    RECT oldRect{};
    get_image_rect(hwnd, s, oldRect);

    s.fitToWindow = false;
    s.zoom = std::clamp(newZoom, 0.05, 32.0);

    RECT newRect{};
    get_image_rect(hwnd, s, newRect);

    invalidate_image_change(
        hwnd,
        oldRect,
        newRect);

    update_title(hwnd, s);
}


static void set_fit_mode(
    HWND hwnd,
    ViewerState& s)
{
    RECT oldRect{};
    get_image_rect(hwnd, s, oldRect);

    s.fitToWindow = true;
    s.panX = 0;
    s.panY = 0;

    RECT newRect{};
    get_image_rect(hwnd, s, newRect);

    invalidate_image_change(
        hwnd,
        oldRect,
        newRect);

    update_title(hwnd, s);
}


static void set_100_percent(
    HWND hwnd,
    ViewerState& s)
{
    RECT oldRect{};
    get_image_rect(hwnd, s, oldRect);

    s.fitToWindow = false;
    s.zoom = 1.0;
    s.panX = 0;
    s.panY = 0;

    RECT newRect{};
    get_image_rect(hwnd, s, newRect);

    invalidate_image_change(
        hwnd,
        oldRect,
        newRect);

    update_title(hwnd, s);
}


static void toggle_view_mode(
    HWND hwnd,
    ViewerState& s)
{
    RECT oldRect{};
    get_image_rect(hwnd, s, oldRect);

    s.fitToWindow = !s.fitToWindow;

    s.panX = 0;
    s.panY = 0;

    RECT newRect{};
    get_image_rect(hwnd, s, newRect);

    invalidate_image_change(
        hwnd,
        oldRect,
        newRect);

    update_title(hwnd, s);
}


static LRESULT CALLBACK wndproc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    ViewerState* s =
        reinterpret_cast<ViewerState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
        case WM_CREATE:
        {
            auto* cs =
                reinterpret_cast<CREATESTRUCTW*>(lp);

            s = reinterpret_cast<ViewerState*>(
                cs->lpCreateParams);

            SetWindowLongPtrW(
                hwnd,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(s));

            s->hwnd = hwnd;

            update_title(hwnd, *s);

            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            if (s) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                const int cw = rc.right - rc.left;
                const int ch = rc.bottom - rc.top;

                if (cw > 0 && ch > 0) {
                    HDC mem = CreateCompatibleDC(dc);
                    HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
                    HGDIOBJ old = SelectObject(mem, bmp);

                    draw_image(hwnd, mem, *s, ps.rcPaint);

                    BitBlt(
                        dc,
                        ps.rcPaint.left,
                        ps.rcPaint.top,
                        ps.rcPaint.right - ps.rcPaint.left,
                        ps.rcPaint.bottom - ps.rcPaint.top,
                        mem,
                        ps.rcPaint.left,
                        ps.rcPaint.top,
                        SRCCOPY);

                    SelectObject(mem, old);
                    DeleteObject(bmp);
                    DeleteDC(mem);
                }
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
            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;

        case WM_MOUSEWHEEL:
        {
            if (!s)
                return 0;

            const int delta =
                GET_WHEEL_DELTA_WPARAM(wp);

            const double newZoom =
                delta > 0
                    ? s->zoom * 1.20
                    : s->zoom / 1.20;

            change_zoom(
                hwnd,
                *s,
                newZoom);

            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            if (!s)
                return 0;

            /*
               No mouse dragging.
               Left click toggles Fit / explicit zoom.
            */
            toggle_view_mode(hwnd, *s);

            return 0;
        }

        case WM_KEYDOWN:
        {
            if (!s)
                break;

            switch (wp)
            {
                case VK_ESCAPE:
                    DestroyWindow(hwnd);
                    return 0;

                case VK_SPACE:
                    toggle_view_mode(hwnd, *s);
                    return 0;

                case VK_HOME:
                {
                    RECT oldRect{};
                    get_image_rect(hwnd, *s, oldRect);

                    s->panX = 0;
                    s->panY = 0;

                    RECT newRect{};
                    get_image_rect(hwnd, *s, newRect);

                    invalidate_image_change(
                        hwnd,
                        oldRect,
                        newRect);

                    return 0;
                }

                case '1':
                    set_100_percent(hwnd, *s);
                    return 0;

                case '0':
                    set_fit_mode(hwnd, *s);
                    return 0;

                case VK_ADD:
                case VK_OEM_PLUS:
                    change_zoom(
                        hwnd,
                        *s,
                        s->zoom * 1.20);
                    return 0;

                case VK_SUBTRACT:
                case VK_OEM_MINUS:
                    change_zoom(
                        hwnd,
                        *s,
                        s->zoom / 1.20);
                    return 0;

                case VK_LEFT:
                {
                    RECT oldRect{};
                    get_image_rect(hwnd, *s, oldRect);

                    s->panX += 32;

                    RECT newRect{};
                    get_image_rect(hwnd, *s, newRect);

                    invalidate_image_change(
                        hwnd,
                        oldRect,
                        newRect);

                    return 0;
                }

                case VK_RIGHT:
                {
                    RECT oldRect{};
                    get_image_rect(hwnd, *s, oldRect);

                    s->panX -= 32;

                    RECT newRect{};
                    get_image_rect(hwnd, *s, newRect);

                    invalidate_image_change(
                        hwnd,
                        oldRect,
                        newRect);

                    return 0;
                }

                case VK_UP:
                {
                    RECT oldRect{};
                    get_image_rect(hwnd, *s, oldRect);

                    s->panY += 32;

                    RECT newRect{};
                    get_image_rect(hwnd, *s, newRect);

                    invalidate_image_change(
                        hwnd,
                        oldRect,
                        newRect);

                    return 0;
                }

                case VK_DOWN:
                {
                    RECT oldRect{};
                    get_image_rect(hwnd, *s, oldRect);

                    s->panY -= 32;

                    RECT newRect{};
                    get_image_rect(hwnd, *s, newRect);

                    invalidate_image_change(
                        hwnd,
                        oldRect,
                        newRect);

                    return 0;
                }
            }

            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}


static DWORD WINAPI viewer_thread(LPVOID param)
{
std::unique_ptr<ViewerState> state(
        reinterpret_cast<ViewerState*>(param));

    EnterCriticalSection(&g_cs);
    g_state = state.get();
    LeaveCriticalSection(&g_cs);

    const HINSTANCE inst =
        GetModuleHandleW(nullptr);

WNDCLASSW wc{};
if (!GetClassInfoW(inst, kClassName, &wc)) {
    wc = {};
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClassName;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    if (!RegisterClassW(&wc))
        return 1;
}
    RegisterClassW(&wc);

    RECT wa{};

    SystemParametersInfoW(
        SPI_GETWORKAREA,
        0,
        &wa,
        0);

    const int workWidth =
        wa.right - wa.left;

    const int workHeight =
        wa.bottom - wa.top;

    const int ww =
        std::min(workWidth, 1200);

    const int wh =
        std::min(workHeight, 900);

    const int x =
        wa.left + (workWidth - ww) / 2;

    const int y =
        wa.top + (workHeight - wh) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kClassName,
        L"QOI",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x,
        y,
        ww,
        wh,
        nullptr,
        nullptr,
        inst,
        state.get());

    if (!hwnd)
    {
        g_state = nullptr;
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Забрать фокус у FAR
    HWND fg = GetForegroundWindow();
    DWORD farTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD ourTid = GetCurrentThreadId();

    if (farTid && farTid != ourTid)
        AttachThreadInput(farTid, ourTid, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (farTid && farTid != ourTid)
        AttachThreadInput(farTid, ourTid, FALSE);

    MSG msg{};

    while (GetMessageW(
               &msg,
               nullptr,
               0,
               0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (state->dib) {
        DeleteObject(state->dib);
        state->dib = nullptr;
    }
    if (state->dibDC) {
        DeleteDC(state->dibDC);
        state->dibDC = nullptr;
    }
UnregisterClassW(kClassName, inst);
    g_state = nullptr;
EnterCriticalSection(&g_cs);
g_state = nullptr;
if (g_viewerThread) {
    CloseHandle(g_viewerThread);
    g_viewerThread = nullptr;
}
LeaveCriticalSection(&g_cs);
    return 0;
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

static bool get_current_filename(std::wstring& path)
{
    if (!g_far.PanelControl)
        return false;

    PanelInfo pi{};

    pi.StructSize = sizeof(pi);

    if (!g_far.PanelControl(
            PANEL_ACTIVE,
            FCTL_GETPANELINFO,
            0,
            &pi))
    {
        return false;
    }

    if (pi.PanelType != PTYPE_FILEPANEL)
        return false;

    FarGetPluginPanelItem item{};

    item.StructSize = sizeof(item);

    const intptr_t required =
        g_far.PanelControl(
            PANEL_ACTIVE,
            FCTL_GETCURRENTPANELITEM,
            0,
            &item);

    if (required <= 0)
        return false;

    std::vector<unsigned char> itemBuffer(
        static_cast<size_t>(required));

    item.StructSize = sizeof(item);
    item.Size = itemBuffer.size();
    item.Item =
        reinterpret_cast<PluginPanelItem*>(
            itemBuffer.data());

    if (!g_far.PanelControl(
            PANEL_ACTIVE,
            FCTL_GETCURRENTPANELITEM,
            0,
            &item))
    {
        return false;
    }

    if (!item.Item ||
        !item.Item->FileName ||
        !item.Item->FileName[0])
    {
        return false;
    }

    const std::wstring filename =
        item.Item->FileName;

    /*
       First ask FAR for the required directory
       buffer size.
    */

    const intptr_t dirSize =
        g_far.PanelControl(
            PANEL_ACTIVE,
            FCTL_GETPANELDIRECTORY,
            0,
            nullptr);

    if (dirSize <= 0)
        return false;

    std::vector<unsigned char> dirBuffer(
        static_cast<size_t>(dirSize));

    auto* dir =
        reinterpret_cast<FarPanelDirectory*>(
            dirBuffer.data());

    dir->StructSize = sizeof(FarPanelDirectory);

    if (!g_far.PanelControl(
            PANEL_ACTIVE,
            FCTL_GETPANELDIRECTORY,
            dirSize,
            dir))
    {
        return false;
    }

    if (!dir->Name || !dir->Name[0])
        return false;

    path = dir->Name;

    if (!path.empty() &&
        path.back() != L'\\')
    {
        path.push_back(L'\\');
    }

    path += filename;

    return true;
}


static void show_error(const wchar_t* text)
{
    if (!g_far.Message)
        return;

    const wchar_t* items[] =
    {
        L"FarQoiViewer",
        L"",
        text,
        L"",
        L"OK"
    };

    g_far.Message(
        &PluginGuid,
        nullptr,
        FMSG_ERRORTYPE | FMSG_MB_OK,
        nullptr,
        items,
        5,
        1);
}


static bool show_qoi_file(const std::wstring& path)
{
    if (!has_qoi_extension(path.c_str()))
        return false;

    QoiImage image;

    if (!load_file(path.c_str(), image))
    {
        show_error(
            L"Invalid or unsupported QOI file.");

        return true;
    }

    auto* state = new ViewerState;

    state->image = std::move(image);

    if (!prepare_bgra(*state))
    {
        delete state;

        show_error(
            L"Unable to prepare image.");

        return true;
    }

    state->filename = path;

EnterCriticalSection(&g_cs);
if (g_viewerThread) {
    // уже есть viewer — не открываем второй
    LeaveCriticalSection(&g_cs);
    if (g_state && g_state->hwnd)
        SetForegroundWindow(g_state->hwnd);
    delete state;
    return true;
}

HANDLE thread = CreateThread(nullptr, 0, viewer_thread, state, 0, nullptr);
if (!thread) {
    LeaveCriticalSection(&g_cs);
    delete state;
    return true;
}
g_viewerThread = thread;
LeaveCriticalSection(&g_cs);

    return true;
}


static bool show_qoi_for_current_file()
{
    std::wstring path;

    if (!get_current_filename(path))
        return false;

    return show_qoi_file(path);
}


/*
   FAR plugin initialization.
*/

extern "C" __declspec(dllexport)
void WINAPI GetGlobalInfoW(GlobalInfo* info)
{
    if (!info)
        return;

    info->StructSize = sizeof(*info);

    info->MinFarVersion = MAKEFARVERSION(
        FARMANAGERVERSION_MAJOR,
        FARMANAGERVERSION_MINOR,
        FARMANAGERVERSION_REVISION,
        FARMANAGERVERSION_BUILD,
        FARMANAGERVERSION_STAGE);

    info->Version =
        MAKEFARVERSION(
            1,
            0,
            0,
            1,
            VS_RELEASE);

    info->Guid = PluginGuid;

    info->Title =
        L"QOI Viewer";

    info->Description =
        L"Fast QOI image viewer for Far Manager";

    info->Author =
        L"QOI Viewer";

    info->Instance = nullptr;
}


extern "C" __declspec(dllexport)
void WINAPI SetStartupInfoW(
    const PluginStartupInfo* info)
{
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
    }
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

extern "C" __declspec(dllexport)
void WINAPI GetPluginInfoW(
    PluginInfo* info)
{
    if (!info)
        return;

    static const wchar_t* const menuStrings[] =
    {
        L"QOI Viewer"
    };

    static const UUID menuGuids[] =
    {
        MenuGuid
    };

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


extern "C" __declspec(dllexport)
HANDLE WINAPI OpenW(const OpenInfo* info)
{
    if (!info)
        return nullptr;

    if (info->OpenFrom == OPEN_PLUGINSMENU) {
        if (!info->Guid || *info->Guid != MenuGuid)
            return nullptr;

        show_qoi_for_current_file();
        return nullptr;
    }

    if (info->OpenFrom == OPEN_COMMANDLINE) {
        auto* c = reinterpret_cast<const OpenCommandLineInfo*>(info->Data);
        if (!c || !c->CommandLine || !c->CommandLine[0])
            return nullptr;

        // "qoi path\to\file.qoi" или просто путь после префикса
        std::wstring path = c->CommandLine;

        // убрать ведущие пробелы
        while (!path.empty() && path.front() == L' ')
            path.erase(path.begin());

        // если путь в кавычках — снять
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
            path = path.substr(1, path.size() - 2);
        }

        if (path.empty())
            return nullptr;

        show_qoi_file(path);
        return nullptr;
    }

    return nullptr;
}

extern "C" __declspec(dllexport)
intptr_t WINAPI ProcessConsoleInputW(
    ProcessConsoleInputInfo* info)
{
    if (!info)
        return 0;

    if (info->StructSize <
        sizeof(ProcessConsoleInputInfo))
    {
        return 0;
    }

    if (info->Rec.EventType != KEY_EVENT)
        return 0;

    const KEY_EVENT_RECORD& key =
        info->Rec.Event.KeyEvent;

    if (!key.bKeyDown)
        return 0;

    if (key.wVirtualKeyCode != VK_F3)
        return 0;

    if (key.dwControlKeyState &
        (LEFT_ALT_PRESSED |
         RIGHT_ALT_PRESSED |
         LEFT_CTRL_PRESSED |
         RIGHT_CTRL_PRESSED |
         SHIFT_PRESSED))
    {
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

extern "C" __declspec(dllexport)
void WINAPI ExitFARW(const ExitInfo* /*info*/)
{
    HWND hwnd = nullptr;
    HANDLE thread = nullptr;

    if (g_cs_ready) {
        EnterCriticalSection(&g_cs);
        if (g_state)
            hwnd = g_state->hwnd;
        thread = g_viewerThread;
        LeaveCriticalSection(&g_cs);
    }

    if (hwnd)
        PostMessageW(hwnd, WM_CLOSE, 0, 0);

    if (thread) {
        WaitForSingleObject(thread, 3000);
        // handle закроет сам viewer_thread
    }

    if (g_cs_ready) {
        DeleteCriticalSection(&g_cs);
        g_cs_ready = false;
    }
    if (g_checkerBrush) {
        DeleteObject(g_checkerBrush);
        g_checkerBrush = nullptr;
    }
}