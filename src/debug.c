// debug.c - headless capture and benchmark modes, compiled only into vektor-debug.exe.
//
//   /shot  <scene> <times> <W> <H> <outdir>   render frames at the given times to BMP files.
//          times: "12.5" or "1,2,3" or "0:60:5" (start:end:step), seconds from the start.
//   /bench <scene> <seconds> <W> <H> <outfile> render every 1/60 s frame and log draw cost.
//   /seed N and /fakemonitors apply to both.

#ifdef VEKTOR_DEBUG

#include "vektor.h"
#include <stdio.h>
#include <wchar.h>

void fake_monitor_mask(HDC dc, int w, int h);

static double now_s(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static bool save_bmp(HDC dc, HBITMAP bm, int w, int h, const wchar_t* path) {
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    int stride = (w * 3 + 3) & ~3;
    DWORD size = (DWORD)stride * (DWORD)h;
    void* bits = HeapAlloc(GetProcessHeap(), 0, size);
    if (!bits) return false;
    GetDIBits(dc, bm, 0, (UINT)h, bits, &bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = { 0 };
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + size;
    FILE* f = _wfopen(path, L"wb");
    if (!f) { HeapFree(GetProcessHeap(), 0, bits); return false; }
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
    fwrite(bits, size, 1, f);
    fclose(f);
    HeapFree(GetProcessHeap(), 0, bits);
    return true;
}

static int parse_times(const wchar_t* s, float* out, int max) {
    int n = 0;
    float a, b, st;
    if (swscanf(s, L"%f:%f:%f", &a, &b, &st) == 3 && st > 0) {
        for (float t = a; t <= b + 1e-4f && n < max; t += st) out[n++] = t;
        return n;
    }
    const wchar_t* p = s;
    while (*p && n < max) {
        out[n++] = (float)wcstod(p, (wchar_t**)&p);
        if (*p == L',') p++;
        else break;
    }
    return n;
}

static bool find_flag(int argc, PWSTR* argv, const wchar_t* name, uint64_t* val) {
    for (int k = 1; k < argc; k++) {
        PWSTR a = argv[k];
        if ((*a == L'/' || *a == L'-') && !_wcsicmp(a + 1, name)) {
            if (val && k + 1 < argc) *val = wcstoull(argv[k + 1], NULL, 10);
            return true;
        }
    }
    return false;
}

int debug_main(int argc, PWSTR* argv, bool* handled) {
    int at = -1;
    bool bench = false;
    for (int k = 1; k < argc; k++) {
        if (!_wcsicmp(argv[k], L"/shot")) { at = k; break; }
        if (!_wcsicmp(argv[k], L"/bench")) { at = k; bench = true; break; }
    }
    if (at < 0) return 0;
    *handled = true;
    if (at + 5 >= argc) return 2;

    int scene = _wtoi(argv[at + 1]) - 1;
    int w = _wtoi(argv[at + 3]), h = _wtoi(argv[at + 4]);
    const wchar_t* out = argv[at + 5];
    uint64_t seed = 12345;
    find_flag(argc, argv, L"seed", &seed);
    bool fake = find_flag(argc, argv, L"fakemonitors", NULL);

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bm = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(NULL, screen);
    if (!dc || !bm) return 3;
    HBITMAP old = (HBITMAP)SelectObject(dc, bm);

    view_aspect = (float)w / (float)h;
    director_start(scene, seed);
    int rc = 0;

    if (!bench) {
        static float times[512];
        int n = parse_times(argv[at + 2], times, 512);
        CreateDirectoryW(out, NULL);
        double t = 0;
        for (int k = 0; k < n; k++) {
            while (t + SIM_STEP * 0.5 < times[k]) { director_advance(SIM_STEP); t += SIM_STEP; }
            Gfx g;
            gfx_begin(&g, dc, w, h);
            director_draw(&g);
            gfx_end(&g);
            if (fake) fake_monitor_mask(dc, w, h);
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\t%07.2f_s%d.bmp", out, times[k], director_scene() + 1);
            if (!save_bmp(dc, bm, w, h, path)) rc = 4;
        }
    } else {
        float secs = (float)_wtof(argv[at + 2]);
        FILE* f = _wfopen(out, L"w");
        if (!f) return 5;
        fprintf(f, "bench %dx%d, %.1f s from scene %d, seed %llu\n", w, h, secs, scene + 1, (unsigned long long)seed);
        fprintf(f, "%6s %5s %8s %8s %6s\n", "time", "scene", "avg_ms", "max_ms", "parts");
        double sum = 0, mx = 0, all_max = 0;
        int frames = 0, maxp = 0, slow = 0, total = 0;
        double t = 0, next_report = 1.0;
        while (t < secs) {
            director_advance(1.0f / 60.0f);
            t += 1.0 / 60.0;
            double t0 = now_s();
            Gfx g;
            gfx_begin(&g, dc, w, h);
            director_draw(&g);
            gfx_end(&g);
            GdiFlush();
            double ms = (now_s() - t0) * 1000.0;
            sum += ms;
            if (ms > mx) mx = ms;
            if (ms > all_max) all_max = ms;
            if (ms > 16.0) slow++;
            if (fx_count() > maxp) maxp = fx_count();
            frames++;
            total++;
            if (t >= next_report) {
                fprintf(f, "%6.1f %5d %8.2f %8.2f %6d\n", t, director_scene() + 1, sum / frames, mx, maxp);
                sum = mx = 0;
                frames = maxp = 0;
                next_report += 1.0;
            }
        }
        fprintf(f, "frames %d, over 16 ms: %d, worst %.2f ms\n", total, slow, all_max);
        fclose(f);
    }

    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
    return rc;
}

#endif
