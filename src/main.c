// main.c - Win32 screensaver shell: argument parsing, the full-screen and preview windows,
// exit-on-input rules, the frame loop and the back buffer. Debug builds add /fakemonitors,
// /scene, /seed and the headless /shot and /bench modes (debug.c).

#include "vektor.h"
#include <windowsx.h>
#include <shellapi.h>
#include <wchar.h>

#ifdef VEKTOR_DEBUG
int debug_main(int argc, PWSTR* argv, bool* handled);
#endif

static bool g_preview;
static bool g_fake;            // /fakemonitors: two pretend monitors on one screen
static int g_start_scene;
static uint64_t g_seed;
static bool g_seed_set;

static HDC g_bufDC;
static HBITMAP g_bufBM, g_bufOld;
static int g_bufW, g_bufH;

// ---- back buffer --------------------------------------------------------------------

static void free_buffer(void) {
    if (!g_bufDC) return;
    SelectObject(g_bufDC, g_bufOld);
    DeleteObject(g_bufBM);
    DeleteDC(g_bufDC);
    g_bufDC = NULL;
    g_bufBM = NULL;
    g_bufW = g_bufH = 0;
}

static bool ensure_buffer(HDC wdc, int w, int h) {
    if (g_bufDC && g_bufW == w && g_bufH == h) return true;
    free_buffer();
    g_bufDC = CreateCompatibleDC(wdc);
    g_bufBM = CreateCompatibleBitmap(wdc, w, h);   // from the window DC, or it is monochrome
    if (!g_bufDC || !g_bufBM) {
        if (g_bufBM) DeleteObject(g_bufBM);
        if (g_bufDC) DeleteDC(g_bufDC);
        g_bufDC = NULL;
        g_bufBM = NULL;
        return false;
    }
    g_bufOld = (HBITMAP)SelectObject(g_bufDC, g_bufBM);
    g_bufW = w;
    g_bufH = h;
    return true;
}

// With /fakemonitors the screen is two monitors of different heights with a bezel between
// them. The scene is drawn across the whole span; whatever no monitor covers is blacked out.
void fake_monitor_mask(HDC dc, int w, int h) {
    int split = (int)(w * 0.56f), gap = w / 60, top = h / 8, bottom = h - h / 10;
    PatBlt(dc, split, 0, gap, h, BLACKNESS);                  // bezel
    PatBlt(dc, split + gap, 0, w - split - gap, top, BLACKNESS);
    PatBlt(dc, split + gap, bottom, w - split - gap, h - bottom, BLACKNESS);
}

static void render_frame(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC wdc = GetDC(hwnd);
    if (ensure_buffer(wdc, w, h)) {
        Gfx g;
        gfx_begin(&g, g_bufDC, w, h);
        director_draw(&g);
        gfx_end(&g);
        if (g_fake) fake_monitor_mask(g_bufDC, w, h);
        BitBlt(wdc, 0, 0, w, h, g_bufDC, 0, 0, SRCCOPY);
    }
    ReleaseDC(hwnd, wdc);
}

// ---- window procedure ---------------------------------------------------------------

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_bufDC) BitBlt(dc, 0, 0, g_bufW, g_bufH, g_bufDC, 0, 0, SRCCOPY);
        else PatBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                    ps.rcPaint.bottom - ps.rcPaint.top, BLACKNESS);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR:
        if (g_preview) break;
        SetCursor(NULL);
        return TRUE;

    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (g_preview) break;
        DestroyWindow(hwnd);
        return 0;

    case WM_MOUSEMOVE: {
        if (g_preview) break;
        // The first WM_MOUSEMOVE only reports where the pointer already is.
        static bool have_start;
        static int sx, sy;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (!have_start) { have_start = true; sx = x; sy = y; return 0; }
        if (abs(x - sx) > 4 || abs(y - sy) > 4) DestroyWindow(hwnd);
        return 0;
    }
    case WM_ACTIVATEAPP:
        if (g_preview) break;
        if (!wp) DestroyWindow(hwnd);
        return 0;

    case WM_SYSCOMMAND:
        if (!g_preview && (wp & 0xFFF0) == SC_SCREENSAVE) return 0;
        break;

    case WM_DESTROY:
        free_buffer();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- frame loop ---------------------------------------------------------------------

