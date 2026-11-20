/*
 * WinShell File Manager  -  native C (Win32 / GDI) port of FilesV3_1.py
 *
 * Runs on Windows and under Wine/Winlator (Android).  No runtime dependencies
 * other than the Win32 DLLs that ship with Wine.  GDI+ is loaded dynamically
 * (optional: used only for image thumbnails/preview).
 *
 * Build (MinGW-w64, 32-bit  -> works in every Winlator container):
 *   i686-w64-mingw32-gcc -O2 -municode -mwindows winshell_fm.c -o WinShellFM.exe \
 *       -lgdi32 -luser32 -lshell32 -lcomdlg32 -lcomctl32 -lole32 -luuid -lshlwapi -ladvapi32 -lmsimg32 -static
 * 64-bit:  x86_64-w64-mingw32-gcc  (same flags)
 *
 * Config file is the same JSON file the Python version uses
 * (%PROGRAMDATA%\WinShell\fm_config.json); unknown keys are preserved.
 */
#define UNICODE
#define _UNICODE
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#define _WIN32_IE 0x0600
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <math.h>

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;
#define CAP(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define UNSET (-32768)

/* ========================================================================== */
/* small utilities                                                            */
/* ========================================================================== */
static void scpy(WCHAR *d, int cap, const WCHAR *s) { lstrcpynW(d, s ? s : L"", cap); }
static void scat(WCHAR *d, int cap, const WCHAR *s) {
    int l = lstrlenW(d); if (l < cap - 1) lstrcpynW(d + l, s, cap - l);
}
static void wfmt(WCHAR *d, int cap, const WCHAR *f, ...) {
    va_list a; va_start(a, f); _vsnwprintf(d, cap, f, a); va_end(a); d[cap - 1] = 0;
}
static WCHAR *wdup(const WCHAR *s) {
    size_t n = lstrlenW(s) + 1; WCHAR *p = (WCHAR *)malloc(n * sizeof(WCHAR)); memcpy(p, s, n * sizeof(WCHAR)); return p;
}
static int wieq(const WCHAR *a, const WCHAR *b) { return lstrcmpiW(a, b) == 0; }

/* string builder */
typedef struct { WCHAR *s; int n, cap; } SB;
static void sb_addn(SB *b, const WCHAR *s, int n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 256; b->s = (WCHAR *)realloc(b->s, b->cap * sizeof(WCHAR));
    }
    memcpy(b->s + b->n, s, n * sizeof(WCHAR)); b->n += n; b->s[b->n] = 0;
}
static void sb_add(SB *b, const WCHAR *s) { sb_addn(b, s, lstrlenW(s)); }
static void sb_addc(SB *b, WCHAR c) { sb_addn(b, &c, 1); }
static void sb_addf(SB *b, const WCHAR *f, ...) {
    WCHAR t[512]; va_list a; va_start(a, f); _vsnwprintf(t, 512, f, a); va_end(a); t[511] = 0; sb_add(b, t);
}
static void sb_jstr(SB *b, const WCHAR *s) {
    sb_addc(b, '"');
    for (; *s; s++) {
        WCHAR c = *s;
        if (c == '"' || c == '\\') { sb_addc(b, '\\'); sb_addc(b, c); }
        else if (c == '\n') sb_add(b, L"\\n");
        else if (c == '\r') sb_add(b, L"\\r");
        else if (c == '\t') sb_add(b, L"\\t");
        else if (c < 32 || c > 126) { WCHAR t[12]; wfmt(t, 12, L"\\u%04x", (unsigned)c); sb_add(b, t); }
        else sb_addc(b, c);
    }
    sb_addc(b, '"');
}

/* string list */
typedef struct { WCHAR **v; int n, cap; } SL;
static void sl_add(SL *l, const WCHAR *s) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = (WCHAR **)realloc(l->v, l->cap * sizeof(WCHAR *)); }
    l->v[l->n++] = wdup(s);
}
static int sl_find(const SL *l, const WCHAR *s) {
    for (int i = 0; i < l->n; i++) if (wieq(l->v[i], s)) return i;
    return -1;
}
static void sl_del(SL *l, int i) {
    free(l->v[i]); memmove(l->v + i, l->v + i + 1, (l->n - i - 1) * sizeof(WCHAR *)); l->n--;
}
static void sl_clear(SL *l) { for (int i = 0; i < l->n; i++) free(l->v[i]); l->n = 0; }
static void sl_front(SL *l, const WCHAR *s, int max) {
    int i = sl_find(l, s); if (i >= 0) sl_del(l, i);
    sl_add(l, s); WCHAR *x = l->v[l->n - 1];
    memmove(l->v + 1, l->v, (l->n - 1) * sizeof(WCHAR *)); l->v[0] = x;
    while (l->n > max) sl_del(l, l->n - 1);
}

/* ========================================================================== */
/* logging + Wine detection                                                   */
/* ========================================================================== */
static WCHAR g_logpath[MAX_PATH];
static int ON_WINE = 0;
static void LOG(const char *fmt, ...) {
    FILE *f = _wfopen(g_logpath, L"ab"); if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d INFO ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}
static void log_init(void) {
    WCHAR t[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"TEMP", t, MAX_PATH);
    if (!n || n >= MAX_PATH) lstrcpyW(t, L"C:\\");
    wfmt(g_logpath, MAX_PATH, L"%ls%lswinshell_fm.log", t, (t[lstrlenW(t) - 1] == '\\') ? L"" : L"\\");
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExW(g_logpath, GetFileExInfoStandard, &d) && d.nFileSizeLow > 1000000) {
        WCHAR o[MAX_PATH]; wfmt(o, MAX_PATH, L"%ls.1", g_logpath);
        MoveFileExW(g_logpath, o, MOVEFILE_REPLACE_EXISTING);
    }
}
static void detect_wine(void) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt && GetProcAddress(nt, "wine_get_version")) { ON_WINE = 1; return; }
    WCHAR b[8];
    if (GetEnvironmentVariableW(L"WINEPREFIX", b, 8) || GetEnvironmentVariableW(L"WINELOADER", b, 8) ||
        GetEnvironmentVariableW(L"WINEARCH", b, 8)) ON_WINE = 1;
}

/* ========================================================================== */
/* colour helpers                                                             */
/* ========================================================================== */
#define RGBH(x) RGB(((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255)
static COLORREF mixc(COLORREF a, COLORREF b, double t) {
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t),
               (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}
static COLORREF shadec(COLORREF c, double f) {
    int r = (int)(GetRValue(c) * f), g = (int)(GetGValue(c) * f), b = (int)(GetBValue(c) * f);
    return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}
static double lumc(COLORREF c) { return 0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c); }
static COLORREF contrast(COLORREF bg) { return lumc(bg) < 140 ? RGB(255, 255, 255) : RGB(0, 0, 0); }
static COLORREF hex_color(const WCHAR *t, COLORREF def) {
    if (!t) return def; if (*t == '#') t++;
    if (lstrlenW(t) < 6) return def;
    WCHAR *e; WCHAR b[7]; lstrcpynW(b, t, 7); u32 v = (u32)wcstoul(b, &e, 16);
    if (*e) return def; return RGBH(v);
}

/* ========================================================================== */
/* CONFIG (JSON, shares file format with the Python version)                  */
/* ========================================================================== */
typedef struct { WCHAR k[64]; WCHAR *raw; } Extra;
typedef struct {
    WCHAR theme[16], accent[16], font[64], view[16], sort[16], last_dir[MAX_PATH];
    WCHAR os_style[16], style_accent[16], sort_dirs[8];
    int font_size, icon_size, preview_width, search_limit, window_w, window_h, window_x, window_y, session_cur;
    double scale;
    int show_hidden, show_preview, asc, restore_session, style_dark, style_fonts, search_subfolders;
    int autostart_enabled, run_at_login, thumbnails;
    SL favorites, pins, recent_folders, recent_files, session_tabs, pinned_tabs, autostart_apps;
    Extra *extra; int nextra;
} Cfg;
static Cfg C;
static WCHAR CONFIG_DIR[MAX_PATH], CONFIG_PATH[MAX_PATH];

enum { T_STR, T_INT, T_BOOL, T_DBL, T_LIST };
typedef struct { const WCHAR *key; int type; size_t off; int cap; } Field;
#define FS(k, m) { L##k, T_STR, offsetof(Cfg, m), (int)(sizeof(((Cfg *)0)->m) / sizeof(WCHAR)) }
#define FI(k, m) { L##k, T_INT, offsetof(Cfg, m), 0 }
#define FB(k, m) { L##k, T_BOOL, offsetof(Cfg, m), 0 }
#define FD(k, m) { L##k, T_DBL, offsetof(Cfg, m), 0 }
#define FL(k, m) { L##k, T_LIST, offsetof(Cfg, m), 0 }
static const Field FIELDS[] = {
    FS("theme", theme), FS("accent", accent), FS("font", font), FI("font_size", font_size), FD("scale", scale),
    FS("view", view), FB("show_hidden", show_hidden), FI("icon_size", icon_size), FB("show_preview", show_preview),
    FI("preview_width", preview_width), FL("favorites", favorites), FL("pins", pins), FS("sort", sort),
    FB("asc", asc), FS("last_dir", last_dir), FI("window_w", window_w), FI("window_h", window_h),
    FI("window_x", window_x), FI("window_y", window_y), FL("recent_folders", recent_folders),
    FL("recent_files", recent_files), FB("restore_session", restore_session), FL("session_tabs", session_tabs),
    FI("session_cur", session_cur), FL("pinned_tabs", pinned_tabs), FS("os_style", os_style),
    FB("style_dark", style_dark), FS("style_accent", style_accent), FB("style_fonts", style_fonts),
    FB("search_subfolders", search_subfolders), FI("search_limit", search_limit),
    FB("autostart_enabled", autostart_enabled), FL("autostart_apps", autostart_apps),
    FB("run_at_login", run_at_login), FB("thumbnails", thumbnails), FS("sort_dirs", sort_dirs),
};
#define NFIELDS CAP(FIELDS)

static void cfg_defaults(void) {
    for (int i = 0; i < C.nextra; i++) free(C.extra[i].raw);
    free(C.extra);
    SL *lists[] = { &C.favorites, &C.pins, &C.recent_folders, &C.recent_files, &C.session_tabs, &C.pinned_tabs, &C.autostart_apps };
    for (int i = 0; i < 7; i++) sl_clear(lists[i]);
    Cfg z; memset(&z, 0, sizeof z);
    z.favorites = C.favorites; z.pins = C.pins; z.recent_folders = C.recent_folders; z.recent_files = C.recent_files;
    z.session_tabs = C.session_tabs; z.pinned_tabs = C.pinned_tabs; z.autostart_apps = C.autostart_apps;
    C = z;
    scpy(C.theme, 16, L"dark"); scpy(C.accent, 16, L"#0078D4"); scpy(C.font, 64, L"Segoe UI");
    C.font_size = 14; C.scale = 1.0; scpy(C.view, 16, L"details"); C.icon_size = 64;
    C.show_preview = 1; C.preview_width = 280; scpy(C.sort, 16, L"name"); C.asc = 1;
    C.window_w = 1180; C.window_h = 720; C.window_x = UNSET; C.window_y = UNSET;
    C.restore_session = 1; scpy(C.os_style, 16, L"win10"); C.style_fonts = 1; C.search_subfolders = 1;
    C.search_limit = 20000; C.thumbnails = 1; scpy(C.sort_dirs, 8, L"first");
}

/* --- tiny JSON reader (operates on UTF-16) --- */
static const WCHAR *js_ws(const WCHAR *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static const WCHAR *js_str(const WCHAR *p, WCHAR *out, int cap) {
    int n = 0; p++;
    while (*p && *p != '"') {
        WCHAR c = *p++;
        if (c == '\\') {
            c = *p++;
            switch (c) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case 'u': { unsigned v = 0; for (int i = 0; i < 4 && *p; i++) { WCHAR h = *p++;
                        v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10); } c = (WCHAR)v; } break;
            default: break;
            }
        }
        if (out && n < cap - 1) out[n++] = c;
    }
    if (*p == '"') p++;
    if (out) out[n] = 0;
    return p;
}
static const WCHAR *js_skip(const WCHAR *p) {
    p = js_ws(p);
    if (*p == '"') return js_str(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int d = 0;
        do {
            if (*p == '"') { p = js_str(p, NULL, 0); continue; }
            if (*p == '{' || *p == '[') d++; else if (*p == '}' || *p == ']') d--;
            p++;
        } while (*p && d > 0);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}
static const WCHAR *js_num(const WCHAR *p, double *v, int *isnull) {
    p = js_ws(p); *isnull = 0; *v = 0;
    if (!wcsncmp(p, L"true", 4)) { *v = 1; return p + 4; }
    if (!wcsncmp(p, L"false", 5)) { return p + 5; }
    if (!wcsncmp(p, L"null", 4)) { *isnull = 1; return p + 4; }
    WCHAR *e; double d = wcstod(p, &e);
    if (e == p) { *isnull = 1; return js_skip(p); }
    *v = d; return e;
}
static void cfg_parse(const WCHAR *p) {
    p = js_ws(p); if (*p != '{') return; p++;
    for (;;) {
        p = js_ws(p);
        if (*p == '}' || !*p) break;
        if (*p == ',') { p++; continue; }
        if (*p != '"') break;
        WCHAR key[64]; p = js_str(p, key, 64); p = js_ws(p); if (*p == ':') p++; p = js_ws(p);
        const Field *f = NULL;
        for (int i = 0; i < (int)NFIELDS; i++) if (!wcscmp(FIELDS[i].key, key)) { f = &FIELDS[i]; break; }
        if (!f) {
            const WCHAR *e = js_skip(p);
            C.extra = (Extra *)realloc(C.extra, (C.nextra + 1) * sizeof(Extra));
            Extra *x = &C.extra[C.nextra++]; scpy(x->k, 64, key);
            int n = (int)(e - p); x->raw = (WCHAR *)malloc((n + 1) * sizeof(WCHAR)); memcpy(x->raw, p, n * sizeof(WCHAR)); x->raw[n] = 0;
            p = e; continue;
        }
        u8 *base = (u8 *)&C + f->off;
        if (f->type == T_STR) {
            if (*p == '"') p = js_str(p, (WCHAR *)base, f->cap); else p = js_skip(p);
        } else if (f->type == T_LIST) {
            SL *l = (SL *)base;
            if (*p == '[') {
                sl_clear(l); p++;
                for (;;) {
                    p = js_ws(p);
                    if (*p == ']' || !*p) { if (*p) p++; break; }
                    if (*p == ',') { p++; continue; }
                    if (*p == '"') { WCHAR t[MAX_PATH]; p = js_str(p, t, MAX_PATH); sl_add(l, t); }
                    else p = js_skip(p);
                }
            } else p = js_skip(p);
        } else {
            double v; int nl; p = js_num(p, &v, &nl);
            if (!nl) {
                if (f->type == T_DBL) *(double *)base = v; else *(int *)base = (int)v;
            }
        }
    }
}
static WCHAR *read_text_file(const WCHAR *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), rd = 0;
    if (sz == INVALID_FILE_SIZE || sz > 64u << 20) { CloseHandle(h); return NULL; }
    char *buf = (char *)malloc(sz + 1); ReadFile(h, buf, sz, &rd, NULL); CloseHandle(h);
    char *s = buf; int n = (int)rd;
    if (n >= 3 && (u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) { s += 3; n -= 3; }
    int w = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    WCHAR *out = (WCHAR *)malloc((w + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, n, out, w); out[w] = 0; free(buf); return out;
}
static int write_utf8_file(const WCHAR *path, const WCHAR *w, int n) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL);
    char *b = (char *)malloc(len + 1); WideCharToMultiByte(CP_UTF8, 0, w, n, b, len, NULL, NULL);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = 0;
    if (h != INVALID_HANDLE_VALUE) { DWORD wr; ok = WriteFile(h, b, len, &wr, NULL) && wr == (DWORD)len; CloseHandle(h); }
    free(b); return ok;
}
static void cfg_validate(void);
static void cfg_save(void) {
    SB b = { 0 }; sb_add(&b, L"{\n");
    for (int i = 0; i < (int)NFIELDS; i++) {
        const Field *f = &FIELDS[i]; u8 *base = (u8 *)&C + f->off;
        sb_add(&b, L"  "); sb_jstr(&b, f->key); sb_add(&b, L": ");
        if (f->type == T_STR) sb_jstr(&b, (WCHAR *)base);
        else if (f->type == T_BOOL) sb_add(&b, *(int *)base ? L"true" : L"false");
        else if (f->type == T_INT) { int v = *(int *)base; if (v == UNSET) sb_add(&b, L"null"); else sb_addf(&b, L"%d", v); }
        else if (f->type == T_DBL) sb_addf(&b, L"%g", *(double *)base);
        else {
            SL *l = (SL *)base;
            if (!l->n) sb_add(&b, L"[]");
            else { sb_add(&b, L"[\n"); for (int k = 0; k < l->n; k++) { sb_add(&b, L"    "); sb_jstr(&b, l->v[k]); sb_add(&b, k + 1 < l->n ? L",\n" : L"\n"); } sb_add(&b, L"  ]"); }
        }
        sb_add(&b, (i + 1 < (int)NFIELDS || C.nextra) ? L",\n" : L"\n");
    }
    for (int i = 0; i < C.nextra; i++) {
        sb_add(&b, L"  "); sb_jstr(&b, C.extra[i].k); sb_add(&b, L": "); sb_add(&b, C.extra[i].raw);
        sb_add(&b, i + 1 < C.nextra ? L",\n" : L"\n");
    }
    sb_add(&b, L"}\n");
    if (!write_utf8_file(CONFIG_PATH, b.s, b.n)) LOG("cfg save failed");
    free(b.s);
}
static void cfg_init_paths(void) {
    WCHAR pd[MAX_PATH];
    if (!GetEnvironmentVariableW(L"PROGRAMDATA", pd, MAX_PATH)) lstrcpyW(pd, L"C:\\ProgramData");
    wfmt(CONFIG_DIR, MAX_PATH, L"%ls\\WinShell", pd);
    wfmt(CONFIG_PATH, MAX_PATH, L"%ls\\fm_config.json", CONFIG_DIR);
}
static void cfg_load(void) {
    cfg_defaults();
    SHCreateDirectoryExW(NULL, CONFIG_DIR, NULL);
    WCHAR *t = read_text_file(CONFIG_PATH);
    if (t) { cfg_parse(t); free(t); }
    cfg_validate();
    cfg_save();
}

/* ========================================================================== */
/* THEMES / PALETTE / OS STYLES                                               */
/* ========================================================================== */
typedef struct { const WCHAR *name; u32 bg, alt; int dark; } Theme;
static const Theme THEMES[] = {
    { L"light", 0xFFFFFF, 0xF3F3F3, 0 }, { L"dark", 0x202020, 0x2B2B2B, 1 },
    { L"amoled", 0x000000, 0x101010, 1 }, { L"midnight", 0x121826, 0x1B2438, 1 },
    { L"ocean", 0x0F2837, 0x17384B, 1 }, { L"nord", 0x2E3440, 0x3B4252, 1 },
};
#define NTHEMES CAP(THEMES)
static const Theme *theme_by_name(const WCHAR *n) {
    for (int i = 0; i < (int)NTHEMES; i++) if (wieq(THEMES[i].name, n)) return &THEMES[i];
    return &THEMES[1];
}
static const WCHAR *ACCENTS[] = { L"#0078D4", L"#15A6F0", L"#E95420", L"#8FA876", L"#3498DB", L"#0A64DC", L"#E81123", L"#8E5BD8" };

typedef struct {
    COLORREF dark, bg, alt, text, dim, line, accent, border, hover, sel, cap, cap_text, input, input_border;
    COLORREF btn, btn_press, btn_border, sb_track, sb_thumb, side, tool, status, cap2, nav_sel, nav_sel_text;
    COLORREF sel_text, zebra, hdr_bg, hdr;
} Pal;

static void palette_core(Pal *p, COLORREF bg, COLORREF alt, COLORREF text, int dark, COLORREF acc) {
    COLORREF black = RGB(0, 0, 0);
    memset(p, 0, sizeof *p);
    p->dark = dark; p->bg = bg; p->alt = alt; p->text = text; p->accent = acc;
    p->dim = mixc(text, bg, 0.42); p->line = mixc(bg, text, 0.16); p->border = mixc(bg, text, 0.28);
    if (dark) {
        p->hover = mixc(bg, text, 0.14); p->sel = mixc(bg, acc, 0.42); p->cap = mixc(bg, black, 0.10);
        p->input = mixc(bg, text, 0.08); p->input_border = mixc(bg, text, 0.30);
        p->btn = mixc(bg, text, 0.12); p->btn_border = mixc(bg, text, 0.34);
        p->sb_track = mixc(bg, black, 0.35); p->sb_thumb = mixc(bg, text, 0.28);
    } else {
        p->hover = mixc(bg, acc, 0.10); p->sel = mixc(bg, acc, 0.20); p->cap = bg; p->input = bg;
        p->input_border = mixc(bg, text, 0.24); p->btn = mixc(bg, black, 0.04); p->btn_border = mixc(bg, text, 0.24);
        p->sb_track = mixc(bg, black, 0.06); p->sb_thumb = mixc(bg, black, 0.25);
    }
    p->cap_text = contrast(p->cap); p->btn_press = mixc(bg, acc, 0.35); p->hdr = p->dim;
    p->side = alt; p->tool = bg; p->status = alt; p->cap2 = p->cap; p->nav_sel = p->sel;
    p->nav_sel_text = text; p->sel_text = text; p->zebra = bg; p->hdr_bg = bg;
}

/* colour table index: 0 bg,1 side,2 text,3 dim,4 line,5 cap,6 cap2,7 cap_text,8 tool,9 status,
   10 input,11 input_b,12 hover,13 sel,14 nav_sel,15 accent,16 zebra,17 hdr_bg */
enum { LAY_RIBBON, LAY_WIN7, LAY_HEADER, LAY_TOOLBAR, LAY_MENUBAR };
typedef struct {
    const WCHAR *key, *label, *info;
    int layout, cap_h_fm, menubar_h, tool_h, field_h, tb_btn, tb_gap, search_w, nav_w, nav_row, row_h, hdr_h, status_h, sb_w;
    int sel_solid, zebra, round, hdr_raised, nav_tree, base_dark;
    const WCHAR *font[4];
    const WCHAR *sec[4];                 /* home, recent, fav, pc */
    u32 folder;                          /* folder icon colour */
    u32 light[18]; u32 dark[18]; int has_dark;
} Style;

#define PAL_(...) { __VA_ARGS__ }
static const Style STYLES[] = {
 { L"win10", L"Windows 10", L"Command bar - flat - Segoe UI", LAY_RIBBON, 32, 0, 40, 28, 34, 4, 240, 220, 24, 22, 26, 24, 17,
   0, 0, 0, 0, 1, 0, { L"Segoe UI" }, { L"Quick Access", L"Recent", L"Favorites", L"This PC" }, 0xF8D775,
   PAL_(0), PAL_(0), 1 },
 { L"win11", L"Windows 11", L"Fluent - rounded - Mica", LAY_TOOLBAR, 36, 0, 48, 32, 34, 4, 250, 230, 32, 30, 32, 28, 12,
   0, 0, 8, 0, 1, 0, { L"Segoe UI Variable Text", L"Segoe UI" }, { L"Home", L"Recent", L"Favorites", L"This PC" }, 0xFFCE4A,
   PAL_(0xFFFFFF,0xF3F3F3,0x1A1A1A,0x616161,0xE5E5E5,0xF3F3F3,0xF3F3F3,0x1A1A1A,0xFFFFFF,0xF9F9F9,0xFFFFFF,0xD0D0D0,0xEAEAEA,0xCCE4F7,0xE4E4E4,0x005FB8,0xFFFFFF,0xFFFFFF),
   PAL_(0x202020,0x2B2B2B,0xFFFFFF,0xB3B3B3,0x3A3A3A,0x2B2B2B,0x2B2B2B,0xFFFFFF,0x202020,0x272727,0x2D2D2D,0x5A5A5A,0x383838,0x2E4A63,0x3A3A3A,0x4CC2FF,0x202020,0x202020), 1 },
 { L"win7", L"Windows 7", L"Aero glass - command bar - details pane", LAY_WIN7, 30, 0, 40, 26, 28, 4, 210, 190, 22, 21, 24, 52, 17,
   0, 0, 3, 1, 1, 0, { L"Segoe UI" }, { L"Favorites", L"Recent Places", L"Libraries", L"Computer" }, 0xF0CF6A,
   PAL_(0xFFFFFF,0xFFFFFF,0x000000,0x6D6D6D,0xD9E1EC,0xBBD1EC,0xE1ECF9,0x000000,0xF1F5FB,0xF1F5FA,0xFFFFFF,0x8E9BB0,0xE6F1FD,0xCCE4FA,0xD3E6FA,0x3399FF,0xFFFFFF,0xF6F9FD),
   PAL_(0), 0 },
 { L"macos", L"macOS", L"Finder - unified toolbar", LAY_HEADER, 52, 0, 0, 28, 30, 2, 190, 200, 26, 24, 24, 28, 12,
   1, 1, 6, 0, 0, 0, { L"SF Pro Text", L"Helvetica Neue", L"Segoe UI" }, { L"Favorites", L"Recents", L"Pinned", L"Locations" }, 0x5AB8F5,
   PAL_(0xFFFFFF,0xE8E6E8,0x1D1D1F,0x86868B,0xD8D6D8,0xECEAEC,0xDEDCDE,0x3C3C43,0xECEAEC,0xF7F6F7,0xF7F6F7,0xCFCDCF,0xEDEDEF,0x0A64DC,0xCFCDD1,0x0A64DC,0xF5F5F7,0xFFFFFF),
   PAL_(0x1E1E1E,0x2A2829,0xF5F5F7,0x98989D,0x3A383A,0x3A383B,0x2F2D30,0xEDEDED,0x3A383B,0x262426,0x2C2A2D,0x4A484B,0x2E2E30,0x0A64DC,0x4A484B,0x0A64DC,0x252525,0x1E1E1E), 1 },
 { L"ubuntu", L"Ubuntu", L"Nautilus - Yaru header bar", LAY_HEADER, 48, 0, 0, 32, 32, 2, 190, 220, 30, 28, 28, 24, 12,
   1, 0, 6, 0, 0, 0, { L"Ubuntu", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Devices" }, 0xE99A5C,
   PAL_(0xFFFFFF,0xF6F5F4,0x333333,0x7E7E7E,0xD5D3CF,0xEBEBEA,0xE1E0DF,0x333333,0xEBEBEA,0xF6F5F4,0xFFFFFF,0xCDC7C2,0xEDEBEA,0xE95420,0xE95420,0xE95420,0xFFFFFF,0xF6F5F4),
   PAL_(0x2B2B2B,0x323232,0xEEEEEC,0xA0A0A0,0x1E1E1E,0x262626,0x202020,0xEEEEEC,0x262626,0x303030,0x1F1F1F,0x4A4A4A,0x3A3A3A,0xE95420,0xE95420,0xE95420,0x2B2B2B,0x303030), 1 },
 { L"zorin", L"Zorin OS", L"Nautilus - rounded - Zorin blue", LAY_HEADER, 50, 0, 0, 32, 32, 2, 190, 220, 32, 30, 28, 26, 12,
   1, 0, 9, 0, 0, 0, { L"Inter", L"Cantarell", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Devices" }, 0x4DB2F2,
   PAL_(0xFAFAFA,0xF1F1F1,0x2B2B2B,0x7A7A7A,0xDEDEDE,0xF4F4F4,0xECECEC,0x2B2B2B,0xF4F4F4,0xF1F1F1,0xFFFFFF,0xD4D4D4,0xE9E9E9,0x15A6F0,0x15A6F0,0x15A6F0,0xFAFAFA,0xF4F4F4),
   PAL_(0x222222,0x1B1B1B,0xEDEDED,0x9A9A9A,0x333333,0x2B2B2B,0x262626,0xEDEDED,0x2B2B2B,0x1E1E1E,0x1A1A1A,0x444444,0x2E2E2E,0x15A6F0,0x15A6F0,0x15A6F0,0x222222,0x262626), 1 },
 { L"mint", L"Linux Mint", L"Nemo - Mint-Y - green toolbar", LAY_TOOLBAR, 30, 0, 42, 28, 32, 2, 180, 200, 26, 24, 26, 26, 14,
   1, 0, 3, 1, 0, 0, { L"Noto Sans", L"DejaVu Sans", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Devices" }, 0x9DB87F,
   PAL_(0xFFFFFF,0xF5F5F5,0x2F2F2F,0x7A7A7A,0xD8D8D8,0xE6E6E6,0xD9D9D9,0x2F2F2F,0xEFEFEF,0xEFEFEF,0xFFFFFF,0xC5C5C5,0xE4E4E4,0x8FA876,0x8FA876,0x8FA876,0xFFFFFF,0xF1F1F1),
   PAL_(0x2F2F2F,0x272727,0xEBEBEB,0x9B9B9B,0x1F1F1F,0x202020,0x1A1A1A,0xEBEBEB,0x343434,0x2A2A2A,0x262626,0x4D4D4D,0x3B3B3B,0x8FA876,0x8FA876,0x8FA876,0x2F2F2F,0x333333), 1 },
 { L"kde", L"KDE Dolphin", L"Breeze - Plasma - places panel", LAY_MENUBAR, 28, 24, 40, 28, 30, 2, 190, 200, 26, 24, 24, 26, 14,
   1, 1, 4, 0, 0, 0, { L"Noto Sans", L"Segoe UI" }, { L"Places", L"Recent", L"Favorites", L"Devices" }, 0x3DAEE9,
   PAL_(0xFCFCFC,0xEFF0F1,0x232627,0x7F8C8D,0xBDC3C7,0xE3E5E7,0xD5D8DB,0x232627,0xEFF0F1,0xEFF0F1,0xFCFCFC,0xBDC3C7,0xDDEFFA,0x3DAEE9,0x3DAEE9,0x3DAEE9,0xF3F4F5,0xEFF0F1),
   PAL_(0x31363B,0x232629,0xEFF0F1,0x7F8C8D,0x4D4D4D,0x2A2E32,0x232629,0xEFF0F1,0x31363B,0x232629,0x232629,0x4D4D4D,0x3B4045,0x3DAEE9,0x3DAEE9,0x3DAEE9,0x2C3035,0x2A2E32), 1 },
 { L"chromeos", L"ChromeOS Files", L"Material - pill search", LAY_HEADER, 52, 0, 0, 36, 34, 2, 240, 230, 36, 34, 32, 28, 12,
   0, 0, 18, 0, 0, 0, { L"Roboto", L"Segoe UI" }, { L"My files", L"Recent", L"Starred", L"Drives" }, 0x5F6368,
   PAL_(0xFFFFFF,0xF8F9FA,0x202124,0x5F6368,0xDADCE0,0xF1F3F4,0xE8EAED,0x202124,0xF8F9FA,0xF8F9FA,0xF1F3F4,0xF1F3F4,0xF1F3F4,0xD2E3FC,0xD2E3FC,0x1A73E8,0xFFFFFF,0xFFFFFF),
   PAL_(0x202124,0x292A2D,0xE8EAED,0x9AA0A6,0x3C4043,0x292A2D,0x202124,0xE8EAED,0x292A2D,0x292A2D,0x3C4043,0x3C4043,0x35363A,0x394457,0x394457,0x8AB4F8,0x202124,0x292A2D), 1 },
 { L"xfce", L"XFCE", L"Thunar - Greybird - menu bar", LAY_MENUBAR, 26, 24, 38, 26, 30, 2, 170, 180, 24, 22, 24, 24, 14,
   1, 0, 2, 1, 0, 0, { L"DejaVu Sans", L"Cantarell", L"Segoe UI" }, { L"Places", L"Recent", L"Bookmarks", L"Devices" }, 0x6FA8DC,
   PAL_(0xFFFFFF,0xFAFAFA,0x3C3C3C,0x7C7C7B,0xCECECD,0xE3E2E1,0xCFCECD,0x3C3C3C,0xEDECEB,0xEDECEB,0xFFFFFF,0xABABAA,0xDDDCDB,0x3498DB,0x3498DB,0x3498DB,0xFFFFFF,0xEDECEB),
   PAL_(0x333333,0x2A2A2A,0xE6E6E6,0x9A9A9A,0x1E1E1E,0x3C3C3C,0x2E2E2E,0xE6E6E6,0x3A3A3A,0x343434,0x2A2A2A,0x555555,0x454545,0x3498DB,0x3498DB,0x3498DB,0x333333,0x3A3A3A), 1 },
 { L"total", L"Total Commander", L"Classic Win95 - navy - raised", LAY_MENUBAR, 22, 20, 34, 22, 26, 2, 170, 170, 18, 18, 20, 20, 16,
   1, 0, 0, 1, 1, 0, { L"MS Sans Serif", L"Tahoma", L"Segoe UI" }, { L"Favorites", L"Recent", L"Pinned", L"Drives" }, 0xFFFF55,
   PAL_(0xFFFFFF,0xC0C0C0,0x000000,0x404040,0x808080,0x000080,0x1084D0,0xFFFFFF,0xC0C0C0,0xC0C0C0,0xFFFFFF,0x808080,0xD4D0C8,0x000080,0x000080,0x000080,0xFFFFFF,0xC0C0C0),
   PAL_(0), 0 },
 { L"mc", L"Midnight Commander", L"Text-mode - blue - monospace", LAY_MENUBAR, 22, 22, 34, 22, 26, 2, 170, 180, 18, 18, 20, 20, 14,
   1, 0, 0, 0, 0, 1, { L"Consolas", L"Courier New", L"DejaVu Sans Mono", L"Segoe UI" }, { L"Panel", L"History", L"Hotlist", L"Drives" }, 0x55FFFF,
   PAL_(0x0000AA,0x0000AA,0xE8E8E8,0xFFFF55,0xAAAAAA,0x00AAAA,0x00AAAA,0x000000,0x00AAAA,0x00AAAA,0x000088,0x55FFFF,0x0000CC,0x00AAAA,0x00AAAA,0x00AAAA,0x0000AA,0x0000AA),
   PAL_(0), 0 },
};
#define NSTYLES CAP(STYLES)
/* order shown in the Styles menu (indices into STYLES) */
static const int STYLE_ORDER[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static int style_index(const WCHAR *k) {
    for (int i = 0; i < (int)NSTYLES; i++) if (!wcscmp(STYLES[i].key, k)) return i;
    return 0;
}

static void cfg_validate(void) {
    if (wcscmp(C.view, L"details") && wcscmp(C.view, L"icons") && wcscmp(C.view, L"tiles")) scpy(C.view, 16, L"details");
    if (wcscmp(C.sort, L"name") && wcscmp(C.sort, L"date") && wcscmp(C.sort, L"type") && wcscmp(C.sort, L"size")) scpy(C.sort, 16, L"name");
    if (wcscmp(C.sort_dirs, L"first") && wcscmp(C.sort_dirs, L"last") && wcscmp(C.sort_dirs, L"mixed")) scpy(C.sort_dirs, 8, L"first");
    scpy(C.os_style, 16, STYLES[style_index(C.os_style)].key);
    scpy(C.theme, 16, theme_by_name(C.theme)->name);
    if (C.preview_width < 220) C.preview_width = 220;
    if (C.preview_width > 900) C.preview_width = 900;
    if (C.search_limit < 100) C.search_limit = 100;
    if (C.font_size < 8) C.font_size = 8;
    if (C.font_size > 40) C.font_size = 40;
    if (C.icon_size < 32) C.icon_size = 32;
    if (C.icon_size > 160) C.icon_size = 160;
    if (C.scale < 0.5 || C.scale > 3.0) C.scale = 1.0;
    if (C.window_w < 640) C.window_w = 1180;
    if (C.window_h < 420) C.window_h = 720;
    if (ON_WINE && !wcscmp(C.theme, L"light")) { LOG("Migrating light->dark on Wine"); scpy(C.theme, 16, L"dark"); }
}

static const Style *cur_style(void) { return &STYLES[style_index(C.os_style)]; }
static int is_win10(void) { return !wcscmp(C.os_style, L"win10"); }
static int is_dark(void) {
    if (is_win10()) return theme_by_name(C.theme)->dark;
    return C.style_dark && cur_style()->has_dark;
}
static void build_pal(Pal *p) {
    const Style *st = cur_style();
    if (is_win10()) {
        const Theme *t = theme_by_name(C.theme);
        COLORREF acc = hex_color(C.accent, RGB(0, 120, 212));
        palette_core(p, RGBH(t->bg), RGBH(t->alt), t->dark ? RGB(255, 255, 255) : RGB(0, 0, 0), t->dark, acc);
        return;
    }
    int dk = C.style_dark && st->has_dark;
    const u32 *v = dk ? st->dark : st->light;
    int isdark = dk || st->base_dark;
    COLORREF acc = RGBH(v[15]);
    if (C.style_accent[0]) acc = hex_color(C.style_accent, acc);
    palette_core(p, RGBH(v[0]), RGBH(v[1]), RGBH(v[2]), isdark, acc);
    p->dim = RGBH(v[3]); p->line = RGBH(v[4]); p->border = mixc(RGBH(v[4]), RGBH(v[2]), 0.12);
    p->hover = RGBH(v[12]); p->cap = RGBH(v[5]); p->cap2 = RGBH(v[6]); p->cap_text = RGBH(v[7]);
    p->tool = RGBH(v[8]); p->status = RGBH(v[9]); p->input = RGBH(v[10]); p->input_border = RGBH(v[11]);
    p->zebra = RGBH(v[16]); p->hdr_bg = RGBH(v[17]); p->hdr = p->dim; p->side = RGBH(v[1]);
    p->btn = mixc(p->tool, RGBH(v[2]), 0.05); p->btn_border = mixc(p->tool, RGBH(v[2]), 0.25);
    p->btn_press = mixc(RGBH(v[0]), acc, 0.35);
    if (st->sel_solid) { p->sel = acc; p->nav_sel = !wcscmp(st->key, L"macos") ? RGBH(v[14]) : acc; }
    else if (!C.style_accent[0]) { p->sel = RGBH(v[13]); p->nav_sel = RGBH(v[14]); }
    else { p->sel = mixc(RGBH(v[0]), acc, 0.22); p->nav_sel = mixc(RGBH(v[0]), acc, 0.18); }
    p->sel_text = st->sel_solid ? contrast(p->sel) : p->text;
    p->nav_sel_text = st->sel_solid ? contrast(p->nav_sel) : p->text;
}

/* ========================================================================== */
/* GDI helpers                                                                */
/* ========================================================================== */
static int px(double v) { int r = (int)(v * C.scale + 0.5); return r < 1 ? 1 : r; }
static void fillr(HDC dc, const RECT *r, COLORREF c) {
    SetBkColor(dc, c); ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}
static void fill4(HDC dc, int l, int t, int r, int b, COLORREF c) { RECT q = { l, t, r, b }; fillr(dc, &q, c); }
static void framer(HDC dc, const RECT *r, COLORREF c) {
    fill4(dc, r->left, r->top, r->right, r->top + 1, c); fill4(dc, r->left, r->bottom - 1, r->right, r->bottom, c);
    fill4(dc, r->left, r->top, r->left + 1, r->bottom, c); fill4(dc, r->right - 1, r->top, r->right, r->bottom, c);
}
static void rrect(HDC dc, const RECT *r, COLORREF fillc, COLORREF outline, int rad, int use_outline) {
    HBRUSH b = CreateSolidBrush(fillc); HPEN p = use_outline ? CreatePen(PS_SOLID, 1, outline) : (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    if (rad > 1) RoundRect(dc, r->left, r->top, r->right + 1, r->bottom + 1, rad * 2, rad * 2);
    else Rectangle(dc, r->left, r->top, r->right + 1, r->bottom + 1);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); if (use_outline) DeleteObject(p);
}
static void linec(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int w) {
    HPEN p = CreatePen(PS_SOLID, w, c); HGDIOBJ o = SelectObject(dc, p);
    MoveToEx(dc, x1, y1, NULL); LineTo(dc, x2, y2); SelectObject(dc, o); DeleteObject(p);
}
static void poly(HDC dc, const POINT *pt, int n, COLORREF f) {
    HBRUSH b = CreateSolidBrush(f); HPEN p = CreatePen(PS_SOLID, 1, f);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p); Polygon(dc, pt, n);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}
static void ellipsec(HDC dc, const RECT *r, COLORREF f, COLORREF o, int hasf) {
    HBRUSH b = hasf ? CreateSolidBrush(f) : (HBRUSH)GetStockObject(NULL_BRUSH); HPEN p = CreatePen(PS_SOLID, 1, o);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, r->left, r->top, r->right, r->bottom);
    SelectObject(dc, ob); SelectObject(dc, op); if (hasf) DeleteObject(b); DeleteObject(p);
}
static void textr(HDC dc, const WCHAR *s, const RECT *r, UINT fl, COLORREF c) {
    RECT q = *r; SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &q, fl | DT_NOPREFIX);
}
#define TL (DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_LEFT)
#define TC (DT_SINGLELINE | DT_VCENTER | DT_CENTER)
static int text_w(HDC dc, const WCHAR *s) { SIZE z; GetTextExtentPoint32W(dc, s, lstrlenW(s), &z); return z.cx; }
static int in_rect(const RECT *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }
static void vgrad(HDC dc, const RECT *r, COLORREF a, COLORREF b) {
    int h = r->bottom - r->top; if (h <= 0) return;
    for (int y = 0; y < h; y += 2) fill4(dc, r->left, r->top + y, r->right, r->top + y + 2 > r->bottom ? r->bottom : r->top + y + 2, mixc(a, b, (double)y / h));
}

static void fmt_bytes(u64 n, WCHAR *o, int cap) {
    static const WCHAR *u[] = { L"B", L"KB", L"MB", L"GB", L"TB" }; double v = (double)n; int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    if (i == 0) wfmt(o, cap, L"%d B", (int)n); else wfmt(o, cap, L"%.1f %ls", v, u[i]);
}
static void fmt_date(const FILETIME *ft, WCHAR *o, int cap) {
    SYSTEMTIME u, l; if (!ft->dwHighDateTime && !ft->dwLowDateTime) { o[0] = 0; return; }
    FileTimeToSystemTime(ft, &u); SystemTimeToTzSpecificLocalTime(NULL, &u, &l);
    wfmt(o, cap, L"%04d-%02d-%02d %02d:%02d", l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute);
}
static int CALLBACK font_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD ty, LPARAM lp) { (void)lf; (void)tm; (void)ty; *(int *)lp = 1; return 0; }
static int font_available(const WCHAR *n) {
    HDC dc = GetDC(NULL); LOGFONTW lf; memset(&lf, 0, sizeof lf); lf.lfCharSet = DEFAULT_CHARSET; lstrcpynW(lf.lfFaceName, n, 32);
    int found = 0; EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)font_cb, (LPARAM)&found, 0); ReleaseDC(NULL, dc); return found;
}
static const WCHAR *pick_font(const WCHAR *const *c) {
    for (int i = 0; i < 4 && c[i]; i++) if (font_available(c[i])) return c[i];
    return L"Segoe UI";
}
static HFONT make_font(const WCHAR *n, int size, int weight) {
    return CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, n);
}