static double qpc_seconds(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static uint64_t qpc_seed(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId();
}

static int run_loop(HWND hwnd) {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.bottom > rc.top) view_aspect = (float)(rc.right - rc.left) / (float)(rc.bottom - rc.top);
    director_start(g_start_scene, g_seed_set ? g_seed : qpc_seed());

    // 60 fps, dropping to 30 if frames run long; the preview always runs at 30.
    double interval = g_preview ? 1.0 / 30.0 : 1.0 / 60.0;
    double last = qpc_seconds(), next = last, cost = 0;
    int slow = 0, fast = 0;

    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { CloseHandle(timer); return (int)msg.wParam; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        double now = qpc_seconds();
        if (now < next) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((next - now) * 1e7);
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            MsgWaitForMultipleObjects(1, &timer, FALSE, INFINITE, QS_ALLINPUT);
            continue;
        }

        director_advance((float)(now - last));
        last = now;
        render_frame(hwnd);
        double done = qpc_seconds();
        cost = cost * 0.9 + (done - now) * 0.1;

        if (!g_preview) {
            if (interval < 0.02 && cost > 0.015) { if (++slow > 60) { interval = 1.0 / 30.0; slow = 0; } }
            else slow = 0;
            if (interval > 0.02 && cost < 0.008) { if (++fast > 300) { interval = 1.0 / 60.0; fast = 0; } }
            else fast = 0;
        }
        next += interval;
        if (next < done) next = done + interval * 0.5;
    }
}

static ATOM register_class(void) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"VektorSaver";
    wc.hCursor = g_preview ? LoadCursor(NULL, IDC_ARROW) : NULL;
    wc.hbrBackground = NULL;
    return RegisterClassW(&wc);
}

static int run_fullscreen(void) {
    register_class();
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"VektorSaver", L"Vektor", WS_POPUP,
                                x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    return run_loop(hwnd);
}

static int run_preview(HWND parent) {
    register_class();
    RECT rc;
    GetClientRect(parent, &rc);
    HWND hwnd = CreateWindowExW(0, L"VektorSaver", NULL, WS_CHILD | WS_VISIBLE,
                                0, 0, rc.right - rc.left, rc.bottom - rc.top,
                                parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hwnd) return 1;
    return run_loop(hwnd);
}

static int run_configure(HWND parent) {
    MessageBoxW(parent, L"Vektor 1.0\n\nVector graphics screensaver. There are no settings.",
                L"Vektor", MB_OK | MB_ICONINFORMATION);
    return 0;
}

// "/p 1234", "/p:1234", "-P:1234" all name the same window.
static HWND parse_hwnd(PWSTR arg, int argc, PWSTR* argv, int idx) {
    PWSTR s = arg;
    if (*s == L':') s++;
    else if (*s == 0) s = idx + 1 < argc ? argv[idx + 1] : NULL;
    if (!s || !*s) return NULL;
    HWND h = (HWND)(UINT_PTR)wcstoull(s, NULL, 10);
    return IsWindow(h) ? h : NULL;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)inst; (void)prev; (void)cmd; (void)show;
    int argc;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 0;
    gfx_startup();

    int mode = 's';
    HWND parent = NULL;
    for (int k = 1; k < argc; k++) {
        PWSTR a = argv[k];
        if (*a != L'/' && *a != L'-') continue;
        a++;
#ifdef VEKTOR_DEBUG
        if (!_wcsicmp(a, L"fakemonitors")) { g_fake = true; continue; }
        if (!_wcsicmp(a, L"scene") && k + 1 < argc) { g_start_scene = _wtoi(argv[++k]) - 1; continue; }
        if (!_wcsicmp(a, L"seed") && k + 1 < argc) { g_seed = wcstoull(argv[++k], NULL, 10); g_seed_set = true; continue; }
#endif
        wchar_t c = towlower(*a);
        if (c == L's' && a[1] == 0) mode = 's';
        else if (c == L'p' && (a[1] == 0 || a[1] == L':')) { mode = 'p'; parent = parse_hwnd(a + 1, argc, argv, k); }
        else if (c == L'c' && (a[1] == 0 || a[1] == L':')) { mode = 'c'; parent = parse_hwnd(a + 1, argc, argv, k); }
    }

    int rc = 0;
#ifdef VEKTOR_DEBUG
    bool handled = false;
    rc = debug_main(argc, argv, &handled);
    if (handled) { LocalFree(argv); gfx_shutdown(); return rc; }
#endif
    LocalFree(argv);

    if (mode == 'p') {
        if (!parent) { gfx_shutdown(); return 0; }
        g_preview = true;
        rc = run_preview(parent);
    } else if (mode == 'c') {
        rc = run_configure(parent);
    } else {
        rc = run_fullscreen();
    }
    gfx_shutdown();
    return rc;
}