static u64 free_space(const WCHAR *path, u64 *total) {
    ULARGE_INTEGER a, t, f; WCHAR root[8]; wfmt(root, 8, L"%c:\\", path[0]);
    if (GetDiskFreeSpaceExW(root, &a, &t, &f)) { if (total) *total = t.QuadPart; return f.QuadPart; }
    if (total) *total = 0; return 0;
}

/* ========================================================================== */
/* GDI+ (dynamic flat API)  - used for thumbnails + preview                   */
/* ========================================================================== */
typedef struct { UINT v; void *cb; BOOL a, b; } GdipInit;
static struct {
    int ok; ULONG_PTR tok;
    int (WINAPI *Startup)(ULONG_PTR *, const GdipInit *, void *);
    int (WINAPI *FromFile)(const WCHAR *, void **);
    int (WINAPI *W)(void *, UINT *); int (WINAPI *H)(void *, UINT *);
    int (WINAPI *FromHDC)(HDC, void **); int (WINAPI *DelG)(void *); int (WINAPI *DelI)(void *);
    int (WINAPI *Interp)(void *, int); int (WINAPI *Draw)(void *, void *, int, int, int, int);
} G;
static void gdip_start(void) {
    HMODULE m = LoadLibraryW(L"gdiplus.dll"); if (!m) return;
#define GP(f, n) *(FARPROC *)&G.f = GetProcAddress(m, n)
    GP(Startup, "GdiplusStartup"); GP(FromFile, "GdipCreateBitmapFromFile"); GP(W, "GdipGetImageWidth"); GP(H, "GdipGetImageHeight");
    GP(FromHDC, "GdipCreateFromHDC"); GP(DelG, "GdipDeleteGraphics"); GP(DelI, "GdipDisposeImage");
    GP(Interp, "GdipSetInterpolationMode"); GP(Draw, "GdipDrawImageRectI");
    if (!G.Startup || !G.FromFile || !G.Draw || !G.FromHDC) return;
    GdipInit in = { 1, NULL, FALSE, FALSE };
    G.ok = (G.Startup(&G.tok, &in, NULL) == 0);
}
/* draw image fitted (aspect preserved) into box; returns 1 on success */
static int gdip_draw_fit(HDC dc, const WCHAR *path, int x, int y, int w, int h, int *iw, int *ih) {
    if (!G.ok) return 0;
    void *img = NULL; if (G.FromFile(path, &img) || !img) return 0;
    UINT W_ = 0, H_ = 0; G.W(img, &W_); G.H(img, &H_);
    if (!W_ || !H_) { G.DelI(img); return 0; }
    if (iw) *iw = (int)W_; if (ih) *ih = (int)H_;
    double s = (double)w / W_; if ((double)h / H_ < s) s = (double)h / H_; if (s > 1.0 && w > 200) s = 1.0;
    int dw = (int)(W_ * s), dh = (int)(H_ * s); if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    void *g = NULL; G.FromHDC(dc, &g); if (g) { G.Interp(g, 7); G.Draw(g, img, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh); G.DelG(g); }
    G.DelI(img); return 1;
}

/* thumbnail cache: HBITMAP per (path,size) */
typedef struct { WCHAR path[MAX_PATH]; int size; HBITMAP bmp; int w, h; } Thumb;
static Thumb g_thumbs[256]; static int g_thumb_next;
static int ext_in(const WCHAR *path, const WCHAR *list) {   /* list like L".png.jpg." */
    const WCHAR *e = PathFindExtensionW(path); if (!*e) return 0;
    WCHAR k[24]; wfmt(k, 24, L"%ls.", e); CharLowerW(k);
    return wcsstr(list, k) != NULL;
}
static int is_image(const WCHAR *p) { return ext_in(p, L".png.jpg.jpeg.bmp.gif.tif.tiff.ico.webp.jfif."); }
static int g_thumb_more;
static HBITMAP thumb_get(HDC dc, const WCHAR *path, int size, int *budget) {
    for (int i = 0; i < 256; i++) if (g_thumbs[i].size == size && !lstrcmpiW(g_thumbs[i].path, path)) return g_thumbs[i].bmp;
    if (!G.ok) return NULL;
    if (*budget <= 0) { g_thumb_more = 1; return NULL; }
    (*budget)--;
    Thumb *t = &g_thumbs[g_thumb_next]; g_thumb_next = (g_thumb_next + 1) % 256;
    if (t->bmp) DeleteObject(t->bmp);
    scpy(t->path, MAX_PATH, path); t->size = size; t->bmp = NULL;
    HDC m = CreateCompatibleDC(dc); HBITMAP b = CreateCompatibleBitmap(dc, size, size); HGDIOBJ ob = SelectObject(m, b);
    RECT r = { 0, 0, size, size }; fillr(m, &r, RGB(128, 128, 128));
    int ok = gdip_draw_fit(m, path, 0, 0, size, size, NULL, NULL);
    SelectObject(m, ob); DeleteDC(m);
    if (!ok) { DeleteObject(b); b = NULL; }
    t->bmp = b; return b;
}

/* ========================================================================== */
/* icons                                                                      */
/* ========================================================================== */
typedef struct { WCHAR key[MAX_PATH + 4]; HICON ic; } IconEnt;
static IconEnt g_icons[700]; static int g_icon_next;
static HICON icon_for(const WCHAR *path, int large, int is_dir) {
    const WCHAR *e = PathFindExtensionW(path); WCHAR key[MAX_PATH + 4];
    int per_file = ext_in(path, L".exe.ico.lnk.scr.cur.ani.");
    if (per_file) wfmt(key, MAX_PATH + 4, L"%c|%ls", large ? 'L' : 'S', path);
    else wfmt(key, MAX_PATH + 4, L"%c|%ls", large ? 'L' : 'S', e);
    CharLowerW(key);
    for (int i = 0; i < 700; i++) if (g_icons[i].ic && !wcscmp(g_icons[i].key, key)) return g_icons[i].ic;
    SHFILEINFOW sfi; memset(&sfi, 0, sizeof sfi);
    UINT fl = SHGFI_ICON | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (!per_file) fl |= SHGFI_USEFILEATTRIBUTES;
    SHGetFileInfoW(per_file ? path : (e[0] ? e : L"file"), is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi, fl);
    IconEnt *g = &g_icons[g_icon_next]; g_icon_next = (g_icon_next + 1) % 700;
    if (g->ic) DestroyIcon(g->ic);
    scpy(g->key, MAX_PATH + 4, key); g->ic = sfi.hIcon;
    return g->ic;
}
static void paint_folder(HDC dc, int x, int y, int s, u32 col) {
    COLORREF c = RGBH(col), back = shadec(c, 0.82), hi = shadec(c, 1.12);
    int tw = s * 4 / 10, th = s / 8 + 1, top = y + s / 8, bh = s * 3 / 4;
    RECT a = { x + s / 16, top, x + s / 16 + tw, top + th + 2 }; rrect(dc, &a, back, back, 2, 0);
    RECT b = { x + s / 16, top + th, x + s - s / 16, top + th + bh - th }; fillr(dc, &b, back);
    RECT f = { x + s / 16, top + th + s / 8, x + s - s / 16, top + bh }; rrect(dc, &f, c, c, 2, 0);
    fill4(dc, f.left + 1, f.top, f.right - 1, f.top + 1, hi);
}
/* generic painted glyphs for nav roots / drives */
static void paint_glyph(HDC dc, int kind, int x, int y, int s, COLORREF c) {
    HBRUSH b; RECT r;
    switch (kind) {
    case 1: { POINT p[5] = { { x + s / 2, y + 1 }, { x + s - 1, y + s / 2 }, { x + s - 3, y + s / 2 }, { x + s - 3, y + s - 2 }, { x + 3, y + s - 2 } };
              POINT q[3] = { { x + 1, y + s / 2 }, { x + s / 2, y + 1 }, { x + 3, y + s / 2 } }; (void)q; poly(dc, p, 5, c);
              fill4(dc, x + 3, y + s / 2, x + s - 3, y + s / 2 + 1, c); break; }                           /* home */
    case 2: { RECT e = { x + 1, y + 1, x + s - 1, y + s - 1 }; ellipsec(dc, &e, 0, c, 0);
              linec(dc, x + s / 2, y + s / 2, x + s / 2, y + 3, c, 1); linec(dc, x + s / 2, y + s / 2, x + s - 4, y + s / 2, c, 1); break; } /* recent */
    case 3: { POINT p[10]; for (int i = 0; i < 10; i++) { double a = -1.5708 + i * 0.62832, rr = (i & 1) ? s * 0.22 : s * 0.5;
              p[i].x = x + s / 2 + (int)(cos(a) * rr); p[i].y = y + s / 2 + 1 + (int)(sin(a) * rr); } poly(dc, p, 10, c); break; } /* star */
    case 4: { r.left = x + 1; r.top = y + 2; r.right = x + s - 1; r.bottom = y + s - 5; framer(dc, &r, c); framer(dc, &(RECT){ r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 }, c);
              fill4(dc, x + s / 2 - 2, y + s - 5, x + s / 2 + 2, y + s - 2, c); fill4(dc, x + 3, y + s - 2, x + s - 3, y + s - 1, c); break; } /* pc */
    case 5: { r.left = x + 1; r.top = y + s / 3; r.right = x + s - 1; r.bottom = y + s * 2 / 3 + 2; b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b);
              fill4(dc, x + s - 5, y + s / 2, x + s - 3, y + s / 2 + 2, RGB(90, 220, 90)); break; }                  /* drive */
    }
}

/* ========================================================================== */
/* MODEL: items, tabs, global state                                           */
/* ========================================================================== */
#define MAXTABS 16
#define WM_APP_REFRESH (WM_APP + 1)
#define WM_APP_EDITKEY (WM_APP + 2)
#define WM_APP_TASKDONE (WM_APP + 3)
#define TM_UI 1
#define TM_NAV 2
#define TM_SEARCH 3
#define ES_CLASS L"WinShellFM.Window"
#define MUTEX_NAME L"WinShellFM.Mutex"

typedef struct {
    WCHAR name[256], path[MAX_PATH], type[64];
    int dir, kind, sel; u64 size; FILETIME mt;      /* kind: 0 normal, 1 drive/special */
} Item;
typedef struct {
    WCHAR path[MAX_PATH], filter[256];
    SL hist; int hidx;
    Item *it; int n, cap; int *view; int nv;
    int scroll, focus, anchor, searching, pinned;
} Tab;
typedef struct { WCHAR label[MAX_PATH], path[MAX_PATH]; int root, glyph, sect; } NavEnt;
typedef struct { RECT r; WCHAR path[MAX_PATH]; } Crumb;
typedef struct { WCHAR path[MAX_PATH]; int kind; WCHAR *text; int tscroll; u64 size; FILETIME mt; WCHAR type[64]; int dir; } Prev;

typedef struct {
    volatile LONG cancel, done, refs; CRITICAL_SECTION cs;
    WCHAR root[MAX_PATH], q[256]; int wild, sub, limit, hidden;
    Item *res; int n, cap, taken;
} SearchCtx;

static struct {
    HWND hwnd, e_addr, e_search, e_rename;
    HFONT font, font_big, font_small; Pal pal; HBRUSH br_input;
    Tab tabs[MAXTABS]; int ntabs, cur;
    SL clip; int clip_cut;
    NavEnt *nav; int nnav, ncap_nav; DWORD nav_t; int nav_scroll; int nav_open[4];
    WCHAR toast[256]; DWORD toast_t;
    SearchCtx *search; WCHAR status_msg[160];
    int hover_item, hover_nav, hover_tab, hover_btn, noevent, addr_edit, rename_item;
    int dragsb, sb_off; Prev prev;
    Crumb crumbs[40]; int ncrumbs;
    WCHAR typebuf[32]; DWORD type_t;
    volatile LONG busy; int autostart_done;
    WNDPROC edit_old;
} S;

static Tab *T(void) { return &S.tabs[S.cur]; }
static void toast(const WCHAR *s) { scpy(S.toast, 256, s); S.toast_t = GetTickCount(); if (S.hwnd) InvalidateRect(S.hwnd, NULL, FALSE); }
static void redraw(void) { if (S.hwnd) InvalidateRect(S.hwnd, NULL, FALSE); }

/* ---- path helpers ---- */
static int is_special(const WCHAR *p) { return p[0] == ':' && p[1] == ':'; }
static int dir_exists(const WCHAR *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static int path_exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static void path_join(WCHAR *o, int cap, const WCHAR *dir, const WCHAR *name) {
    scpy(o, cap, dir); int l = lstrlenW(o); if (l && o[l - 1] != '\\') scat(o, cap, L"\\"); scat(o, cap, name);
}
static void norm_path(WCHAR *p) {
    WCHAR *s = p; while (*s == ' ' || *s == '"') s++;
    if (s != p) memmove(p, s, (lstrlenW(s) + 1) * sizeof(WCHAR));
    int l = lstrlenW(p); while (l && (p[l - 1] == ' ' || p[l - 1] == '"')) p[--l] = 0;
    for (WCHAR *c = p; *c; c++) if (*c == '/') *c = '\\';
    while (l > 3 && p[l - 1] == '\\') p[--l] = 0;
    if (l == 2 && p[1] == ':') { p[2] = '\\'; p[3] = 0; }
}
static const WCHAR *base_name(const WCHAR *p) {
    const WCHAR *b = wcsrchr(p, '\\'); return b ? b + 1 : p;
}
static void parent_of(const WCHAR *p, WCHAR *o) {
    scpy(o, MAX_PATH, p); int l = lstrlenW(o);
    while (l > 3 && o[l - 1] == '\\') o[--l] = 0;
    if (l <= 3) { o[0] = 0; return; }
    WCHAR *s = wcsrchr(o, '\\'); if (!s) { o[0] = 0; return; }
    if (s - o == 2 && o[1] == ':') s[1] = 0; else *s = 0;
}
static int under_path(const WCHAR *child, const WCHAR *parent) {
    int l = lstrlenW(parent); if (_wcsnicmp(child, parent, l)) return 0;
    return child[l] == '\\' || child[l] == 0 || (l && parent[l - 1] == '\\');
}
static void unique_name(const WCHAR *dir, const WCHAR *name, int isdir, WCHAR *out, int cap) {
    WCHAR stem[MAX_PATH], ext[MAX_PATH] = L""; scpy(stem, MAX_PATH, name);
    if (!isdir) { WCHAR *e = PathFindExtensionW(stem); if (*e) { scpy(ext, MAX_PATH, e); *e = 0; } }
    WCHAR cand[MAX_PATH]; path_join(out, cap, dir, name);
    for (int i = 1; path_exists(out) && i < 10000; i++) {
        if (i == 1) wfmt(cand, MAX_PATH, L"%ls - Copy%ls", stem, ext); else wfmt(cand, MAX_PATH, L"%ls - Copy (%d)%ls", stem, i, ext);
        path_join(out, cap, dir, cand);
    }
}
static void type_name(const WCHAR *name, int dir, WCHAR *o, int cap) {
    if (dir) { scpy(o, cap, L"File folder"); return; }
    const WCHAR *e = PathFindExtensionW(name);
    if (!*e) { scpy(o, cap, L"File"); return; }
    WCHAR t[40]; scpy(t, 40, e + 1); CharUpperW(t); wfmt(o, cap, L"%ls File", t);
}
static int hidden_name(const WCHAR *n, DWORD attr) { return n[0] == '.' || (attr & FILE_ATTRIBUTE_HIDDEN); }

/* ---- item/tab storage ---- */
static Item *tab_add(Tab *t) {
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 256; t->it = (Item *)realloc(t->it, t->cap * sizeof(Item)); }
    Item *i = &t->it[t->n++]; memset(i, 0, sizeof *i); return i;
}
static void tab_clear_items(Tab *t) { t->n = 0; t->nv = 0; t->focus = t->anchor = -1; }
static void item_from_fd(Item *i, const WCHAR *dir, const WIN32_FIND_DATAW *fd) {
    scpy(i->name, 256, fd->cFileName); path_join(i->path, MAX_PATH, dir, fd->cFileName);
    i->dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    i->size = i->dir ? 0 : (((u64)fd->nFileSizeHigh << 32) | fd->nFileSizeLow);
    i->mt = fd->ftLastWriteTime; type_name(fd->cFileName, i->dir, i->type, 64);
}

/* ---- sorting ---- */
static Tab *g_st;
static int is_pinned(const Item *it) { return sl_find(&C.pins, it->path) >= 0; }
static int cmp_view(const void *a, const void *b) {
    const Item *x = &g_st->it[*(const int *)a], *y = &g_st->it[*(const int *)b];
    int px_ = is_pinned(x), py_ = is_pinned(y); if (px_ != py_) return py_ - px_;
    if (wcscmp(C.sort_dirs, L"mixed") && x->dir != y->dir) { int d = y->dir - x->dir; return wcscmp(C.sort_dirs, L"first") ? -d : d; }
    int r = 0;
    if (!wcscmp(C.sort, L"date")) r = CompareFileTime(&x->mt, &y->mt);
    else if (!wcscmp(C.sort, L"size")) r = x->size < y->size ? -1 : x->size > y->size;
    else if (!wcscmp(C.sort, L"type")) { r = lstrcmpiW(x->type, y->type); if (!r) r = StrCmpLogicalW(x->name, y->name); }
    else r = StrCmpLogicalW(x->name, y->name);
    if (!r) r = StrCmpLogicalW(x->name, y->name);
    return C.asc ? r : -r;
}
static void rebuild_view(Tab *t) {
    free(t->view); t->view = (int *)malloc((t->n + 1) * sizeof(int)); t->nv = 0;
    for (int i = 0; i < t->n; i++) {
        if (!t->searching && t->filter[0] && !StrStrIW(t->it[i].name, t->filter)) continue;
        t->view[t->nv++] = i;
    }
    g_st = t; qsort(t->view, t->nv, sizeof(int), cmp_view);
}

/* ---- special folders ---- */
static void home_dir(WCHAR *o) {
    if (!GetEnvironmentVariableW(L"USERPROFILE", o, MAX_PATH)) lstrcpyW(o, L"C:\\users\\Public");
}
static void add_folder_item(Tab *t, const WCHAR *path, int kind) {
    if (!dir_exists(path)) return; Item *i = tab_add(t);
    scpy(i->path, MAX_PATH, path); i->dir = 1; i->kind = kind;
    const WCHAR *b = base_name(path); scpy(i->name, 256, *b ? b : path); scpy(i->type, 64, L"File folder");
    WIN32_FILE_ATTRIBUTE_DATA d; if (GetFileAttributesExW(path, GetFileExInfoStandard, &d)) i->mt = d.ftLastWriteTime;
}
static void read_special(Tab *t) {
    WCHAR h[MAX_PATH], p[MAX_PATH]; home_dir(h);
    if (!t->path[0]) {                              /* This PC: drives */
        DWORD mask = GetLogicalDrives();
        for (int k = 0; k < 26; k++) {
            if (!(mask & (1u << k))) continue;
            WCHAR root[4] = { (WCHAR)('A' + k), ':', '\\', 0 }; UINT dt = GetDriveTypeW(root);
            const WCHAR *tn = dt == DRIVE_REMOVABLE ? L"Removable Disk" : dt == DRIVE_REMOTE ? L"Network Drive" : dt == DRIVE_CDROM ? L"CD Drive" : L"Local Disk";
            Item *i = tab_add(t); scpy(i->path, MAX_PATH, root); i->dir = 1; i->kind = 1; scpy(i->type, 64, tn);
            wfmt(i->name, 256, L"%lc: (%ls)", root[0], tn);
            u64 tot = 0; free_space(root, &tot); i->size = tot;
        }
    } else if (!wcscmp(t->path, L"::quick")) {
        static const WCHAR *n[] = { L"Desktop", L"Downloads", L"Documents", L"Pictures", L"Music", L"Videos" };
        for (int k = 0; k < 6; k++) { path_join(p, MAX_PATH, h, n[k]); add_folder_item(t, p, 0); }
        for (int k = 0; k < C.favorites.n; k++) add_folder_item(t, C.favorites.v[k], 0);
    } else if (!wcscmp(t->path, L"::fav")) {
        for (int k = 0; k < C.favorites.n; k++) add_folder_item(t, C.favorites.v[k], 0);
    } else if (!wcscmp(t->path, L"::recent")) {
        for (int k = 0; k < C.recent_files.n; k++) {
            WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(C.recent_files.v[k], &fd);
            if (f == INVALID_HANDLE_VALUE) continue; FindClose(f);
            WCHAR dir[MAX_PATH]; parent_of(C.recent_files.v[k], dir); Item *i = tab_add(t); item_from_fd(i, dir, &fd);
            scpy(i->path, MAX_PATH, C.recent_files.v[k]);
        }
        for (int k = 0; k < C.recent_folders.n; k++) add_folder_item(t, C.recent_folders.v[k], 0);
    }
}
static void read_dir(Tab *t, WCHAR *err) {
    err[0] = 0; WCHAR pat[MAX_PATH]; path_join(pat, MAX_PATH, t->path, L"*");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { wfmt(err, 160, L"Cannot open folder (error %lu)", GetLastError()); return; }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!C.show_hidden && hidden_name(fd.cFileName, fd.dwFileAttributes)) continue;
        item_from_fd(tab_add(t), t->path, &fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---- recursive search (worker thread) ---- */
static void sctx_release(SearchCtx *c) {
    if (InterlockedDecrement(&c->refs) == 0) { DeleteCriticalSection(&c->cs); free(c->res); free(c); }
}
static void search_walk(SearchCtx *c, const WCHAR *dir, int depth) {
    WCHAR pat[MAX_PATH]; path_join(pat, MAX_PATH, dir, L"*");
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (c->cancel) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (!c->hidden && hidden_name(fd.cFileName, fd.dwFileAttributes)) continue;
        int isd = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int match = c->wild ? PathMatchSpecW(fd.cFileName, c->q) : (StrStrIW(fd.cFileName, c->q) != NULL);
        if (match) {
            EnterCriticalSection(&c->cs);
            if (c->n < c->limit) {
                if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 512; c->res = (Item *)realloc(c->res, c->cap * sizeof(Item)); }
                Item *i = &c->res[c->n++]; memset(i, 0, sizeof *i); item_from_fd(i, dir, &fd); scpy(i->type, 64, dir);
            } else c->cancel = 1;
            LeaveCriticalSection(&c->cs);
        }
        if (isd && c->sub && depth < 24 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            WCHAR sub[MAX_PATH]; path_join(sub, MAX_PATH, dir, fd.cFileName); search_walk(c, sub, depth + 1);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static DWORD WINAPI search_thread(LPVOID p) {
    SearchCtx *c = (SearchCtx *)p; search_walk(c, c->root, 0); c->done = 1; sctx_release(c); return 0;
}
static void cancel_search(void) {
    if (S.search) { S.search->cancel = 1; sctx_release(S.search); S.search = NULL; }
    for (int i = 0; i < S.ntabs; i++) S.tabs[i].searching = 0;
}
static void start_search(const WCHAR *q) {
    Tab *t = T(); cancel_search();
    SearchCtx *c = (SearchCtx *)calloc(1, sizeof *c); InitializeCriticalSection(&c->cs); c->refs = 2;
    scpy(c->root, MAX_PATH, t->path); scpy(c->q, 256, q); c->wild = wcspbrk(q, L"*?") != NULL;
    c->sub = C.search_subfolders; c->limit = C.search_limit; c->hidden = C.show_hidden;
    S.search = c; t->searching = 1; tab_clear_items(t); rebuild_view(t); t->scroll = 0;
    HANDLE h = CreateThread(NULL, 0, search_thread, c, 0, NULL);
    if (h) CloseHandle(h); else { c->done = 1; c->refs = 1; t->searching = 0; }
    redraw();
}
static void poll_search(void) {
    SearchCtx *c = S.search; Tab *t = T(); if (!c || !t->searching) return;
    int added = 0;
    EnterCriticalSection(&c->cs);
    for (; c->taken < c->n; c->taken++) { Item *d = tab_add(t); *d = c->res[c->taken]; added++; }
    int done = c->done, total = c->n, hit = c->cancel;
    LeaveCriticalSection(&c->cs);
    if (added) rebuild_view(t);
    if (done) {
        wfmt(S.status_msg, 160, L"Search finished: %d found%ls", total, hit ? L" (limit reached)" : L"");
        sctx_release(c); S.search = NULL; t->searching = 1;   /* keep results shown as search mode */
    } else wfmt(S.status_msg, 160, L"Searching... %d found", total);
    if (added || done) redraw();
}

/* ---- reload / navigate ---- */
static void nav_rebuild(void);
static void sel_clear(Tab *t) { for (int i = 0; i < t->n; i++) t->it[i].sel = 0; t->focus = t->anchor = -1; }
static void preview_update(void);
static void reload(int keep) {
    Tab *t = T(); SL keepsel = { 0 };
    if (keep) for (int i = 0; i < t->n; i++) if (t->it[i].sel) sl_add(&keepsel, t->it[i].path);
    int sc = t->scroll;
    if (t->filter[0] && t->path[0] && !is_special(t->path) && C.search_subfolders) {
        S.nav_t = 0; start_search(t->filter); sl_clear(&keepsel); free(keepsel.v); return;
    }
    cancel_search(); t->searching = 0; tab_clear_items(t); WCHAR err[160] = L"";
    if (!t->path[0] || is_special(t->path)) read_special(t); else read_dir(t, err);
    rebuild_view(t);
    for (int i = 0; i < t->n; i++) if (sl_find(&keepsel, t->it[i].path) >= 0) t->it[i].sel = 1;
    sl_clear(&keepsel); free(keepsel.v);
    t->scroll = keep ? sc : 0; scpy(S.status_msg, 160, err); S.nav_t = 0;
    preview_update(); redraw();
}
static void set_title(void) {
    Tab *t = T(); WCHAR ti[MAX_PATH + 32];
    const WCHAR *n = !t->path[0] ? L"This PC" : !wcscmp(t->path, L"::quick") ? cur_style()->sec[0] : !wcscmp(t->path, L"::recent") ? cur_style()->sec[1]
                   : !wcscmp(t->path, L"::fav") ? cur_style()->sec[2] : (base_name(t->path)[0] ? base_name(t->path) : t->path);
    wfmt(ti, MAX_PATH + 32, L"%ls - %ls", n, cur_style()->label); SetWindowTextW(S.hwnd, ti);
}
static void set_search_text(const WCHAR *s) { S.noevent++; SetWindowTextW(S.e_search, s); S.noevent--; }
static void navigate(const WCHAR *path, int push) {
    WCHAR p[MAX_PATH]; scpy(p, MAX_PATH, path); Tab *t = T();
    if (p[0] && !is_special(p)) { norm_path(p); if (!dir_exists(p)) { toast(L"Folder not found"); return; } }
    cancel_search(); t->filter[0] = 0; set_search_text(L""); scpy(t->path, MAX_PATH, p);
    if (push) {
        while (t->hist.n > t->hidx + 1) sl_del(&t->hist, t->hist.n - 1);
        if (!t->hist.n || !wieq(t->hist.v[t->hist.n - 1], p)) sl_add(&t->hist, p);
        t->hidx = t->hist.n - 1;
    }
    sel_clear(t); t->scroll = 0;
    if (p[0] && !is_special(p)) { sl_front(&C.recent_folders, p, 12); scpy(C.last_dir, MAX_PATH, p); }
    reload(0); set_title();
}
static void go_back(void) { Tab *t = T(); if (t->hidx > 0) { t->hidx--; navigate(t->hist.v[t->hidx], 0); } }
static void go_fwd(void) { Tab *t = T(); if (t->hidx < t->hist.n - 1) { t->hidx++; navigate(t->hist.v[t->hidx], 0); } }
static void go_up(void) {
    Tab *t = T(); if (!t->path[0]) return;
    if (is_special(t->path)) { navigate(L"", 1); return; }
    WCHAR p[MAX_PATH]; parent_of(t->path, p); navigate(p, 1);
}

/* ---- tabs ---- */
static void tab_init(Tab *t, const WCHAR *path) {
    memset(t, 0, sizeof *t); scpy(t->path, MAX_PATH, path); sl_add(&t->hist, path); t->focus = t->anchor = -1;
}
static void switch_tab(int i) {
    if (i < 0 || i >= S.ntabs) return; cancel_search(); S.cur = i;
    set_search_text(T()->filter); set_title(); reload(1);
}
static void new_tab(const WCHAR *path) {
    if (S.ntabs >= MAXTABS) { toast(L"Too many tabs"); return; }
    WCHAR p[MAX_PATH]; scpy(p, MAX_PATH, path ? path : T()->path); if (p[0] && !is_special(p)) norm_path(p);
    if (p[0] && !is_special(p) && !dir_exists(p)) p[0] = 0;
    tab_init(&S.tabs[S.ntabs], p); S.cur = S.ntabs++; set_search_text(L""); set_title(); reload(0);
}
static void tab_free(Tab *t) { sl_clear(&t->hist); free(t->hist.v); free(t->it); free(t->view); }
static void close_tab(int i) {
    if (i < 0 || i >= S.ntabs || S.tabs[i].pinned) return;
    if (S.ntabs == 1) { navigate(L"::quick", 1); return; }
    cancel_search(); tab_free(&S.tabs[i]);
    memmove(&S.tabs[i], &S.tabs[i + 1], (S.ntabs - i - 1) * sizeof(Tab)); S.ntabs--;
    if (S.cur >= S.ntabs) S.cur = S.ntabs - 1; else if (S.cur > i) S.cur--;
    set_search_text(T()->filter); set_title(); reload(1);
}

/* ---- selection ---- */
static int sel_count(Tab *t, u64 *bytes) {
    int n = 0; u64 b = 0;
    for (int i = 0; i < t->n; i++) if (t->it[i].sel) { n++; b += t->it[i].size; }
    if (bytes) *bytes = b; return n;
}
static Item *focus_item(Tab *t) { return (t->focus >= 0 && t->focus < t->nv) ? &t->it[t->view[t->focus]] : NULL; }
static void sel_view(int vi, int ctrl, int shift) {
    Tab *t = T(); if (vi < 0 || vi >= t->nv) return;
    if (shift && t->anchor >= 0 && t->anchor < t->nv) {
        if (!ctrl) for (int i = 0; i < t->n; i++) t->it[i].sel = 0;
        int a = t->anchor < vi ? t->anchor : vi, b = t->anchor < vi ? vi : t->anchor;
        for (int i = a; i <= b; i++) t->it[t->view[i]].sel = 1;
    } else if (ctrl) { Item *x = &t->it[t->view[vi]]; x->sel = !x->sel; t->anchor = vi; }
    else { for (int i = 0; i < t->n; i++) t->it[i].sel = 0; t->it[t->view[vi]].sel = 1; t->anchor = vi; }
    t->focus = vi; preview_update(); redraw();
}
static void select_all(void) { Tab *t = T(); for (int i = 0; i < t->nv; i++) t->it[t->view[i]].sel = 1; preview_update(); redraw(); }
static void invert_sel(void) { Tab *t = T(); for (int i = 0; i < t->nv; i++) { Item *x = &t->it[t->view[i]]; x->sel = !x->sel; } preview_update(); redraw(); }
static void collect_sel(Tab *t, SL *out) {
    for (int i = 0; i < t->nv; i++) { Item *x = &t->it[t->view[i]]; if (x->sel && x->kind != 1) sl_add(out, x->path); }
}

/* ---- file operations ---- */
static void clear_ro(const WCHAR *p) {
    DWORD a = GetFileAttributesW(p); if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(p, a & ~FILE_ATTRIBUTE_READONLY);
}
static int delete_tree(const WCHAR *p) {
    DWORD a = GetFileAttributesW(p); if (a == INVALID_FILE_ATTRIBUTES) return 1;
    clear_ro(p);
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            WCHAR pat[MAX_PATH]; path_join(pat, MAX_PATH, p, L"*"); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                     WCHAR c[MAX_PATH]; path_join(c, MAX_PATH, p, fd.cFileName); delete_tree(c);
                } while (FindNextFileW(h, &fd)); FindClose(h);
            }
        }
        return RemoveDirectoryW(p) != 0;
    }
    return DeleteFileW(p) != 0;
}
static int copy_tree(const WCHAR *src, const WCHAR *dst) {
    DWORD a = GetFileAttributesW(src); if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (under_path(dst, src)) return 0;                  /* folder into itself */
        if (!CreateDirectoryW(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
        WCHAR pat[MAX_PATH]; path_join(pat, MAX_PATH, src, L"*"); WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
        int ok = 1; if (h == INVALID_HANDLE_VALUE) return 1;
        do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
             WCHAR s2[MAX_PATH], d2[MAX_PATH]; path_join(s2, MAX_PATH, src, fd.cFileName); path_join(d2, MAX_PATH, dst, fd.cFileName);
             if (!copy_tree(s2, d2)) ok = 0;
        } while (FindNextFileW(h, &fd)); FindClose(h); return ok;
    }
    return CopyFileW(src, dst, FALSE) != 0;
}
typedef struct { WCHAR dest[MAX_PATH]; SL srcs; int move; HWND hwnd; } PasteJob;
static DWORD WINAPI paste_thread(LPVOID p) {
    PasteJob *j = (PasteJob *)p; int done = 0, fail = 0, apply = 0, mode = 0;   /* mode 1 replace 2 both 3 skip */
    for (int i = 0; i < j->srcs.n; i++) {
        const WCHAR *src = j->srcs.v[i]; WCHAR dst[MAX_PATH], par[MAX_PATH]; const WCHAR *nm = base_name(src);
        parent_of(src, par); int same = wieq(par, j->dest) || (j->dest[0] && wieq(par, j->dest));
        wfmt(S.status_msg, 160, L"%ls %d of %d: %ls", j->move ? L"Moving" : L"Copying", i + 1, j->srcs.n, nm); PostMessageW(j->hwnd, WM_APP_REFRESH, 0, 0);
        path_join(dst, MAX_PATH, j->dest, nm);
        if (same && j->move) continue;
        if (same) unique_name(j->dest, nm, dir_exists(src), dst, MAX_PATH);
        else if (path_exists(dst)) {
            int m = mode;
            if (!apply) {
                WCHAR msg[MAX_PATH + 200]; wfmt(msg, MAX_PATH + 200, L"\"%ls\" already exists in the destination.\n\nYes = Replace\nNo = Keep both\nCancel = Skip", nm);
                int r = MessageBoxW(j->hwnd, msg, L"Name conflict", MB_YESNOCANCEL | MB_ICONQUESTION);
                m = r == IDYES ? 1 : r == IDNO ? 2 : 3;
                if (j->srcs.n - i > 1 && MessageBoxW(j->hwnd, L"Do this for the remaining conflicts?", L"Name conflict", MB_YESNO) == IDYES) { apply = 1; mode = m; }
            }
            if (m == 3) continue;
            if (m == 2) unique_name(j->dest, nm, dir_exists(src), dst, MAX_PATH);
            else delete_tree(dst);
        }
        int ok;
        if (j->move) {
            ok = MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED) != 0;
            if (!ok) { ok = copy_tree(src, dst); if (ok) delete_tree(src); }
        } else ok = copy_tree(src, dst);
        if (ok) done++; else fail++;
    }
    wfmt(S.status_msg, 160, L"%d item(s) %ls%ls", done, j->move ? L"moved" : L"copied", fail ? L" - some failed" : L"");
    PostMessageW(j->hwnd, WM_APP_TASKDONE, fail, 0);
    sl_clear(&j->srcs); free(j->srcs.v); free(j); return 0;
}
static void do_paste(void) {
    Tab *t = T(); if (!S.clip.n) { toast(L"Clipboard is empty"); return; }
    if (!t->path[0] || is_special(t->path)) { toast(L"Open a folder to paste into"); return; }
    if (InterlockedCompareExchange(&S.busy, 1, 0)) { toast(L"Another operation is running"); return; }
    PasteJob *j = (PasteJob *)calloc(1, sizeof *j); scpy(j->dest, MAX_PATH, t->path); j->move = S.clip_cut; j->hwnd = S.hwnd;
    for (int i = 0; i < S.clip.n; i++) sl_add(&j->srcs, S.clip.v[i]);
    if (S.clip_cut) { sl_clear(&S.clip); S.clip_cut = 0; }
    HANDLE h = CreateThread(NULL, 0, paste_thread, j, 0, NULL);
    if (h) CloseHandle(h); else { S.busy = 0; sl_clear(&j->srcs); free(j->srcs.v); free(j); }
}
static void do_copy(int cut) {
    Tab *t = T(); sl_clear(&S.clip); collect_sel(t, &S.clip); S.clip_cut = cut;
    WCHAR m[64]; wfmt(m, 64, L"%d item(s) %ls", S.clip.n, cut ? L"cut" : L"copied"); if (S.clip.n) toast(m);
}
static void do_delete(void) {
    Tab *t = T(); SL s = { 0 }; collect_sel(t, &s); if (!s.n) return;
    WCHAR msg[MAX_PATH + 120];
    if (s.n == 1) wfmt(msg, MAX_PATH + 120, L"Permanently delete \"%ls\"?\nThis cannot be undone.", base_name(s.v[0]));
    else wfmt(msg, 200, L"Permanently delete %d items?\nThis cannot be undone.", s.n);
    if (MessageBoxW(S.hwnd, msg, L"Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
        int fail = 0; for (int i = 0; i < s.n; i++) if (!delete_tree(s.v[i])) fail++;
        for (int i = 0; i < s.n; i++) { int k = sl_find(&C.pins, s.v[i]); if (k >= 0) sl_del(&C.pins, k); }
        if (fail) toast(L"Some items could not be deleted");
        reload(0);
    }
    sl_clear(&s); free(s.v);
}
static void begin_rename(void);
static void unique_new(const WCHAR *dir, const WCHAR *stem, const WCHAR *ext, WCHAR *out, int cap) {
    WCHAR n[MAX_PATH]; wfmt(n, MAX_PATH, L"%ls%ls", stem, ext); path_join(out, cap, dir, n);
    for (int i = 2; path_exists(out) && i < 10000; i++) { wfmt(n, MAX_PATH, L"%ls (%d)%ls", stem, i, ext); path_join(out, cap, dir, n); }
}
static void new_item(int folder) {
    Tab *t = T(); if (!t->path[0] || is_special(t->path)) { toast(L"Open a folder first"); return; }
    WCHAR p[MAX_PATH]; if (folder) unique_new(t->path, L"New folder", L"", p, MAX_PATH); else unique_new(t->path, L"New Text Document", L".txt", p, MAX_PATH);
    int ok = folder ? CreateDirectoryW(p, NULL) : (CloseHandle(CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)) != 0);
    if (!ok) { toast(L"Could not create item"); return; }
    reload(0);
    for (int i = 0; i < t->nv; i++) if (wieq(t->it[t->view[i]].path, p)) { sel_view(i, 0, 0); break; }
    begin_rename();
}
static void open_item(Item *it) {
    if (!it) return;
    if (it->dir) { navigate(it->path, 1); return; }
    sl_front(&C.recent_files, it->path, 20);
    WCHAR dir[MAX_PATH]; parent_of(it->path, dir);
    HINSTANCE r = ShellExecuteW(S.hwnd, L"open", it->path, NULL, dir[0] ? dir : NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) toast(L"No program is associated with this file - use Open with...");
}
static void open_with(Item *it) {
    if (!it || it->dir) return;
    WCHAR f[MAX_PATH] = L""; OPENFILENAMEW o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = S.hwnd;
    o.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH;
    o.lpstrTitle = L"Choose a program"; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&o)) return;
    WCHAR args[MAX_PATH + 4]; wfmt(args, MAX_PATH + 4, L"\"%ls\"", it->path); WCHAR dir[MAX_PATH]; parent_of(it->path, dir);
    ShellExecuteW(S.hwnd, L"open", f, args, dir, SW_SHOWNORMAL);
}
static void show_properties(Item *it) {
    if (!it) return; WIN32_FILE_ATTRIBUTE_DATA d; WCHAR m[1400], sz[64], mod[40], cr[40], par[MAX_PATH], attr[32] = L"";
    if (!GetFileAttributesExW(it->path, GetFileExInfoStandard, &d)) { toast(L"Cannot read properties"); return; }
    u64 s = ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow; fmt_date(&d.ftLastWriteTime, mod, 40); fmt_date(&d.ftCreationTime, cr, 40); parent_of(it->path, par);
    if (it->dir) scpy(sz, 64, L"-"); else { WCHAR b[32]; fmt_bytes(s, b, 32); wfmt(sz, 64, L"%ls (%I64u bytes)", b, s); }
    if (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) scat(attr, 32, L"Read-only ");
    if (d.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) scat(attr, 32, L"Hidden ");
    if (d.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) scat(attr, 32, L"System");
    wfmt(m, 1400, L"Name:\t%ls\nType:\t%ls\nLocation:\t%ls\nSize:\t%ls\nModified:\t%ls\nCreated:\t%ls\nAttributes:\t%ls", it->name, it->type, par, sz, mod, cr, attr[0] ? attr : L"-");
    MessageBoxW(S.hwnd, m, L"Properties", MB_OK | MB_ICONINFORMATION);
}
static void copy_path_text(Item *it) {
    if (!it || !OpenClipboard(S.hwnd)) return; EmptyClipboard();
    size_t n = (lstrlenW(it->path) + 1) * sizeof(WCHAR); HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) { memcpy(GlobalLock(g), it->path, n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard(); toast(L"Path copied");
}
static void open_cmd(void) {
    Tab *t = T(); WCHAR a[MAX_PATH + 16];
    if (t->path[0] && !is_special(t->path)) wfmt(a, MAX_PATH + 16, L"/k cd /d \"%ls\"", t->path); else lstrcpyW(a, L"/k");
    ShellExecuteW(S.hwnd, L"open", L"cmd.exe", a, NULL, SW_SHOWNORMAL);
}

/* ========================================================================== */
/* PREVIEW                                                                    */
/* ========================================================================== */
static HBITMAP g_pbmp; static int g_pbw, g_pbh;
static int g_mx = -1, g_my = -1;
static RECT g_mb[6], g_tabr[MAXTABS], g_tabplus;
static void preview_free(void) {
    free(S.prev.text); S.prev.text = NULL; S.prev.kind = 0; S.prev.tscroll = 0;
    if (g_pbmp) { DeleteObject(g_pbmp); g_pbmp = NULL; }
}
static WCHAR *load_text_preview(const WCHAR *path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL); if (sz == INVALID_FILE_SIZE || sz == 0) { CloseHandle(h); return NULL; }
    int known = ext_in(path, L".txt.md.log.ini.cfg.json.xml.csv.c.h.cpp.hpp.py.js.ts.html.htm.css.bat.cmd.sh.yaml.yml.lua.java.rs.go.conf.inf.reg.srt.toml.properties.");
    if (!known && sz > (2u << 20)) { CloseHandle(h); return NULL; }
    DWORD want = sz > 65536 ? 65536 : sz, rd = 0; u8 *b = (u8 *)malloc(want + 4); ReadFile(h, b, want, &rd, NULL); CloseHandle(h);
    if (!rd) { free(b); return NULL; }
    if (!known) for (DWORD i = 0; i < rd && i < 4096; i++) if (b[i] == 0 && !(rd >= 2 && ((b[0] == 0xFF && b[1] == 0xFE)))) { free(b); return NULL; }
    WCHAR *out = NULL;
    if (rd >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        int n = (rd - 2) / 2; out = (WCHAR *)malloc((n + 1) * sizeof(WCHAR)); memcpy(out, b + 2, n * 2); out[n] = 0;
    } else {
        u8 *s = b; DWORD n = rd; if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { s += 3; n -= 3; }
        UINT cp = CP_UTF8; int w = 0;
        for (int cut = 0; cut < 4 && n > (DWORD)cut; cut++) { w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)s, n - cut, NULL, 0); if (w > 0) { n -= cut; break; } }
        if (w <= 0) { cp = CP_ACP; w = MultiByteToWideChar(cp, 0, (char *)s, n, NULL, 0); }
        out = (WCHAR *)malloc((w + 1) * sizeof(WCHAR)); MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, (char *)s, n, out, w); out[w] = 0;
    }
    free(b); return out;
}
static void preview_update(void) {
    preview_free(); Tab *t = T(); u64 by; int nsel = sel_count(t, &by); (void)by;
    Item *it = NULL;
    if (nsel == 1) for (int i = 0; i < t->n; i++) if (t->it[i].sel) { it = &t->it[i]; break; }
    if (!it) { S.prev.path[0] = 0; return; }
    scpy(S.prev.path, MAX_PATH, it->path); scpy(S.prev.type, 64, it->type); S.prev.size = it->size; S.prev.mt = it->mt; S.prev.dir = it->dir;
    S.prev.kind = 0;
    if (!it->dir && is_image(it->path) && G.ok) S.prev.kind = 1;
    else if (!it->dir && it->size > 0) { S.prev.text = load_text_preview(it->path); if (S.prev.text) S.prev.kind = 2; }
}

/* ========================================================================== */
/* LAYOUT                                                                     */
/* ========================================================================== */
typedef struct { RECT menubar, navbar, tabs, nav, content, preview, status, hdr, body, sb, back, fwd, up, refresh, menu, addr, search, views[3]; int has_menubar; } Lay;
static Lay LY;
static int view_mode(void) { Tab *t = T(); if (!t->path[0]) return 0; return !wcscmp(C.view, L"icons") ? 1 : !wcscmp(C.view, L"tiles") ? 2 : 0; }
static void do_layout(int W, int H) {
    const Style *st = cur_style(); Lay *L = &LY; memset(L, 0, sizeof *L); int y = 0;
    int mh = st->menubar_h ? px(st->menubar_h) : 0;
    if (mh) { L->menubar = (RECT){ 0, y, W, y + mh }; y += mh; L->has_menubar = 1; }
    int th = st->layout == LAY_HEADER ? px(st->cap_h_fm) : px(st->tool_h); if (th < px(34)) th = px(34);
    L->navbar = (RECT){ 0, y, W, y + th }; y += th;
    if (S.ntabs > 1) { L->tabs = (RECT){ 0, y, W, y + px(28) }; y += px(28); }
    int sh = px(st->status_h); int bottom = H - sh; if (bottom < y) bottom = y;
    L->status = (RECT){ 0, bottom, W, H };
    int navw = px(st->nav_w); if (navw > W / 2) navw = W / 2;
    int pw = 0;
    if (C.show_preview) { pw = px(C.preview_width); if (pw > W - navw - px(300)) pw = W - navw - px(300); if (pw < px(120)) pw = 0; }
    L->nav = (RECT){ 0, y, navw, bottom }; L->content = (RECT){ navw, y, W - pw, bottom }; L->preview = (RECT){ W - pw, y, W, bottom };
    int bh = px(st->tb_btn), fh = px(st->field_h), gap = px(st->tb_gap), cy = (L->navbar.top + L->navbar.bottom) / 2, x = px(8);
    RECT *b[4] = { &L->back, &L->fwd, &L->up, &L->refresh };
    for (int i = 0; i < 4; i++) { *b[i] = (RECT){ x, cy - bh / 2, x + bh, cy - bh / 2 + bh }; x += bh + gap; }
    int right = W - px(8);
    if (!L->has_menubar) { L->menu = (RECT){ right - bh, cy - bh / 2, right, cy - bh / 2 + bh }; right -= bh + gap * 2; }
    int sw = px(st->search_w); if (sw > (right - x) / 2) sw = (right - x) / 2;
    L->search = (RECT){ right - sw, cy - fh / 2, right, cy - fh / 2 + fh }; right -= sw + gap * 2 + px(4);
    L->addr = (RECT){ x + px(4), cy - fh / 2, right, cy - fh / 2 + fh };
    int det = view_mode() == 0, sbw = px(st->sb_w); if (sbw < px(10)) sbw = px(10);
    int hh = det ? px(st->hdr_h) : 0;
    L->hdr = (RECT){ L->content.left, L->content.top, L->content.right, L->content.top + hh };
    L->body = (RECT){ L->content.left, L->content.top + hh, L->content.right - sbw, bottom };
    L->sb = (RECT){ L->content.right - sbw, L->body.top, L->content.right, bottom };
    int vb = sh - px(4); if (vb < px(18)) vb = px(18);
    for (int i = 0; i < 3; i++) L->views[i] = (RECT){ W - px(10) - vb * (3 - i), bottom + (sh - vb) / 2, W - px(10) - vb * (2 - i), bottom + (sh - vb) / 2 + vb };
}
static void col_x(int bw, int c[5]) {
    int date = px(150), typ = px(T()->searching ? 230 : 130), size = px(90); int name = bw - date - typ - size;
    if (name < px(160)) name = px(160);
    c[0] = 0; c[1] = name; c[2] = name + date; c[3] = name + date + typ; c[4] = name + date + typ + size;
}
static int cell_geom(int *cw, int *chh, int *per) {
    int bw = LY.body.right - LY.body.left; int m = view_mode();
    if (m == 1) { *cw = px(C.icon_size * 1.5 + 30); *chh = px(C.icon_size + 52); }
    else { *cw = px(230); *chh = px(58); }
    *per = (bw - px(8)) / *cw; if (*per < 1) *per = 1; return m;
}
static int total_h(void) {
    Tab *t = T(); int m = view_mode();
    if (m == 0) return t->nv * px(cur_style()->row_h) + px(4);
    int cw, ch, per; cell_geom(&cw, &ch, &per); return ((t->nv + per - 1) / per) * ch + px(8);
}
static void clamp_scroll(void) {
    Tab *t = T(); int vh = LY.body.bottom - LY.body.top, mx = total_h() - vh; if (mx < 0) mx = 0;
    if (t->scroll > mx) t->scroll = mx; if (t->scroll < 0) t->scroll = 0;
}
static RECT item_rect(int i) {
    Tab *t = T(); int m = view_mode(); RECT r; int sc = t->scroll;
    if (m == 0) { int rh = px(cur_style()->row_h); r = (RECT){ LY.body.left, LY.body.top + i * rh - sc, LY.body.right, LY.body.top + (i + 1) * rh - sc }; return r; }
    int cw, ch, per; cell_geom(&cw, &ch, &per); int col = i % per, row = i / per;
    r.left = LY.body.left + px(4) + col * cw; r.top = LY.body.top + px(4) + row * ch - sc; r.right = r.left + cw - px(4); r.bottom = r.top + ch - px(4); return r;
}
static int hit_item(int x, int y) {
    Tab *t = T(); if (!in_rect(&LY.body, x, y)) return -1;
    int m = view_mode(), vi;
    if (m == 0) vi = (y - LY.body.top + t->scroll) / px(cur_style()->row_h);
    else { int cw, ch, per; cell_geom(&cw, &ch, &per); int col = (x - LY.body.left - px(4)) / cw, row = (y - LY.body.top - px(4) + t->scroll) / ch;
           if (col >= per || col < 0 || row < 0) return -1; vi = row * per + col; }
    if (vi < 0 || vi >= t->nv) return -1;
    if (m != 0) { RECT r = item_rect(vi); if (!in_rect(&r, x, y)) return -1; }
    return vi;
}
static void ensure_visible(int vi) {
    Tab *t = T(); if (vi < 0 || vi >= t->nv) return; RECT r = item_rect(vi);
    if (r.top < LY.body.top) t->scroll -= LY.body.top - r.top; else if (r.bottom > LY.body.bottom) t->scroll += r.bottom - LY.body.bottom;
    clamp_scroll();
}

/* ---- nav pane model ---- */
static void nav_add(const WCHAR *label, const WCHAR *path, int root, int glyph, int sect) {
    if (S.nnav == S.ncap_nav) { S.ncap_nav = S.ncap_nav ? S.ncap_nav * 2 : 64; S.nav = (NavEnt *)realloc(S.nav, S.ncap_nav * sizeof(NavEnt)); }
    NavEnt *e = &S.nav[S.nnav++]; scpy(e->label, MAX_PATH, label); scpy(e->path, MAX_PATH, path); e->root = root; e->glyph = glyph; e->sect = sect;
}
static void nav_rebuild(void) {
    const Style *st = cur_style(); WCHAR h[MAX_PATH], p[MAX_PATH]; home_dir(h); S.nnav = 0;
    nav_add(st->sec[0], L"::quick", 1, 1, 0);
    if (S.nav_open[0] == 0) {
        static const WCHAR *n[] = { L"Desktop", L"Downloads", L"Documents", L"Pictures", L"Music", L"Videos" };
        for (int k = 0; k < 6; k++) { path_join(p, MAX_PATH, h, n[k]); if (dir_exists(p)) nav_add(n[k], p, 0, 0, 0); }
    }
    nav_add(st->sec[1], L"::recent", 1, 2, 1);
    if (S.nav_open[1] == 0) for (int k = 0; k < C.recent_folders.n && k < 8; k++)
        if (dir_exists(C.recent_folders.v[k])) { const WCHAR *b = base_name(C.recent_folders.v[k]); nav_add(*b ? b : C.recent_folders.v[k], C.recent_folders.v[k], 0, 0, 1); }
    nav_add(st->sec[2], L"::fav", 1, 3, 2);
    if (S.nav_open[2] == 0) for (int k = 0; k < C.favorites.n; k++)
        if (dir_exists(C.favorites.v[k])) { const WCHAR *b = base_name(C.favorites.v[k]); nav_add(*b ? b : C.favorites.v[k], C.favorites.v[k], 0, 0, 2); }
    nav_add(st->sec[3], L"", 1, 4, 3);
    if (S.nav_open[3] == 0) {
        DWORD mask = GetLogicalDrives();
        for (int k = 0; k < 26; k++) if (mask & (1u << k)) { WCHAR root[4] = { (WCHAR)('A' + k), ':', '\\', 0 }, lab[32]; wfmt(lab, 32, L"%lc: (%ls)", root[0], GetDriveTypeW(root) == DRIVE_CDROM ? L"CD" : L"Local Disk"); nav_add(lab, root, 0, 5, 3); }
    }
    S.nav_t = GetTickCount();
}
static int nav_hit(int x, int y) {
    if (y < LY.nav.top || x >= LY.nav.right) return -1;
    int rh = px(cur_style()->nav_row); int i = (y - LY.nav.top - px(4) + S.nav_scroll) / rh; return (i >= 0 && i < S.nnav) ? i : -1;
}

/* ========================================================================== */
/* DRAWING                                                                    */
/* ========================================================================== */
static void update_pal(void) {
    build_pal(&S.pal); if (S.br_input) DeleteObject(S.br_input); S.br_input = CreateSolidBrush(S.pal.input);
}
static void apply_fonts(void) {
    if (S.font) DeleteObject(S.font); if (S.font_big) DeleteObject(S.font_big); if (S.font_small) DeleteObject(S.font_small);
    int size = px(C.font_size); const WCHAR *fn = C.font[0] ? C.font : L"Segoe UI";
    if (C.style_fonts) fn = pick_font(cur_style()->font);
    S.font = make_font(fn, size, 400); S.font_big = make_font(fn, size + 2, 600); S.font_small = make_font(fn, size > 10 ? size - 1 : size, 400);
    if (S.e_addr) { SendMessageW(S.e_addr, WM_SETFONT, (WPARAM)S.font, TRUE); SendMessageW(S.e_search, WM_SETFONT, (WPARAM)S.font, TRUE); SendMessageW(S.e_rename, WM_SETFONT, (WPARAM)S.font, TRUE); }
}
static void tb_glyph(HDC dc, int kind, const RECT *r, COLORREF c) {
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, s = (r->bottom - r->top) / 5; if (s < 3) s = 3;
    switch (kind) {
    case 0: linec(dc, cx + s, cy, cx - s, cy, c, 2); linec(dc, cx - s, cy, cx, cy - s, c, 2); linec(dc, cx - s, cy, cx, cy + s, c, 2); break;
    case 1: linec(dc, cx - s, cy, cx + s, cy, c, 2); linec(dc, cx + s, cy, cx, cy - s, c, 2); linec(dc, cx + s, cy, cx, cy + s, c, 2); break;
    case 2: linec(dc, cx, cy + s, cx, cy - s, c, 2); linec(dc, cx, cy - s, cx - s, cy, c, 2); linec(dc, cx, cy - s, cx + s, cy, c, 2); break;
    case 3: { HPEN p = CreatePen(PS_SOLID, 2, c); HGDIOBJ o = SelectObject(dc, p); Arc(dc, cx - s, cy - s, cx + s, cy + s, cx + s * 9 / 10, cy + s / 2, cx + s / 2, cy - s * 9 / 10);
              SelectObject(dc, o); DeleteObject(p); POINT t[3] = { { cx + s / 2 - 2, cy - s - 3 }, { cx + s / 2 + 5, cy - s + 1 }, { cx + s / 2 - 1, cy - s + 5 } }; poly(dc, t, 3, c); break; }
    case 4: linec(dc, cx - s, cy - s, cx + s, cy - s, c, 2); linec(dc, cx - s, cy, cx + s, cy, c, 2); linec(dc, cx - s, cy + s, cx + s, cy + s, c, 2); break;
    case 5: { RECT e = { cx - s, cy - s, cx + s - 1, cy + s - 1 }; HPEN p = CreatePen(PS_SOLID, 2, c); HGDIOBJ o = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
              Ellipse(dc, e.left, e.top, e.right, e.bottom); SelectObject(dc, o); SelectObject(dc, ob); DeleteObject(p); linec(dc, cx + s / 2, cy + s / 2, cx + s + 2, cy + s + 2, c, 2); break; }
    case 6: linec(dc, cx - s, cy, cx + s, cy, c, 2); linec(dc, cx, cy - s, cx, cy + s, c, 2); break;
    case 7: for (int i = 0; i < 3; i++) { fill4(dc, cx - s, cy - s + i * s - 1, cx - s + 3, cy - s + i * s + 2, c); fill4(dc, cx - s + 5, cy - s + i * s, cx + s, cy - s + i * s + 1, c); } break;
    case 8: for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) { RECT q = { cx - s + a * (s + 1), cy - s + b * (s + 1), cx - s + a * (s + 1) + s - 1, cy - s + b * (s + 1) + s - 1 }; fillr(dc, &q, c); } break;
    case 9: for (int a = 0; a < 2; a++) { RECT q = { cx - s, cy - s + a * (s + 1), cx - s + 4, cy - s + a * (s + 1) + s - 1 }; fillr(dc, &q, c); fill4(dc, cx - s + 6, cy - s + a * (s + 1) + 2, cx + s, cy - s + a * (s + 1) + 3, c); } break;
    }
}
static void tb_btn(HDC dc, const RECT *r, int kind, int enabled, int hover) {
    const Pal *p = &S.pal; const Style *st = cur_style();
    if (hover && enabled) rrect(dc, r, p->hover, p->hover, st->round > 8 ? 8 : st->round, 0);
    tb_glyph(dc, kind, r, enabled ? p->text : mixc(p->text, p->tool, 0.65));
}
static const WCHAR *special_label(const WCHAR *p) {
    const Style *st = cur_style();
    if (!p[0]) return st->sec[3]; if (!wcscmp(p, L"::quick")) return st->sec[0]; if (!wcscmp(p, L"::recent")) return st->sec[1]; if (!wcscmp(p, L"::fav")) return st->sec[2];
    return p;
}
static void draw_crumbs(HDC dc) {
    const Pal *p = &S.pal; Tab *t = T(); RECT a = LY.addr; S.ncrumbs = 0;
    WCHAR segs[40][MAX_PATH], tg[40][MAX_PATH]; int n = 0;
    if (!t->path[0] || is_special(t->path)) { scpy(segs[0], MAX_PATH, special_label(t->path)); scpy(tg[0], MAX_PATH, t->path); n = 1; }
    else {
        const WCHAR *sp = t->path; WCHAR cum[MAX_PATH] = L"";
        while (*sp && n < 39) {
            while (*sp == '\\') sp++; if (!*sp) break;
            const WCHAR *e = sp; while (*e && *e != '\\') e++;
            int l = (int)(e - sp); if (l >= MAX_PATH) l = MAX_PATH - 1; memcpy(segs[n], sp, l * sizeof(WCHAR)); segs[n][l] = 0;
            if (n == 0) wfmt(cum, MAX_PATH, L"%ls\\", segs[0]); else path_join(cum, MAX_PATH, cum, segs[n]);
            scpy(tg[n], MAX_PATH, cum); n++; sp = e;
        }
    }
    int pad = px(8), sep = px(14), avail = a.right - a.left - px(16), wid[40], tot = 0;
    for (int i = 0; i < n; i++) { wid[i] = text_w(dc, segs[i]) + pad * 2; tot += wid[i] + (i ? sep : 0); }
    int first = 0; while (first < n - 1 && tot > avail) { tot -= wid[first] + (first ? sep : 0); first++; }
    int x = a.left + px(8); int cy = (a.top + a.bottom) / 2;
    for (int i = first; i < n; i++) {
        if (i > first) { int sx = x + sep / 2; linec(dc, sx - 2, cy - 4, sx + 2, cy, p->dim, 1); linec(dc, sx + 2, cy, sx - 2, cy + 4, p->dim, 1); x += sep; }
        RECT r = { x, a.top + 2, x + wid[i], a.bottom - 2 }; if (r.right > a.right - 2) r.right = a.right - 2;
        if (in_rect(&r, g_mx, g_my)) rrect(dc, &r, p->hover, p->hover, 3, 0);
        textr(dc, segs[i], &r, TC, p->text);
        if (S.ncrumbs < 40) { S.crumbs[S.ncrumbs].r = r; scpy(S.crumbs[S.ncrumbs].path, MAX_PATH, tg[i]); S.ncrumbs++; }
        x += wid[i];
    }
}
static void draw_toolbar(HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); Tab *t = T();
    fillr(dc, &LY.navbar, st->layout == LAY_HEADER ? p->cap : p->tool);
    fill4(dc, 0, LY.navbar.bottom - 1, LY.navbar.right, LY.navbar.bottom, p->line);
    SelectObject(dc, S.font);
    tb_btn(dc, &LY.back, 0, t->hidx > 0, in_rect(&LY.back, g_mx, g_my));
    tb_btn(dc, &LY.fwd, 1, t->hidx < t->hist.n - 1, in_rect(&LY.fwd, g_mx, g_my));
    tb_btn(dc, &LY.up, 2, t->path[0] != 0, in_rect(&LY.up, g_mx, g_my));
    tb_btn(dc, &LY.refresh, 3, 1, in_rect(&LY.refresh, g_mx, g_my));
    if (!LY.has_menubar) tb_btn(dc, &LY.menu, 4, 1, in_rect(&LY.menu, g_mx, g_my));
    int rr = st->round > 12 ? (LY.addr.bottom - LY.addr.top) / 2 : st->round;
    rrect(dc, &LY.addr, p->input, p->input_border, rr, 1);
    if (!S.addr_edit) draw_crumbs(dc);
    rrect(dc, &LY.search, p->input, p->input_border, rr, 1);
    RECT mg = { LY.search.left + px(4), LY.search.top, LY.search.left + px(4) + (LY.search.bottom - LY.search.top), LY.search.bottom };
    tb_glyph(dc, 5, &mg, p->dim);
}
static void draw_menubar(HDC dc) {
    static const WCHAR *names[6] = { L"File", L"Edit", L"View", L"Go", L"Style", L"Tools" };
    const Pal *p = &S.pal; fillr(dc, &LY.menubar, p->cap); fill4(dc, 0, LY.menubar.bottom - 1, LY.menubar.right, LY.menubar.bottom, p->line);
    SelectObject(dc, S.font); int x = px(6);
    for (int i = 0; i < 6; i++) {
        int w = text_w(dc, names[i]) + px(18); RECT r = { x, LY.menubar.top + 1, x + w, LY.menubar.bottom - 1 }; g_mb[i] = r;
        if (in_rect(&r, g_mx, g_my)) fillr(dc, &r, p->hover);
        textr(dc, names[i], &r, TC, p->cap_text); x += w;
    }
}
static void draw_tabs(HDC dc) {
    if (S.ntabs <= 1) return; const Pal *p = &S.pal; RECT tr = LY.tabs; fillr(dc, &tr, p->cap);
    SelectObject(dc, S.font); int W = tr.right - tr.left, tw = px(180), pw = px(40); if (S.ntabs * tw > W - pw) tw = (W - pw) / S.ntabs;
    int x = tr.left + px(4);
    for (int i = 0; i < S.ntabs; i++) {
        Tab *t = &S.tabs[i]; RECT r = { x, tr.top + px(3), x + tw - px(2), tr.bottom }; g_tabr[i] = r;
        int act = i == S.cur; COLORREF bg = act ? p->bg : (in_rect(&r, g_mx, g_my) ? p->hover : p->cap);
        rrect(dc, &r, bg, bg, 4, 0); if (act) fill4(dc, r.left + 2, r.bottom - 2, r.right - 2, r.bottom, p->accent);
        const WCHAR *n = !t->path[0] ? L"This PC" : is_special(t->path) ? special_label(t->path) : (base_name(t->path)[0] ? base_name(t->path) : t->path);
        RECT tx = { r.left + px(8), r.top, r.right - (t->pinned ? px(8) : px(24)), r.bottom }; textr(dc, n, &tx, TL, act ? p->text : p->dim);
        if (t->pinned) { RECT d = { r.right - px(12), (r.top + r.bottom) / 2 - 3, r.right - px(6), (r.top + r.bottom) / 2 + 3 }; ellipsec(dc, &d, p->accent, p->accent, 1); }
        else { int cx = r.right - px(12), cy = (r.top + r.bottom) / 2; COLORREF cc = in_rect(&r, g_mx, g_my) ? p->text : p->dim;
               linec(dc, cx - 3, cy - 3, cx + 4, cy + 4, cc, 1); linec(dc, cx - 3, cy + 3, cx + 4, cy - 4, cc, 1); }
        x += tw;
    }
    g_tabplus = (RECT){ x + px(2), tr.top + px(3), x + px(2) + px(28), tr.bottom };
    tb_glyph(dc, 6, &g_tabplus, in_rect(&g_tabplus, g_mx, g_my) ? p->text : p->dim);
    fill4(dc, 0, tr.bottom - 1, tr.right, tr.bottom, p->line);
}
static void draw_item_icon(HDC dc, const Item *it, int x, int y, int s, int thumbs, int *budget) {
    const Style *st = cur_style();
    if (it->kind == 1) { paint_glyph(dc, 5, x, y, s, S.pal.text); return; }
    if (it->dir) { paint_folder(dc, x, y, s, st->folder); return; }
    if (thumbs && C.thumbnails && is_image(it->path)) {
        HBITMAP b = thumb_get(dc, it->path, s, budget);
        if (b) { HDC m = CreateCompatibleDC(dc); HGDIOBJ o = SelectObject(m, b); BitBlt(dc, x, y, s, s, m, 0, 0, SRCCOPY); SelectObject(m, o); DeleteDC(m); return; }
    }
    HICON ic = icon_for(it->path, s > 20, 0);
    if (ic) DrawIconEx(dc, x, y, ic, s, s, 0, NULL, DI_NORMAL);
}
static void sort_arrow(HDC dc, int cx, int cy, COLORREF c, int up) {
    POINT t[3]; if (up) { t[0] = (POINT){ cx - 4, cy + 2 }; t[1] = (POINT){ cx + 4, cy + 2 }; t[2] = (POINT){ cx, cy - 3 }; }
    else { t[0] = (POINT){ cx - 4, cy - 2 }; t[1] = (POINT){ cx + 4, cy - 2 }; t[2] = (POINT){ cx, cy + 3 }; } poly(dc, t, 3, c);
}
static void draw_content(HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); Tab *t = T(); int m = view_mode(); int budget = 8; g_thumb_more = 0;
    fillr(dc, &LY.content, p->bg); clamp_scroll();
    int rr = st->round > 8 ? 6 : st->round;
    if (m == 0) {
        int c[5]; col_x(LY.body.right - LY.body.left, c); RECT h = LY.hdr; fillr(dc, &h, p->hdr_bg);
        static const WCHAR *lab[4] = { L"Name", L"Date modified", L"Type", L"Size" }; static const WCHAR *key[4] = { L"name", L"date", L"type", L"size" };
        SelectObject(dc, S.font_small);
        for (int i = 0; i < 4; i++) {
            RECT r = { h.left + c[i], h.top, h.left + c[i + 1], h.bottom }; RECT tr = { r.left + px(8), r.top, r.right - px(18), r.bottom };
            textr(dc, i == 3 && !t->searching ? lab[i] : (t->searching && i == 2 ? L"Location" : lab[i]), &tr, TL | (i == 3 ? DT_RIGHT : 0), p->hdr);
            if (!wcscmp(C.sort, key[i])) sort_arrow(dc, r.right - px(10), (r.top + r.bottom) / 2, p->hdr, C.asc);
            if (i) fill4(dc, r.left, r.top + 3, r.left + 1, r.bottom - 3, p->line);
        }
        fill4(dc, h.left, h.bottom - 1, h.right, h.bottom, p->line);
    }
    HRGN rg = CreateRectRgn(LY.body.left, LY.body.top, LY.body.right, LY.body.bottom); SelectClipRgn(dc, rg); DeleteObject(rg);
    int first = 0, last = t->nv - 1;
    if (m == 0) { int rh = px(st->row_h); first = t->scroll / rh; last = (t->scroll + LY.body.bottom - LY.body.top) / rh; if (last >= t->nv) last = t->nv - 1; }
    int c[5]; col_x(LY.body.right - LY.body.left, c);
    for (int vi = first < 0 ? 0 : first; vi <= last; vi++) {
        Item *it = &t->it[t->view[vi]]; RECT r = item_rect(vi);
        if (r.bottom < LY.body.top || r.top > LY.body.bottom) continue;
        int hov = in_rect(&r, g_mx, g_my) && in_rect(&LY.body, g_mx, g_my);
        COLORREF bg = p->bg, fg = p->text;
        if (m == 0 && st->zebra && (vi & 1)) bg = p->zebra;
        if (it->sel) { bg = p->sel; fg = p->sel_text; } else if (hov) bg = p->hover;
        if (bg != p->bg) { if (m == 0) { RECT q = r; if (rr > 2) { q.left += px(4); q.right -= px(4); } rrect(dc, &q, bg, bg, rr > 2 ? rr : 0, 0); } else rrect(dc, &r, bg, bg, rr, 0); }
        if (vi == t->focus) { RECT q = r; q.left += 1; q.top += 1; q.right -= 1; q.bottom -= 1; HPEN pn = CreatePen(PS_DOT, 1, it->sel ? fg : p->dim); HGDIOBJ op = SelectObject(dc, pn), ob = SelectObject(dc, GetStockObject(NULL_BRUSH)); Rectangle(dc, q.left, q.top, q.right, q.bottom); SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pn); }
        if (m == 0) {
            int isz = px(18); int iy = r.top + (r.bottom - r.top - isz) / 2; SelectObject(dc, S.font);
            draw_item_icon(dc, it, r.left + px(10), iy, isz, 0, &budget);
            RECT nr = { r.left + px(10) + isz + px(8), r.top, r.left + c[1] - px(4), r.bottom }; textr(dc, it->name, &nr, TL, fg);
            WCHAR b[64]; RECT dr = { r.left + c[1] + px(8), r.top, r.left + c[2] - px(4), r.bottom }; fmt_date(&it->mt, b, 64); textr(dc, b, &dr, TL, it->sel ? fg : p->dim);
            RECT tr = { r.left + c[2] + px(8), r.top, r.left + c[3] - px(4), r.bottom }; textr(dc, it->type, &tr, TL, it->sel ? fg : p->dim);
            RECT sr = { r.left + c[3], r.top, r.left + c[4] - px(12), r.bottom };
            if (!it->dir || it->kind == 1) { if (it->size || !it->dir) fmt_bytes(it->size, b, 64); else b[0] = 0; textr(dc, b, &sr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, it->sel ? fg : p->dim); }
        } else if (m == 1) {
            int isz = px(C.icon_size); draw_item_icon(dc, it, r.left + (r.right - r.left - isz) / 2, r.top + px(6), isz, 1, &budget); SelectObject(dc, S.font_small);
            RECT nr = { r.left + px(3), r.top + isz + px(10), r.right - px(3), r.bottom - px(2) }; textr(dc, it->name, &nr, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS, fg);
        } else {
            int isz = px(40); draw_item_icon(dc, it, r.left + px(8), r.top + (r.bottom - r.top - isz) / 2, isz, 1, &budget);
            int x0 = r.left + isz + px(16); WCHAR b[64];
            RECT n1 = { x0, r.top + px(4), r.right - px(4), r.top + (r.bottom - r.top) / 2 + 2 }; SelectObject(dc, S.font); textr(dc, it->name, &n1, TL, fg);
            SelectObject(dc, S.font_small); RECT n2 = { x0, n1.bottom - 2, r.right - px(4), r.bottom - px(2) };
            if (it->dir) wfmt(b, 64, L"%ls", it->type); else { WCHAR s[32]; fmt_bytes(it->size, s, 32); wfmt(b, 64, L"%ls - %ls", it->type, s); }
            textr(dc, b, &n2, TL, it->sel ? fg : p->dim);
        }
    }
    if (!t->nv) {
        SelectObject(dc, S.font); RECT r = LY.body; r.top += px(30);
        textr(dc, S.status_msg[0] && !t->searching ? S.status_msg : (t->searching ? L"Searching..." : (t->filter[0] ? L"No matching items" : L"This folder is empty")), &r, DT_CENTER | DT_SINGLELINE, p->dim);
    }
    SelectClipRgn(dc, NULL);
    /* scrollbar */
    int th = total_h(), vh = LY.sb.bottom - LY.sb.top; fillr(dc, &LY.sb, p->bg);
    if (th > vh && vh > 0) {
        int tl = vh * vh / th; if (tl < px(24)) tl = px(24); int rng = vh - tl, mx = th - vh; int ty = LY.sb.top + (mx ? (int)((double)t->scroll / mx * rng) : 0);
        RECT tr = { LY.sb.left + px(3), ty, LY.sb.right - px(3), ty + tl }; rrect(dc, &tr, S.dragsb ? mixc(p->sb_thumb, p->text, 0.3) : p->sb_thumb, p->sb_thumb, 4, 0);
    }
}
static void draw_nav(HDC dc) {
    const Pal *p = &S.pal; const Style *st = cur_style(); Tab *t = T(); RECT nr = LY.nav; fillr(dc, &nr, p->side);
    fill4(dc, nr.right - 1, nr.top, nr.right, nr.bottom, p->line);
    HRGN rg = CreateRectRgn(nr.left, nr.top, nr.right - 1, nr.bottom); SelectClipRgn(dc, rg); DeleteObject(rg);
    int rh = px(st->nav_row), y = nr.top + px(4) - S.nav_scroll, rr = st->round > 8 ? 6 : st->round;
    if (!S.nnav || GetTickCount() - S.nav_t > 2000) nav_rebuild();
    for (int i = 0; i < S.nnav; i++, y += rh) {
        NavEnt *e = &S.nav[i]; RECT r = { nr.left + px(4), y, nr.right - px(6), y + rh };
        if (r.bottom < nr.top || r.top > nr.bottom) continue;
        int cur = wieq(e->path, t->path) && (!e->root || t->path[0] == ':' || !t->path[0]); int hov = i == S.hover_nav;
        COLORREF fg = p->text;
        if (cur) { rrect(dc, &r, p->nav_sel, p->nav_sel, rr, 0); fg = p->nav_sel_text; } else if (hov) rrect(dc, &r, p->hover, p->hover, rr, 0);
        int ix = r.left + px(e->root ? 6 : 24), isz = px(16), iy = y + (rh - isz) / 2;
        if (e->root) {
            int open = S.nav_open[e->sect] == 0, ax = r.left + px(8), ay = y + rh / 2;
            if (e->sect < 4) { if (open) { linec(dc, ax - 3, ay - 2, ax, ay + 2, p->dim, 1); linec(dc, ax, ay + 2, ax + 4, ay - 2, p->dim, 1); } else { linec(dc, ax - 2, ay - 3, ax + 2, ay, p->dim, 1); linec(dc, ax + 2, ay, ax - 2, ay + 4, p->dim, 1); } }
            ix = r.left + px(20); SelectObject(dc, S.font_big);
            paint_glyph(dc, e->glyph, ix, iy, isz, cur ? fg : p->accent);
        } else {
            SelectObject(dc, S.font);
            if (e->glyph == 5) paint_glyph(dc, 5, ix, iy, isz, fg); else paint_folder(dc, ix, iy, isz, st->folder);
        }
        RECT tx = { ix + isz + px(8), y, r.right - px(4), y + rh }; textr(dc, e->label, &tx, TL, fg);
    }
    SelectClipRgn(dc, NULL);
}
static void draw_preview(HDC dc) {
    if (LY.preview.right - LY.preview.left < 2) return; const Pal *p = &S.pal; RECT pr = LY.preview; fillr(dc, &pr, p->side);
    fill4(dc, pr.left, pr.top, pr.left + 1, pr.bottom, p->line);
    HRGN rg = CreateRectRgn(pr.left + 1, pr.top, pr.right, pr.bottom); SelectClipRgn(dc, rg); DeleteObject(rg);
    Prev *v = &S.prev; RECT in = { pr.left + px(12), pr.top + px(10), pr.right - px(12), pr.bottom - px(10) };
    if (!v->path[0]) { SelectObject(dc, S.font); textr(dc, L"Select a file to preview", &pr, DT_CENTER | DT_VCENTER | DT_SINGLELINE, p->dim); SelectClipRgn(dc, NULL); return; }
    WCHAR meta[200], sz[32], dt[40]; fmt_bytes(v->size, sz, 32); fmt_date(&v->mt, dt, 40);
    if (v->dir) wfmt(meta, 200, L"%ls\n%ls", v->type, dt); else wfmt(meta, 200, L"%ls - %ls\n%ls", v->type, sz, dt);
    SelectObject(dc, S.font_big); RECT nr = in; nr.bottom = nr.top + px(40); textr(dc, base_name(v->path), &nr, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX, p->text);
    SelectObject(dc, S.font_small); RECT mr = in; mr.top = nr.bottom; mr.bottom = mr.top + px(36); textr(dc, meta, &mr, DT_WORDBREAK, p->dim);
    RECT body = in; body.top = mr.bottom + px(6);
    if (v->kind == 1) {
        int w = body.right - body.left, h = body.bottom - body.top;
        if (w > 8 && h > 8) {
            if (!g_pbmp || g_pbw != w || g_pbh != h) {
                if (g_pbmp) DeleteObject(g_pbmp);
                HDC m = CreateCompatibleDC(dc); g_pbmp = CreateCompatibleBitmap(dc, w, h); g_pbw = w; g_pbh = h; HGDIOBJ o = SelectObject(m, g_pbmp);
                RECT z = { 0, 0, w, h }; fillr(m, &z, p->side); gdip_draw_fit(m, v->path, 0, 0, w, h, NULL, NULL); SelectObject(m, o); DeleteDC(m);
            }
            HDC m = CreateCompatibleDC(dc); HGDIOBJ o = SelectObject(m, g_pbmp); BitBlt(dc, body.left, body.top, w, h, m, 0, 0, SRCCOPY); SelectObject(m, o); DeleteDC(m);
        }
    } else if (v->kind == 2 && v->text) {
        HRGN r2 = CreateRectRgn(body.left, body.top, body.right, body.bottom); SelectClipRgn(dc, r2); DeleteObject(r2);
        static int has_cons = -1; if (has_cons < 0) has_cons = font_available(L"Consolas");
        HFONT mf = make_font(has_cons ? L"Consolas" : L"Courier New", px(C.font_size > 12 ? C.font_size - 2 : C.font_size), 400); HGDIOBJ of = SelectObject(dc, mf);
        RECT tr = body; tr.top -= v->tscroll; textr(dc, v->text, &tr, DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX, p->text); SelectObject(dc, of); DeleteObject(mf);
    } else {
        Item tmp; memset(&tmp, 0, sizeof tmp); scpy(tmp.path, MAX_PATH, v->path); tmp.dir = v->dir; int isz = px(64);
        draw_item_icon(dc, &tmp, body.left + (body.right - body.left - isz) / 2, body.top + px(20), isz, 0, &(int){ 0 });
    }
    SelectClipRgn(dc, NULL);
}
static void draw_status(HDC dc) {
    const Pal *p = &S.pal; Tab *t = T(); RECT r = LY.status; fillr(dc, &r, p->status); fill4(dc, r.left, r.top, r.right, r.top + 1, p->line);
    SelectObject(dc, S.font_small); u64 by; int ns = sel_count(t, &by); WCHAR m[300], b[32];
    if (S.status_msg[0] && (t->searching || InterlockedCompareExchange(&S.busy, 0, 0))) scpy(m, 300, S.status_msg);
    else if (ns) { fmt_bytes(by, b, 32); wfmt(m, 300, L"%d of %d selected   %ls", ns, t->nv, by ? b : L""); }
    else wfmt(m, 300, L"%d item%ls", t->nv, t->nv == 1 ? L"" : L"s");
    if (!ns && t->path[0] && !is_special(t->path)) { u64 tot; u64 fr = free_space(t->path, &tot); if (tot) { WCHAR f[32]; fmt_bytes(fr, f, 32); wfmt(m + lstrlenW(m), 80, L"    %ls free", f); } }
    RECT tr = { r.left + px(12), r.top, LY.views[0].left - px(8), r.bottom }; textr(dc, m, &tr, TL, p->dim);
    int cur = view_mode();
    for (int i = 0; i < 3; i++) {
        RECT v = LY.views[i]; if (cur == i) rrect(dc, &v, p->sel, p->sel, 3, 0); else if (in_rect(&v, g_mx, g_my)) rrect(dc, &v, p->hover, p->hover, 3, 0);
        tb_glyph(dc, 7 + i, &v, cur == i ? p->sel_text : p->dim);
    }
}
static void draw_client(HDC dc, int W, int H) {
    const Pal *p = &S.pal; RECT all = { 0, 0, W, H }; fillr(dc, &all, p->bg); do_layout(W, H); SetBkMode(dc, TRANSPARENT);
    if (LY.has_menubar) draw_menubar(dc);
    draw_toolbar(dc); draw_tabs(dc); draw_nav(dc); draw_content(dc); draw_preview(dc); draw_status(dc);
    if (S.toast[0] && GetTickCount() - S.toast_t < 3000) {
        SelectObject(dc, S.font); int tw = text_w(dc, S.toast) + px(36), th = px(34);
        RECT r = { (W - tw) / 2, LY.status.top - th - px(14), (W + tw) / 2, LY.status.top - px(14) };
        rrect(dc, &r, p->dark ? RGB(70, 70, 70) : RGB(40, 40, 40), p->border, 8, 1); textr(dc, S.toast, &r, TC, RGB(255, 255, 255));
    }
}

/* ========================================================================== */
/* COMMANDS / MENUS                                                           */
/* ========================================================================== */
enum {
    ID_NEWTAB = 100, ID_NEWFOLDER, ID_NEWFILE, ID_CMD, ID_EXIT, ID_CUT, ID_COPY, ID_PASTE, ID_SELALL, ID_INVERT, ID_RENAME, ID_DELETE,
    ID_COPYPATH, ID_PROPS, ID_OPEN, ID_OPENWITH, ID_REFRESH, ID_VDET, ID_VICON, ID_VTILE, ID_PREVIEW, ID_HIDDEN, ID_THUMBS,
    ID_SORT_NAME, ID_SORT_DATE, ID_SORT_TYPE, ID_SORT_SIZE, ID_ASC, ID_DIRS_FIRST, ID_DIRS_LAST, ID_DIRS_MIXED,
    ID_ICON_PLUS, ID_ICON_MINUS, ID_ZOOM_IN, ID_ZOOM_OUT, ID_BACK, ID_FWD, ID_UP, ID_HOME, ID_PC, ID_FAV, ID_RECENT,
    ID_DARK, ID_STYLEFONTS, ID_SUBFOLDERS, ID_RESTORE, ID_FONT_PLUS, ID_FONT_MINUS, ID_BACKUP, ID_RESTORE_CFG,
    ID_AUTO_EN, ID_AUTO_ADD, ID_AUTO_RUN, ID_AUTO_LOGIN, ID_PIN, ID_FAVTOGGLE, ID_TABCLOSE, ID_TABOTHERS, ID_TABPIN, ID_TABDUP,
    ID_ABOUT, ID_RESET_ACCENT, ID_PREVW,
    ID_STYLE0 = 300, ID_THEME0 = 340, ID_ACCENT0 = 360, ID_AUTOREM0 = 400
};
static int g_tab_ctx = 0;
static void mi(HMENU m, int id, const WCHAR *s, int checked, int enabled) {
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, s);
}
static void place_edits(void);
static void style_changed(void) {
    update_pal(); apply_fonts(); set_title(); nav_rebuild(); cfg_save(); place_edits(); redraw();
}
static int exe_path(WCHAR *o, int cap) { return (int)GetModuleFileNameW(NULL, o, cap); }
static int run_at_login_enabled(void) {
    HKEY k; int on = C.run_at_login;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD ty, n = 0; on = RegQueryValueExW(k, L"WinShellFM", NULL, &ty, NULL, &n) == ERROR_SUCCESS; RegCloseKey(k);
    }
    return on;
}
static void set_run_at_login(int on) {
    C.run_at_login = on; HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) { WCHAR e[MAX_PATH + 4], q[MAX_PATH + 4]; exe_path(e, MAX_PATH); wfmt(q, MAX_PATH + 4, L"\"%ls\"", e); RegSetValueExW(k, L"WinShellFM", 0, REG_SZ, (BYTE *)q, (lstrlenW(q) + 1) * sizeof(WCHAR)); }
    else RegDeleteValueW(k, L"WinShellFM");
    RegCloseKey(k);
}
static void run_autostart(int force) {
    if (S.autostart_done && !force) return; S.autostart_done = 1; if (!force && !C.autostart_enabled) return;
    for (int i = 0; i < C.autostart_apps.n; i++) {
        const WCHAR *p = C.autostart_apps.v[i];
        if (path_exists(p)) { WCHAR d[MAX_PATH]; parent_of(p, d); ShellExecuteW(NULL, L"open", p, NULL, d[0] ? d : NULL, SW_SHOWNORMAL); } else LOG("autostart: missing %ls", p);
    }
}
static HMENU build_menu(int w) {
    HMENU m = CreatePopupMenu(); Tab *t = T(); int real = t->path[0] && !is_special(t->path); int ns = sel_count(t, NULL);
    if (w == 0) {
        mi(m, ID_NEWTAB, L"New tab\tCtrl+T", 0, 1); mi(m, ID_NEWFOLDER, L"New folder\tCtrl+Shift+N", 0, real); mi(m, ID_NEWFILE, L"New text file", 0, real);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_CMD, L"Command prompt here", 0, 1); mi(m, ID_PROPS, L"Properties\tAlt+Enter", 0, ns == 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_EXIT, L"Exit", 0, 1);
    } else if (w == 1) {
        mi(m, ID_CUT, L"Cut\tCtrl+X", 0, ns > 0); mi(m, ID_COPY, L"Copy\tCtrl+C", 0, ns > 0); mi(m, ID_PASTE, L"Paste\tCtrl+V", 0, S.clip.n > 0 && real);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_SELALL, L"Select all\tCtrl+A", 0, 1); mi(m, ID_INVERT, L"Invert selection", 0, 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_RENAME, L"Rename\tF2", 0, ns == 1); mi(m, ID_DELETE, L"Delete\tDel", 0, ns > 0); mi(m, ID_COPYPATH, L"Copy path", 0, ns == 1);
    } else if (w == 2) {
        int vm = view_mode(); mi(m, ID_VDET, L"Details", vm == 0, 1); mi(m, ID_VICON, L"Icons", vm == 1, 1); mi(m, ID_VTILE, L"Tiles", vm == 2, 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        HMENU s = CreatePopupMenu();
        mi(s, ID_SORT_NAME, L"Name", !wcscmp(C.sort, L"name"), 1); mi(s, ID_SORT_DATE, L"Date modified", !wcscmp(C.sort, L"date"), 1);
        mi(s, ID_SORT_TYPE, L"Type", !wcscmp(C.sort, L"type"), 1); mi(s, ID_SORT_SIZE, L"Size", !wcscmp(C.sort, L"size"), 1);
        AppendMenuW(s, MF_SEPARATOR, 0, NULL); mi(s, ID_ASC, L"Ascending", C.asc, 1);
        AppendMenuW(s, MF_SEPARATOR, 0, NULL); mi(s, ID_DIRS_FIRST, L"Folders first", !wcscmp(C.sort_dirs, L"first"), 1);
        mi(s, ID_DIRS_LAST, L"Folders last", !wcscmp(C.sort_dirs, L"last"), 1); mi(s, ID_DIRS_MIXED, L"Mixed", !wcscmp(C.sort_dirs, L"mixed"), 1);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)s, L"Sort by");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        mi(m, ID_PREVIEW, L"Preview pane", C.show_preview, 1); mi(m, ID_HIDDEN, L"Show hidden files", C.show_hidden, 1); mi(m, ID_THUMBS, L"Image thumbnails", C.thumbnails, 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_ICON_PLUS, L"Larger icons", 0, 1); mi(m, ID_ICON_MINUS, L"Smaller icons", 0, 1);
        mi(m, ID_ZOOM_IN, L"Zoom interface in\tCtrl+=", 0, 1); mi(m, ID_ZOOM_OUT, L"Zoom interface out\tCtrl+-", 0, 1);
        mi(m, ID_PREVW, L"Cycle preview width", 0, C.show_preview);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_REFRESH, L"Refresh\tF5", 0, 1);
    } else if (w == 3) {
        mi(m, ID_BACK, L"Back\tAlt+Left", 0, t->hidx > 0); mi(m, ID_FWD, L"Forward\tAlt+Right", 0, t->hidx < t->hist.n - 1); mi(m, ID_UP, L"Up\tBackspace", 0, t->path[0] != 0);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        mi(m, ID_HOME, cur_style()->sec[0], 0, 1); mi(m, ID_RECENT, cur_style()->sec[1], 0, 1); mi(m, ID_FAV, cur_style()->sec[2], 0, 1); mi(m, ID_PC, cur_style()->sec[3], 0, 1);
    } else if (w == 4) {
        for (int i = 0; i < (int)NSTYLES; i++) { WCHAR l[200]; wfmt(l, 200, L"%ls\t%ls", STYLES[STYLE_ORDER[i]].label, STYLES[STYLE_ORDER[i]].info); mi(m, ID_STYLE0 + STYLE_ORDER[i], l, STYLE_ORDER[i] == style_index(C.os_style), 1); }
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        if (is_win10()) {
            HMENU th = CreatePopupMenu(); for (int i = 0; i < (int)NTHEMES; i++) mi(th, ID_THEME0 + i, THEMES[i].name, wieq(C.theme, THEMES[i].name), 1);
            AppendMenuW(m, MF_POPUP, (UINT_PTR)th, L"Theme");
        } else mi(m, ID_DARK, L"Dark variant", C.style_dark, cur_style()->has_dark);
        HMENU ac = CreatePopupMenu(); for (int i = 0; i < 8; i++) { WCHAR l[16]; scpy(l, 16, ACCENTS[i]); mi(ac, ID_ACCENT0 + i, l, wieq(is_win10() ? C.accent : C.style_accent, ACCENTS[i]), 1); }
        if (!is_win10()) { AppendMenuW(ac, MF_SEPARATOR, 0, NULL); mi(ac, ID_RESET_ACCENT, L"Style default", C.style_accent[0] == 0, 1); }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)ac, L"Accent colour");
        mi(m, ID_STYLEFONTS, L"Use the style's fonts", C.style_fonts, 1);
    } else {
        HMENU a = CreatePopupMenu(); mi(a, ID_AUTO_EN, L"Run chosen apps when the file manager starts", C.autostart_enabled, 1);
        mi(a, ID_AUTO_ADD, L"Add program or file...", 0, 1); AppendMenuW(a, MF_SEPARATOR, 0, NULL);
        for (int i = 0; i < C.autostart_apps.n && i < 40; i++) { WCHAR l[MAX_PATH + 16]; wfmt(l, MAX_PATH + 16, L"Remove: %ls", base_name(C.autostart_apps.v[i])); mi(a, ID_AUTOREM0 + i, l, 0, 1); }
        if (C.autostart_apps.n) AppendMenuW(a, MF_SEPARATOR, 0, NULL);
        mi(a, ID_AUTO_RUN, L"Run now", 0, C.autostart_apps.n > 0); mi(a, ID_AUTO_LOGIN, L"Start with Windows (Run key)", run_at_login_enabled(), 1);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)a, L"Autostart");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        mi(m, ID_SUBFOLDERS, L"Search inside subfolders", C.search_subfolders, 1); mi(m, ID_RESTORE, L"Restore tabs on start", C.restore_session, 1);
        mi(m, ID_FONT_PLUS, L"Larger font", 0, 1); mi(m, ID_FONT_MINUS, L"Smaller font", 0, 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_BACKUP, L"Back up settings...", 0, 1); mi(m, ID_RESTORE_CFG, L"Restore settings...", 0, 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_ABOUT, L"About", 0, 1);
    }
    return m;
}
static void popup_at(HMENU m, int x, int y) {
    POINT pt = { x, y }; ClientToScreen(S.hwnd, &pt); SetForegroundWindow(S.hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, S.hwnd, NULL);
    DestroyMenu(m); if (cmd) PostMessageW(S.hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}
static void main_menu(void) {
    HMENU m = CreatePopupMenu(); static const WCHAR *n[6] = { L"File", L"Edit", L"View", L"Go", L"Style", L"Tools" };
    for (int i = 0; i < 6; i++) AppendMenuW(m, MF_POPUP, (UINT_PTR)build_menu(i), n[i]);
    popup_at(m, LY.menu.left, LY.menu.bottom);
}
static void item_menu(int x, int y) {
    Tab *t = T(); Item *f = focus_item(t); if (!f) return; HMENU m = CreatePopupMenu(); int ns = sel_count(t, NULL); int real = f->kind != 1;
    mi(m, ID_OPEN, L"Open", 0, 1); SetMenuDefaultItem(m, ID_OPEN, FALSE);
    if (!f->dir) mi(m, ID_OPENWITH, L"Open with...", 0, 1);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (real) {
        mi(m, ID_CUT, L"Cut", 0, 1); mi(m, ID_COPY, L"Copy", 0, 1); mi(m, ID_COPYPATH, L"Copy path", 0, ns == 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_RENAME, L"Rename", 0, ns == 1); mi(m, ID_DELETE, L"Delete", 0, 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        mi(m, ID_PIN, is_pinned(f) ? L"Unpin from top" : L"Pin to top", 0, 1);
        if (f->dir) mi(m, ID_FAVTOGGLE, sl_find(&C.favorites, f->path) >= 0 ? L"Remove from Favorites" : L"Add to Favorites", 0, 1);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_PROPS, L"Properties", 0, ns == 1);
    }
    popup_at(m, x, y);
}
static void blank_menu(int x, int y) {
    Tab *t = T(); int real = t->path[0] && !is_special(t->path); HMENU m = CreatePopupMenu(); int vm = view_mode();
    mi(m, ID_VDET, L"View: Details", vm == 0, 1); mi(m, ID_VICON, L"View: Icons", vm == 1, 1); mi(m, ID_VTILE, L"View: Tiles", vm == 2, 1);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    mi(m, ID_NEWFOLDER, L"New folder", 0, real); mi(m, ID_NEWFILE, L"New text file", 0, real); mi(m, ID_PASTE, L"Paste", 0, real && S.clip.n);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_REFRESH, L"Refresh", 0, 1); mi(m, ID_CMD, L"Command prompt here", 0, 1);
    popup_at(m, x, y);
}
static void tab_menu(int idx, int x, int y) {
    g_tab_ctx = idx; HMENU m = CreatePopupMenu(); Tab *t = &S.tabs[idx];
    mi(m, ID_TABDUP, L"Duplicate tab", 0, 1); mi(m, ID_TABPIN, t->pinned ? L"Unpin tab" : L"Pin tab", 0, 1);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL); mi(m, ID_TABCLOSE, L"Close tab", 0, !t->pinned); mi(m, ID_TABOTHERS, L"Close other tabs", 0, S.ntabs > 1);
    popup_at(m, x, y);
}
static void backup_settings(int restore) {
    WCHAR f[MAX_PATH] = L"winshell_fm_backup.json"; OPENFILENAMEW o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = S.hwnd;
    o.lpstrFilter = L"JSON (*.json)\0*.json\0All files\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH; o.lpstrDefExt = L"json";
    if (restore) { o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST; if (!GetOpenFileNameW(&o)) return; }
    else { o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST; if (!GetSaveFileNameW(&o)) return; }
    if (restore) {
        if (!CopyFileW(f, CONFIG_PATH, FALSE)) { toast(L"Could not restore settings"); return; }
        cfg_load(); update_pal(); apply_fonts(); place_edits(); nav_rebuild(); reload(0); toast(L"Settings restored");
    } else { cfg_save(); toast(CopyFileW(CONFIG_PATH, f, FALSE) ? L"Settings saved" : L"Could not save settings"); }
}
static void add_autostart(void) {
    WCHAR f[MAX_PATH] = L""; OPENFILENAMEW o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o; o.hwndOwner = S.hwnd;
    o.lpstrFilter = L"Programs (*.exe;*.bat;*.cmd;*.lnk)\0*.exe;*.bat;*.cmd;*.lnk\0All files\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&o) && sl_find(&C.autostart_apps, f) < 0) { sl_add(&C.autostart_apps, f); cfg_save(); toast(L"Added to autostart"); }
}
static void sort_by(const WCHAR *k) {
    if (!wcscmp(C.sort, k)) C.asc = !C.asc; else { scpy(C.sort, 16, k); C.asc = 1; }
    cfg_save(); rebuild_view(T()); redraw();
}
static void set_pref_redraw(void) { cfg_save(); rebuild_view(T()); clamp_scroll(); place_edits(); redraw(); }
static void show_addr(void);
static void do_cmd(int id) {
    Tab *t = T(); Item *f = focus_item(t);
    if (id >= ID_STYLE0 && id < ID_STYLE0 + (int)NSTYLES) { scpy(C.os_style, 16, STYLES[id - ID_STYLE0].key); style_changed(); return; }
    if (id >= ID_THEME0 && id < ID_THEME0 + (int)NTHEMES) { scpy(C.theme, 16, THEMES[id - ID_THEME0].name); style_changed(); return; }
    if (id >= ID_ACCENT0 && id < ID_ACCENT0 + 8) { if (is_win10()) scpy(C.accent, 16, ACCENTS[id - ID_ACCENT0]); else scpy(C.style_accent, 16, ACCENTS[id - ID_ACCENT0]); style_changed(); return; }
    if (id >= ID_AUTOREM0 && id < ID_AUTOREM0 + 40) { int i = id - ID_AUTOREM0; if (i < C.autostart_apps.n) { sl_del(&C.autostart_apps, i); cfg_save(); } return; }
    switch (id) {
    case ID_NEWTAB: new_tab(NULL); break;
    case ID_NEWFOLDER: new_item(1); break; case ID_NEWFILE: new_item(0); break;
    case ID_CMD: open_cmd(); break; case ID_EXIT: PostMessageW(S.hwnd, WM_CLOSE, 0, 0); break;
    case ID_CUT: do_copy(1); break; case ID_COPY: do_copy(0); break; case ID_PASTE: do_paste(); break;
    case ID_SELALL: select_all(); break; case ID_INVERT: invert_sel(); break;
    case ID_RENAME: begin_rename(); break; case ID_DELETE: do_delete(); break;
    case ID_COPYPATH: copy_path_text(f); break; case ID_PROPS: show_properties(f); break;
    case ID_OPEN: if (sel_count(t, NULL) > 1) { for (int i = 0; i < t->nv; i++) { Item *x = &t->it[t->view[i]]; if (x->sel && !x->dir) open_item(x); } } else open_item(f); break;
    case ID_OPENWITH: open_with(f); break;
    case ID_REFRESH: nav_rebuild(); reload(1); break;
    case ID_VDET: scpy(C.view, 16, L"details"); set_pref_redraw(); break;
    case ID_VICON: scpy(C.view, 16, L"icons"); set_pref_redraw(); break;
    case ID_VTILE: scpy(C.view, 16, L"tiles"); set_pref_redraw(); break;
    case ID_PREVIEW: C.show_preview = !C.show_preview; set_pref_redraw(); break;
    case ID_PREVW: C.preview_width = C.preview_width >= 700 ? 280 : C.preview_width + 140; set_pref_redraw(); break;
    case ID_HIDDEN: C.show_hidden = !C.show_hidden; cfg_save(); reload(1); break;
    case ID_THUMBS: C.thumbnails = !C.thumbnails; set_pref_redraw(); break;
    case ID_SORT_NAME: sort_by(L"name"); break; case ID_SORT_DATE: sort_by(L"date"); break;
    case ID_SORT_TYPE: sort_by(L"type"); break; case ID_SORT_SIZE: sort_by(L"size"); break;
    case ID_ASC: C.asc = !C.asc; set_pref_redraw(); break;
    case ID_DIRS_FIRST: scpy(C.sort_dirs, 8, L"first"); set_pref_redraw(); break;
    case ID_DIRS_LAST: scpy(C.sort_dirs, 8, L"last"); set_pref_redraw(); break;
    case ID_DIRS_MIXED: scpy(C.sort_dirs, 8, L"mixed"); set_pref_redraw(); break;
    case ID_ICON_PLUS: C.icon_size = C.icon_size + 16 > 160 ? 160 : C.icon_size + 16; set_pref_redraw(); break;
    case ID_ICON_MINUS: C.icon_size = C.icon_size - 16 < 32 ? 32 : C.icon_size - 16; set_pref_redraw(); break;
    case ID_ZOOM_IN: C.scale = C.scale + 0.1 > 2.5 ? 2.5 : C.scale + 0.1; style_changed(); break;
    case ID_ZOOM_OUT: C.scale = C.scale - 0.1 < 0.6 ? 0.6 : C.scale - 0.1; style_changed(); break;
    case ID_FONT_PLUS: C.font_size = C.font_size + 1 > 28 ? 28 : C.font_size + 1; style_changed(); break;
    case ID_FONT_MINUS: C.font_size = C.font_size - 1 < 9 ? 9 : C.font_size - 1; style_changed(); break;
    case ID_BACK: go_back(); break; case ID_FWD: go_fwd(); break; case ID_UP: go_up(); break;
    case ID_HOME: navigate(L"::quick", 1); break; case ID_PC: navigate(L"", 1); break;
    case ID_FAV: navigate(L"::fav", 1); break; case ID_RECENT: navigate(L"::recent", 1); break;
    case ID_DARK: C.style_dark = !C.style_dark; style_changed(); break;
    case ID_RESET_ACCENT: C.style_accent[0] = 0; style_changed(); break;
    case ID_STYLEFONTS: C.style_fonts = !C.style_fonts; style_changed(); break;
    case ID_SUBFOLDERS: C.search_subfolders = !C.search_subfolders; cfg_save(); break;
    case ID_RESTORE: C.restore_session = !C.restore_session; cfg_save(); break;
    case ID_BACKUP: backup_settings(0); break; case ID_RESTORE_CFG: backup_settings(1); break;
    case ID_AUTO_EN: C.autostart_enabled = !C.autostart_enabled; cfg_save(); break;
    case ID_AUTO_ADD: add_autostart(); break; case ID_AUTO_RUN: run_autostart(1); break;
    case ID_AUTO_LOGIN: set_run_at_login(!run_at_login_enabled()); cfg_save(); break;
    case ID_PIN: if (f) { int k = sl_find(&C.pins, f->path); if (k >= 0) sl_del(&C.pins, k); else sl_add(&C.pins, f->path); cfg_save(); rebuild_view(t); redraw(); } break;
    case ID_FAVTOGGLE: if (f) { int k = sl_find(&C.favorites, f->path); if (k >= 0) sl_del(&C.favorites, k); else sl_add(&C.favorites, f->path); cfg_save(); S.nav_t = 0; redraw(); } break;
    case ID_TABCLOSE: close_tab(g_tab_ctx); break;
    case ID_TABPIN: if (g_tab_ctx < S.ntabs) { S.tabs[g_tab_ctx].pinned = !S.tabs[g_tab_ctx].pinned; redraw(); } break;
    case ID_TABDUP: if (g_tab_ctx < S.ntabs) new_tab(S.tabs[g_tab_ctx].path); break;
    case ID_TABOTHERS: for (int i = S.ntabs - 1; i >= 0; i--) if (i != g_tab_ctx && !S.tabs[i].pinned) { if (i < g_tab_ctx) g_tab_ctx--; close_tab(i); } break;
    case ID_ABOUT: MessageBoxW(S.hwnd, L"WinShell File Manager (native C port)\nWin32 / GDI - runs on Windows and Wine/Winlator.\nOS looks: Windows 10/11/7, macOS, Ubuntu, Zorin, Mint, KDE, ChromeOS, XFCE, Total Commander, Midnight Commander.", L"About", MB_OK | MB_ICONINFORMATION); break;
    }
}

/* ========================================================================== */
/* EDIT CONTROLS (address, search, rename)                                    */
/* ========================================================================== */
static void place_edits(void) {
    if (!S.hwnd || !S.e_search || !S.ntabs) return;
    RECT rc; GetClientRect(S.hwnd, &rc); do_layout(rc.right, rc.bottom);
    int eh = px(C.font_size) + px(6); RECT s = LY.search, a = LY.addr; int ix = s.left + px(6) + (s.bottom - s.top);
    MoveWindow(S.e_search, ix, (s.top + s.bottom) / 2 - eh / 2, s.right - ix - px(8), eh, TRUE);
    MoveWindow(S.e_addr, a.left + px(6), (a.top + a.bottom) / 2 - eh / 2, a.right - a.left - px(12), eh, TRUE);
}
static void show_addr(void) {
    Tab *t = T(); S.addr_edit = 1; SetWindowTextW(S.e_addr, (!t->path[0] || is_special(t->path)) ? L"" : t->path);
    place_edits(); ShowWindow(S.e_addr, SW_SHOW); SetFocus(S.e_addr); SendMessageW(S.e_addr, EM_SETSEL, 0, -1); redraw();
}
static void hide_addr(void) { S.addr_edit = 0; ShowWindow(S.e_addr, SW_HIDE); redraw(); }
static void begin_rename(void) {
    Tab *t = T(); Item *it = focus_item(t); if (!it || it->kind == 1 || sel_count(t, NULL) != 1) return;
    ensure_visible(t->focus); RECT r = item_rect(t->focus); int m = view_mode(), eh = px(C.font_size) + px(6); RECT e;
    if (m == 0) { int c[5]; col_x(LY.body.right - LY.body.left, c); e = (RECT){ r.left + px(10) + px(26), r.top + (r.bottom - r.top - eh) / 2, r.left + c[1] - px(6), 0 }; }
    else if (m == 1) e = (RECT){ r.left + px(2), r.bottom - eh - px(4), r.right - px(2), 0 };
    else e = (RECT){ r.left + px(56), r.top + px(6), r.right - px(6), 0 };
    S.rename_item = t->view[t->focus]; SetWindowTextW(S.e_rename, it->name);
    MoveWindow(S.e_rename, e.left, e.top, e.right - e.left, eh, TRUE); ShowWindow(S.e_rename, SW_SHOW); SetFocus(S.e_rename);
    int len = lstrlenW(it->name); const WCHAR *dot = wcsrchr(it->name, '.'); int stop = (!it->dir && dot && dot != it->name) ? (int)(dot - it->name) : len;
    SendMessageW(S.e_rename, EM_SETSEL, 0, stop);
}
static void rename_end(int commit) {
    if (S.rename_item < 0) return; Tab *t = T(); int idx = S.rename_item; S.rename_item = -1;
    WCHAR nn[256]; GetWindowTextW(S.e_rename, nn, 256); ShowWindow(S.e_rename, SW_HIDE); SetFocus(S.hwnd);
    if (!commit || idx >= t->n) return;
    Item *it = &t->it[idx]; int l = lstrlenW(nn); while (l && nn[l - 1] == ' ') nn[--l] = 0;
    if (!nn[0] || !wcscmp(nn, it->name)) return;
    if (wcspbrk(nn, L"\\/:*?\"<>|")) { toast(L"A name cannot contain \\ / : * ? \" < > |"); return; }
    WCHAR par[MAX_PATH], np[MAX_PATH]; parent_of(it->path, par); path_join(np, MAX_PATH, par, nn);
    if (path_exists(np) && !wieq(np, it->path)) { toast(L"An item with that name already exists"); return; }
    if (!MoveFileW(it->path, np)) { toast(L"Rename failed"); return; }
    int pk = sl_find(&C.pins, it->path); if (pk >= 0) { free(C.pins.v[pk]); C.pins.v[pk] = wdup(np); }
    reload(0);
    for (int i = 0; i < t->nv; i++) if (wieq(t->it[t->view[i]].path, np)) { sel_view(i, 0, 0); ensure_visible(i); break; }
}
static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) { PostMessageW(S.hwnd, WM_APP_EDITKEY, (WPARAM)h, wp == VK_RETURN); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_DOWN && h == S.e_search) { SetFocus(S.hwnd); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    if (msg == WM_KILLFOCUS) PostMessageW(S.hwnd, WM_APP_EDITKEY, (WPARAM)h, 2);
    return CallWindowProcW(S.edit_old, h, msg, wp, lp);
}
static void apply_search_text(void) {
    Tab *t = T(); WCHAR q[256]; GetWindowTextW(S.e_search, q, 256);
    if (!wcscmp(q, t->filter)) return;
    scpy(t->filter, 256, q); t->scroll = 0;
    if (!q[0]) { cancel_search(); t->searching = 0; reload(0); }
    else if (t->path[0] && !is_special(t->path) && C.search_subfolders) start_search(q);
    else { cancel_search(); t->searching = 0; rebuild_view(t); redraw(); }
}
static void on_editkey(HWND h, int act) {
    if (h == S.e_rename) { if (act == 1) rename_end(1); else if (act == 0) rename_end(0); else if (IsWindowVisible(h)) rename_end(1); }
    else if (h == S.e_addr) {
        if (!S.addr_edit) return;
        if (act == 1) { WCHAR p[MAX_PATH]; GetWindowTextW(h, p, MAX_PATH); hide_addr(); SetFocus(S.hwnd); if (p[0]) navigate(p, 1); else navigate(L"", 1); }
        else { hide_addr(); if (act == 0) SetFocus(S.hwnd); }
    } else if (h == S.e_search) {
        if (act == 0) { set_search_text(L""); apply_search_text(); SetFocus(S.hwnd); } else if (act == 1) { apply_search_text(); SetFocus(S.hwnd); }
    }
}

/* ========================================================================== */
/* INPUT                                                                      */
/* ========================================================================== */
static int region_at(int x, int y) {
    const RECT *rs[] = { &LY.back, &LY.fwd, &LY.up, &LY.refresh, &LY.menu, &LY.views[0], &LY.views[1], &LY.views[2] };
    for (int i = 0; i < 8; i++) if (in_rect(rs[i], x, y)) return i + 1;
    if (S.ntabs > 1) { for (int i = 0; i < S.ntabs; i++) if (in_rect(&g_tabr[i], x, y)) return 10 + i; if (in_rect(&g_tabplus, x, y)) return 30; }
    if (LY.has_menubar) for (int i = 0; i < 6; i++) if (in_rect(&g_mb[i], x, y)) return 31 + i;
    for (int i = 0; i < S.ncrumbs; i++) if (in_rect(&S.crumbs[i].r, x, y)) return 40 + i;
    return 0;
}
static void scroll_by(int d) { Tab *t = T(); t->scroll += d; clamp_scroll(); redraw(); }
static void sb_drag_to(int y) {
    Tab *t = T(); int th = total_h(), vh = LY.sb.bottom - LY.sb.top; if (th <= vh) return;
    int tl = vh * vh / th; if (tl < px(24)) tl = px(24); int rng = vh - tl, mx = th - vh; if (rng <= 0) return;
    int ty = y - S.sb_off - LY.sb.top; if (ty < 0) ty = 0; if (ty > rng) ty = rng; t->scroll = (int)((double)ty / rng * mx); redraw();
}
static void click_header(int x) {
    int c[5]; col_x(LY.body.right - LY.body.left, c); static const WCHAR *k[4] = { L"name", L"date", L"type", L"size" };
    int rx = x - LY.hdr.left; for (int i = 0; i < 4; i++) if (rx >= c[i] && rx < c[i + 1]) { sort_by(k[i]); return; }
}
static void on_lbutton(int x, int y, WPARAM wp) {
    Tab *t = T(); SetFocus(S.hwnd); int ctrl = (wp & MK_CONTROL) != 0, shift = (wp & MK_SHIFT) != 0;
    if (LY.has_menubar) for (int i = 0; i < 6; i++) if (in_rect(&g_mb[i], x, y)) { popup_at(build_menu(i), g_mb[i].left, g_mb[i].bottom); return; }
    if (in_rect(&LY.back, x, y)) { go_back(); return; } if (in_rect(&LY.fwd, x, y)) { go_fwd(); return; }
    if (in_rect(&LY.up, x, y)) { go_up(); return; } if (in_rect(&LY.refresh, x, y)) { nav_rebuild(); reload(1); return; }
    if (!LY.has_menubar && in_rect(&LY.menu, x, y)) { main_menu(); return; }
    if (in_rect(&LY.addr, x, y)) { for (int i = 0; i < S.ncrumbs; i++) if (in_rect(&S.crumbs[i].r, x, y)) { navigate(S.crumbs[i].path, 1); return; } show_addr(); return; }
    if (S.ntabs > 1 && y >= LY.tabs.top && y < LY.tabs.bottom) {
        for (int i = 0; i < S.ntabs; i++) if (in_rect(&g_tabr[i], x, y)) {
            if (!S.tabs[i].pinned && x > g_tabr[i].right - px(24)) close_tab(i); else if (i != S.cur) switch_tab(i); return; }
        if (in_rect(&g_tabplus, x, y)) new_tab(NULL); return;
    }
    for (int i = 0; i < 3; i++) if (in_rect(&LY.views[i], x, y)) { do_cmd(ID_VDET + i); return; }
    if (x < LY.nav.right && y >= LY.nav.top && y < LY.nav.bottom) {
        int i = nav_hit(x, y); if (i < 0) return; NavEnt *e = &S.nav[i];
        if (e->root && x < LY.nav.left + px(22) && e->sect < 4) { S.nav_open[e->sect] = !S.nav_open[e->sect]; nav_rebuild(); redraw(); return; }
        WCHAR p[MAX_PATH]; scpy(p, MAX_PATH, e->path); navigate(p, 1); return;
    }
    if (view_mode() == 0 && in_rect(&LY.hdr, x, y)) { click_header(x); return; }
    if (in_rect(&LY.sb, x, y)) {
        int th = total_h(), vh = LY.sb.bottom - LY.sb.top; if (th <= vh) return; int tl = vh * vh / th; if (tl < px(24)) tl = px(24);
        int rng = vh - tl, mx = th - vh, ty = LY.sb.top + (int)((double)t->scroll / mx * rng);
        if (y >= ty && y < ty + tl) { S.dragsb = 1; S.sb_off = y - ty; SetCapture(S.hwnd); } else scroll_by(y < ty ? -(vh - px(20)) : (vh - px(20)));
        return;
    }
    if (in_rect(&LY.body, x, y)) {
        int vi = hit_item(x, y);
        if (vi >= 0) sel_view(vi, ctrl, shift); else if (!ctrl && !shift) { sel_clear(t); preview_update(); redraw(); }
    }
}
static void on_key(WPARAM vk) {
    Tab *t = T(); int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    int m = view_mode(), cw = 1, ch = 1, per = 1; if (m) cell_geom(&cw, &ch, &per);
    int vis = (LY.body.bottom - LY.body.top) / (m == 0 ? px(cur_style()->row_h) : ch) * (m == 0 ? 1 : per); if (vis < 1) vis = 1;
    int nf = t->focus, step = 0;
    if (alt && vk == VK_LEFT) { go_back(); return; } if (alt && vk == VK_RIGHT) { go_fwd(); return; }
    if (alt && vk == VK_RETURN) { show_properties(focus_item(t)); return; }
    switch (vk) {
    case VK_UP: step = m ? -per : -1; break; case VK_DOWN: step = m ? per : 1; break;
    case VK_LEFT: if (m) step = -1; break; case VK_RIGHT: if (m) step = 1; break;
    case VK_PRIOR: step = -vis; break; case VK_NEXT: step = vis; break;
    case VK_HOME: nf = 0; break; case VK_END: nf = t->nv - 1; break;
    case VK_RETURN: if (ctrl) return; open_item(focus_item(t)); return;
    case VK_BACK: go_up(); return; case VK_F2: begin_rename(); return; case VK_F5: do_cmd(ID_REFRESH); return;
    case VK_DELETE: do_delete(); return; case VK_F4: if (ctrl) close_tab(S.cur); return;
    case VK_TAB: if (ctrl && S.ntabs > 1) switch_tab((S.cur + (shift ? S.ntabs - 1 : 1)) % S.ntabs); return;
    case VK_ESCAPE: if (t->filter[0]) { set_search_text(L""); apply_search_text(); } else { sel_clear(t); preview_update(); redraw(); } return;
    case 'A': if (ctrl) select_all(); return; case 'C': if (ctrl) do_copy(0); return; case 'X': if (ctrl) do_copy(1); return;
    case 'V': if (ctrl) do_paste(); return; case 'T': if (ctrl) new_tab(NULL); return; case 'W': if (ctrl) close_tab(S.cur); return;
    case 'N': if (ctrl && shift) new_item(1); return;
    case 'L': if (ctrl) show_addr(); return; case 'F': if (ctrl) { SetFocus(S.e_search); SendMessageW(S.e_search, EM_SETSEL, 0, -1); } return;
    case VK_OEM_PLUS: case VK_ADD: if (ctrl) do_cmd(ID_ZOOM_IN); return; case VK_OEM_MINUS: case VK_SUBTRACT: if (ctrl) do_cmd(ID_ZOOM_OUT); return;
    default: return;
    }
    if (!t->nv) return;
    if (step) nf = (t->focus < 0 ? (step > 0 ? -1 : t->nv) : t->focus) + step;
    if (nf < 0) nf = 0; if (nf >= t->nv) nf = t->nv - 1;
    sel_view(nf, ctrl && shift ? 0 : 0, shift); ensure_visible(nf); redraw();
}
static void on_char(WCHAR c) {
    if (c < 32 || GetKeyState(VK_CONTROL) < 0) return; Tab *t = T(); DWORD now = GetTickCount();
    if (now - S.type_t > 1000) S.typebuf[0] = 0; S.type_t = now;
    int l = lstrlenW(S.typebuf); if (l < 30) { S.typebuf[l] = c; S.typebuf[l + 1] = 0; }
    for (int i = 0; i < t->nv; i++) if (!_wcsnicmp(t->it[t->view[i]].name, S.typebuf, lstrlenW(S.typebuf))) { sel_view(i, 0, 0); ensure_visible(i); redraw(); return; }
}
static void save_session(void) {
    sl_clear(&C.session_tabs); sl_clear(&C.pinned_tabs);
    for (int i = 0; i < S.ntabs; i++) { sl_add(&C.session_tabs, S.tabs[i].path); if (S.tabs[i].pinned) sl_add(&C.pinned_tabs, S.tabs[i].path); }
    C.session_cur = S.cur;
    WINDOWPLACEMENT wp; wp.length = sizeof wp;
    if (GetWindowPlacement(S.hwnd, &wp)) {
        C.window_w = wp.rcNormalPosition.right - wp.rcNormalPosition.left; C.window_h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        C.window_x = wp.rcNormalPosition.left; C.window_y = wp.rcNormalPosition.top;
    }
    cfg_save();
}
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        S.hwnd = h; S.rename_item = -1; S.hover_nav = -1;
        S.e_addr = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, h, NULL, NULL, NULL);
        S.e_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, h, NULL, NULL, NULL);
        S.e_rename = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, h, NULL, NULL, NULL);
        HWND es[3] = { S.e_addr, S.e_search, S.e_rename };
        for (int i = 0; i < 3; i++) { WNDPROC o = (WNDPROC)SetWindowLongPtrW(es[i], GWLP_WNDPROC, (LONG_PTR)edit_proc); if (!S.edit_old) S.edit_old = o; }
        return 0;
    }
    if (!S.ntabs) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HDC m = CreateCompatibleDC(dc); HBITMAP b = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ ob = SelectObject(m, b);
        draw_client(m, rc.right, rc.bottom); BitBlt(dc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, ob); DeleteObject(b); DeleteDC(m); EndPaint(h, &ps); return 0; }
    case WM_SIZE: if (wp != SIZE_MINIMIZED) { place_edits(); clamp_scroll(); redraw(); } return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)lp; mm->ptMinTrackSize.x = 520; mm->ptMinTrackSize.y = 360; return 0; }
    case WM_CTLCOLOREDIT: SetTextColor((HDC)wp, S.pal.text); SetBkColor((HDC)wp, S.pal.input); return (LRESULT)S.br_input;
    case WM_COMMAND:
        if ((HWND)lp == S.e_search && HIWORD(wp) == EN_CHANGE) { if (!S.noevent) SetTimer(h, TM_SEARCH, 400, NULL); return 0; }
        if (!lp) do_cmd(LOWORD(wp)); return 0;
    case WM_APP_EDITKEY: on_editkey((HWND)wp, (int)lp); return 0;
    case WM_APP_REFRESH: redraw(); return 0;
    case WM_APP_TASKDONE: InterlockedExchange(&S.busy, 0); reload(1); toast(S.status_msg); return 0;
    case WM_TIMER:
        if (wp == TM_SEARCH) { KillTimer(h, TM_SEARCH); apply_search_text(); }
        else if (wp == TM_UI) { poll_search(); if (S.toast[0] && GetTickCount() - S.toast_t < 3300 && GetTickCount() - S.toast_t > 2900) redraw(); if (g_thumb_more) redraw(); }
        else if (wp == TM_NAV) { DWORD o = S.nav_t; nav_rebuild(); (void)o; if (!GetCapture()) redraw(); }
        return 0;
    case WM_LBUTTONDOWN: on_lbutton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp); return 0;
    case WM_LBUTTONUP: if (S.dragsb) { S.dragsb = 0; ReleaseCapture(); redraw(); } return 0;
    case WM_LBUTTONDBLCLK: { int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); int vi = hit_item(x, y);
        if (vi >= 0) { sel_view(vi, 0, 0); open_item(focus_item(T())); } else if (view_mode() == 0 && in_rect(&LY.hdr, x, y)) click_header(x); return 0; }
    case WM_RBUTTONDOWN: { int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); SetFocus(h); int vi = hit_item(x, y);
        if (vi >= 0 && !T()->it[T()->view[vi]].sel) sel_view(vi, 0, 0); else if (vi >= 0) T()->focus = vi;
        if (S.ntabs > 1 && y >= LY.tabs.top && y < LY.tabs.bottom) { for (int i = 0; i < S.ntabs; i++) if (in_rect(&g_tabr[i], x, y)) { tab_menu(i, x, y); return 0; } }
        else if (vi >= 0) item_menu(x, y); else if (in_rect(&LY.body, x, y)) { sel_clear(T()); preview_update(); redraw(); blank_menu(x, y); }
        return 0; }
    case WM_MBUTTONUP: { int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (S.ntabs > 1 && y >= LY.tabs.top && y < LY.tabs.bottom) for (int i = 0; i < S.ntabs; i++) if (in_rect(&g_tabr[i], x, y)) { close_tab(i); break; }
        return 0; }
    case WM_MOUSEMOVE: {
        static unsigned hk; int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp); g_mx = x; g_my = y;
        if (S.dragsb) { sb_drag_to(y); return 0; }
        TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 }; TrackMouseEvent(&te);
        int ni = nav_hit(x, y); if (x >= LY.nav.right) ni = -1; S.hover_nav = ni;
        unsigned k = (unsigned)region_at(x, y) * 1000003u + (unsigned)(hit_item(x, y) + 1) * 131u + (unsigned)(ni + 1);
        if (k != hk) { hk = k; redraw(); } return 0; }
    case WM_MOUSELEAVE: g_mx = g_my = -1; S.hover_nav = -1; redraw(); return 0;
    case WM_MOUSEWHEEL: {
        short d = GET_WHEEL_DELTA_WPARAM(wp); POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(h, &pt);
        if (pt.x < LY.nav.right && pt.y >= LY.nav.top && pt.y < LY.nav.bottom) {
            int tot = S.nnav * px(cur_style()->nav_row), vh = LY.nav.bottom - LY.nav.top; S.nav_scroll -= d * px(cur_style()->nav_row) * 3 / 120;
            if (S.nav_scroll > tot - vh + px(8)) S.nav_scroll = tot - vh + px(8); if (S.nav_scroll < 0) S.nav_scroll = 0; redraw(); return 0;
        }
        if (LY.preview.right - LY.preview.left > 2 && pt.x >= LY.preview.left && S.prev.kind == 2) { S.prev.tscroll -= d * 40 / 120; if (S.prev.tscroll < 0) S.prev.tscroll = 0; redraw(); return 0; }
        if (GetKeyState(VK_CONTROL) < 0 && view_mode() == 1) { do_cmd(d > 0 ? ID_ICON_PLUS : ID_ICON_MINUS); return 0; }
        if (GetKeyState(VK_CONTROL) < 0) { do_cmd(d > 0 ? ID_ZOOM_IN : ID_ZOOM_OUT); return 0; }
        scroll_by(-d * px(cur_style()->row_h) * 3 / 120); return 0; }
    case WM_KEYDOWN: on_key(wp); return 0;
    case WM_SYSKEYDOWN: if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_RETURN) { on_key(wp); return 0; } break;
    case WM_CHAR: on_char((WCHAR)wp); return 0;
    case WM_SETFOCUS: return 0;
    case WM_COPYDATA: { COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp; if (cd && cd->lpData && cd->cbData >= 2) new_tab((const WCHAR *)cd->lpData);
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); return TRUE; }
    case WM_CLOSE: save_session(); DestroyWindow(h); return 0;
    case WM_DESTROY: cancel_search(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ========================================================================== */
/* main                                                                       */
/* ========================================================================== */
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show) {
    (void)hp; (void)cmd; log_init(); detect_wine(); LOG("WinShell (C) starting - Wine=%d", ON_WINE);
    CoInitialize(NULL);
    int argc = 0; WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc); WCHAR open_path[MAX_PATH] = L"";
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--open") && i + 1 < argc) { scpy(open_path, MAX_PATH, argv[++i]); }
        else if (argv[i][0] != '-' && !open_path[0]) scpy(open_path, MAX_PATH, argv[i]);
    }
    HANDLE mtx = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(ES_CLASS, NULL);
        if (w) {
            if (open_path[0]) { COPYDATASTRUCT cd = { 0, (lstrlenW(open_path) + 1) * sizeof(WCHAR), open_path }; SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cd); }
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE); SetForegroundWindow(w);
        }
        return 0;
    }
    cfg_init_paths(); cfg_load(); gdip_start();
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.style = CS_DBLCLKS; wc.lpfnWndProc = wndproc; wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = ES_CLASS; wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (C.window_x != UNSET && C.window_y != UNSET) {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        if (C.window_x > -C.window_w + 100 && C.window_x < sw - 100 && C.window_y > -20 && C.window_y < sh - 100) { x = C.window_x; y = C.window_y; }
    }
    HWND w = CreateWindowExW(0, ES_CLASS, L"WinShell File Manager", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, C.window_w, C.window_h, NULL, NULL, hi, NULL);
    if (!w) { LOG("CreateWindow failed %lu", GetLastError()); return 1; }
    update_pal(); apply_fonts();
    int restored = 0;
    if (!open_path[0] && C.restore_session && C.session_tabs.n) {
        for (int i = 0; i < C.session_tabs.n && S.ntabs < MAXTABS; i++) {
            const WCHAR *p = C.session_tabs.v[i]; if (p[0] && !is_special(p) && !dir_exists(p)) continue;
            tab_init(&S.tabs[S.ntabs], p); S.tabs[S.ntabs].pinned = sl_find(&C.pinned_tabs, p) >= 0; S.ntabs++;
        }
        if (S.ntabs) { restored = 1; S.cur = C.session_cur >= 0 && C.session_cur < S.ntabs ? C.session_cur : 0; }
    }
    if (!restored) {
        WCHAR p[MAX_PATH]; scpy(p, MAX_PATH, open_path[0] ? open_path : C.last_dir); if (p[0]) norm_path(p);
        if (p[0] && !dir_exists(p)) p[0] = 0; if (!p[0]) scpy(p, MAX_PATH, L"::quick");
        tab_init(&S.tabs[0], p); S.ntabs = 1; S.cur = 0;
    } else if (open_path[0]) new_tab(open_path);
    set_search_text(T()->filter); reload(0); set_title(); place_edits();
    SetTimer(w, TM_UI, 250, NULL); SetTimer(w, TM_NAV, 2000, NULL);
    ShowWindow(w, show); UpdateWindow(w);
    run_autostart(0);
    LOG("WinShell File Manager ready");
    MSG m; while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    CloseHandle(mtx); return (int)m.wParam;
}
