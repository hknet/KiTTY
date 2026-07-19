/*
 * winfont_fallback.c -- per-codepoint font fallback for KiTTY (GDI).
 *
 * Ported from upstream PR cyd01/KiTTY#555 (author: blreay); see
 * winfont_fallback.h for the API and the adaptations made for this tree.
 */

#include "winfont_fallback.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef stricmp	/* platform.h may #define stricmp _stricmp (CRT); don't redeclare */
int stricmp(const char *s1, const char *s2);
#endif

/* ===================================================================
 *  Logging
 * =================================================================== */

enum winfb_log_level {
    WINFB_LOG_OFF   = 0,
    WINFB_LOG_ERROR = 1,
    WINFB_LOG_WARN  = 2,
    WINFB_LOG_INFO  = 3,
    WINFB_LOG_DEBUG = 4,
    WINFB_LOG_TRACE = 5
};

static int  g_log_level = WINFB_LOG_OFF;
static FILE *g_log_fp   = NULL;
static char g_log_path[MAX_PATH] = {0};
static int  g_log_lines = 0;

#define WINFB_LOG_MAX_LINES  100000   /* ~10 MB */
#define WINFB_LOG_FLUSH_EVERY 64

static const char *level_name(int lvl)
{
    switch (lvl) {
    case WINFB_LOG_ERROR: return "ERROR";
    case WINFB_LOG_WARN:  return "WARN ";
    case WINFB_LOG_INFO:  return "INFO ";
    case WINFB_LOG_DEBUG: return "DEBUG";
    case WINFB_LOG_TRACE: return "TRACE";
    default:              return "?????";
    }
}

static void log_rollover(void)
{
    if (!g_log_fp) return;
    fclose(g_log_fp);
    g_log_fp = NULL;
    g_log_lines = 0;
    /* single-generation rollover: .log -> .log.1 */
    char bak[MAX_PATH];
    snprintf(bak, MAX_PATH, "%s.1", g_log_path);
    /* ignore failure -- best effort */
    MoveFileExA(g_log_path, bak, MOVEFILE_REPLACE_EXISTING);
    g_log_fp = fopen(g_log_path, "a");
    if (g_log_fp) {
        setvbuf(g_log_fp, NULL, _IOFBF, 8192);
    }
}

static void winfb_log_init(int level, const char *path)
{
    g_log_level = level;
    if (level <= WINFB_LOG_OFF) return;

    if (path && path[0]) {
        strncpy(g_log_path, path, MAX_PATH - 1);
        g_log_path[MAX_PATH - 1] = '\0';
    } else {
        /* default: beside the .exe */
        GetModuleFileNameA(NULL, g_log_path, MAX_PATH);
        char *slash = strrchr(g_log_path, '\\');
        if (slash) slash[1] = '\0'; else g_log_path[0] = '\0';
        strncat(g_log_path, "fontfallback.log",
                MAX_PATH - strlen(g_log_path) - 1);
    }

    g_log_fp = fopen(g_log_path, "a");
    if (g_log_fp) {
        setvbuf(g_log_fp, NULL, _IOFBF, 8192);
        g_log_lines = 0;
    }
}

void winfb_log_close(void)
{
    if (g_log_fp) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    g_log_level = WINFB_LOG_OFF;
}

static void winfb_logf(int level, const char *fmt, ...)
{
    if (level > g_log_level) return;
    if (!g_log_fp) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log_fp,
            "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            level_name(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', g_log_fp);

    g_log_lines++;
    if (level >= WINFB_LOG_DEBUG || (g_log_lines % WINFB_LOG_FLUSH_EVERY == 0))
        fflush(g_log_fp);
    if (g_log_lines >= WINFB_LOG_MAX_LINES)
        log_rollover();
}

/* ===================================================================
 *  Module state
 * =================================================================== */

#define WINFB_MAX_SLOTS   16
#define WINFB_ATTR_DIM     8   /* bold(2) x italic(2) x underline(2) */
#define WINFB_SMP_CAP   1024
#define WINFB_OVR_CAP     64

typedef struct {
    char  name[LF_FACESIZE];
    HFONT hfont[WINFB_ATTR_DIM];   /* lazily created */
    bool  installed;                /* EnumFontFamiliesEx confirmed */
} winfb_slot;

static HFONT      g_probe_primary_hfont;
static winfb_slot g_slots[WINFB_MAX_SLOTS];
static int        g_n_slots;
static LOGFONT    g_base_lf;
static int        g_cell_w, g_cell_h;
static HDC        g_probe_dc;
static bool       g_initialised;

/* BMP codepoint -> slot index cache.
 * -2 = not probed yet, -1 = primary (or all-miss), 0..N-1 = fallback slot */
static int8_t g_bmp_map[0x10000];

typedef struct { uint32_t cp; int8_t slot; } winfb_smp_ent;
static winfb_smp_ent g_smp[WINFB_SMP_CAP];
static int g_n_smp;

/* Override ranges */
typedef struct { uint32_t lo, hi; int slot; } winfb_override;
static winfb_override g_ovr[WINFB_OVR_CAP];
static int g_n_ovr;

/* ===================================================================
 *  Configuration ([FontFallback] in kitty.ini, stored by LoadParameters)
 * =================================================================== */

static int  g_active_flag = 1;              /* active= (default yes) */
static char g_cfg_fallback[1024];           /* fallback= CSV */
static char g_cfg_overrides[2048];          /* override= ';'-separated */

void SetFontFallbackFlag(int flag) { g_active_flag = flag ? 1 : 0; }
int  GetFontFallbackFlag(void)     { return g_active_flag; }

void winfb_config_set(const char *fallback_csv, const char *overrides,
                      const char *log_level, const char *log_file)
{
    g_cfg_fallback[0] = '\0';
    if (fallback_csv) {
        strncpy(g_cfg_fallback, fallback_csv, sizeof(g_cfg_fallback) - 1);
        g_cfg_fallback[sizeof(g_cfg_fallback) - 1] = '\0';
    }
    g_cfg_overrides[0] = '\0';
    if (overrides) {
        strncpy(g_cfg_overrides, overrides, sizeof(g_cfg_overrides) - 1);
        g_cfg_overrides[sizeof(g_cfg_overrides) - 1] = '\0';
    }

    int level = WINFB_LOG_OFF;
    if (log_level && log_level[0]) {
        if      (!stricmp(log_level, "error")) level = WINFB_LOG_ERROR;
        else if (!stricmp(log_level, "warn"))  level = WINFB_LOG_WARN;
        else if (!stricmp(log_level, "info"))  level = WINFB_LOG_INFO;
        else if (!stricmp(log_level, "debug")) level = WINFB_LOG_DEBUG;
        else if (!stricmp(log_level, "trace")) level = WINFB_LOG_TRACE;
    }
    winfb_log_close();
    winfb_log_init(level, log_file);
}

/* ===================================================================
 *  Built-in fallback font list
 *
 *  Curated for this port: symbol/monospace fonts that ship with Windows
 *  10/11 first, the CJK UI fonts after.  Fonts not installed are dropped
 *  at init time, so unavailable entries cost nothing.
 * =================================================================== */

static const char *g_default_fallbacks[] = {
    "Cascadia Mono",
    "Cascadia Code",
    "Segoe UI Symbol",
    "Segoe UI Emoji",
    "Segoe UI Historic",
    "Microsoft YaHei UI",
    "Microsoft JhengHei UI",
    "Yu Gothic UI",
    "Malgun Gothic",
    NULL
};

/* ===================================================================
 *  Font enumeration: check if a font face is installed
 * =================================================================== */

/* Copy a (possibly oversized) NUL-terminated face name into an
 * LF_FACESIZE buffer, guaranteeing termination.  strncpy here would
 * trigger GCC's -Wstringop-truncation, hence bounded memcpy. */
static void copy_face(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > LF_FACESIZE - 1) n = LF_FACESIZE - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

typedef struct {
    const char *target_name;
    BOOL        found;
} enum_cb_ctx;

static int CALLBACK enum_font_cb(const LOGFONT *lf, const TEXTMETRIC *tm,
                                 DWORD type, LPARAM lParam)
{
    (void)tm; (void)type;
    enum_cb_ctx *ctx = (enum_cb_ctx *)lParam;
    if (stricmp(ctx->target_name, lf->lfFaceName) == 0) {
        ctx->found = TRUE;
        return 0;  /* stop enumeration */
    }
    return 1;  /* continue */
}

static bool is_font_installed(HDC hdc, const char *name)
{
    enum_cb_ctx ctx;
    ctx.target_name = name;
    ctx.found = FALSE;
    LOGFONT lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    copy_face(lf.lfFaceName, name);
    EnumFontFamiliesExA(hdc, &lf, enum_font_cb, (LPARAM)&ctx, 0);
    return ctx.found != FALSE;
}

/* ===================================================================
 *  HFONT creation helpers
 * =================================================================== */

static int attr_index(bool bold, bool italic, bool underline)
{
    return (bold ? 4 : 0) | (italic ? 2 : 0) | (underline ? 1 : 0);
}

static HFONT create_variant(const LOGFONT *base, int cell_w, int cell_h,
                             const char *face, bool bold, bool italic,
                             bool underline)
{
    LOGFONT lf = *base;
    copy_face(lf.lfFaceName, face);
    lf.lfHeight = cell_h;
    lf.lfWidth  = cell_w;
    lf.lfWeight = bold ? FW_BOLD : FW_DONTCARE;
    lf.lfItalic = italic ? TRUE : FALSE;
    lf.lfUnderline = underline ? TRUE : FALSE;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = base->lfQuality;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    return CreateFontIndirect(&lf);
}

/* Return the HFONT for a fallback slot + attribute combination, creating
 * it on first use.  NULL for slot < 0 (primary -- caller keeps its font). */
static HFONT winfb_hfont(int slot, bool bold, bool italic, bool underline)
{
    if (slot < 0 || slot >= g_n_slots) return NULL;

    int idx = attr_index(bold, italic, underline);
    if (!g_slots[slot].hfont[idx]) {
        g_slots[slot].hfont[idx] = create_variant(
            &g_base_lf, g_cell_w, g_cell_h,
            g_slots[slot].name, bold, italic, underline);
    }
    return g_slots[slot].hfont[idx];
}

/* ===================================================================
 *  CSV / override parsers
 * =================================================================== */

/* Parse comma-separated font names into the slot array.
 * If csv starts with '!', the built-in list is replaced. */
static void parse_fallback_csv(const char *csv, bool *replace_builtins)
{
    if (!csv || !csv[0]) return;
    const char *p = csv;
    if (*p == '!') {
        *replace_builtins = true;
        p++;
        while (*p == ' ') p++;
    }
    /* tokenize by comma */
    char buf[LF_FACESIZE];
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int i = 0;
        while (*p && *p != ',' && i < LF_FACESIZE - 1) {
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        /* trim trailing space */
        while (i > 0 && (buf[i-1] == ' ' || buf[i-1] == '\t')) buf[--i] = '\0';
        if (buf[0] && g_n_slots < WINFB_MAX_SLOTS) {
            copy_face(g_slots[g_n_slots].name, buf);
            g_slots[g_n_slots].installed = false; /* checked later */
            memset(g_slots[g_n_slots].hfont, 0, sizeof(HFONT) * WINFB_ATTR_DIM);
            g_n_slots++;
        }
        if (*p == ',') p++;
    }
}

/* Parse "XXXX-YYYY:FontName" or "XXXX:FontName" lines (hex codepoints). */
static void parse_overrides(const char **lines, int n)
{
    int i;
    for (i = 0; i < n && g_n_ovr < WINFB_OVR_CAP; i++) {
        const char *s = lines[i];
        if (!s) continue;
        uint32_t lo = 0, hi = 0;
        int consumed = 0;
        if (sscanf(s, "%x-%x:%n", &lo, &hi, &consumed) == 2 && consumed > 0) {
            /* range */
        } else if (sscanf(s, "%x:%n", &lo, &consumed) == 1 && consumed > 0) {
            hi = lo;
        } else {
            continue;
        }
        const char *fname = s + consumed;
        while (*fname == ' ') fname++;
        if (!*fname) continue;

        /* find or create the slot for this font name */
        int slot;
        for (slot = 0; slot < g_n_slots; slot++) {
            if (stricmp(g_slots[slot].name, fname) == 0) break;
        }
        if (slot == g_n_slots) {
            if (g_n_slots >= WINFB_MAX_SLOTS) continue;
            copy_face(g_slots[g_n_slots].name, fname);
            memset(g_slots[g_n_slots].hfont, 0, sizeof(HFONT) * WINFB_ATTR_DIM);
            g_slots[g_n_slots].installed = false;
            slot = g_n_slots++;
        }
        g_ovr[g_n_ovr].lo   = lo;
        g_ovr[g_n_ovr].hi   = hi;
        g_ovr[g_n_ovr].slot = slot;
        g_n_ovr++;
        winfb_logf(WINFB_LOG_INFO, "override U+%04X-U+%04X -> slot %d \"%s\"",
                   lo, hi, slot, g_slots[slot].name);
    }
}

/* ===================================================================
 *  Lookup: codepoint -> fallback slot
 * =================================================================== */

/* Returns -1 = primary font (confirmed present, or nobody has it),
 * 0..N-1 = fallback slot index.  Results are cached. */
static int winfb_lookup_slot(uint32_t cp)
{
    if (!g_initialised) return -1;

    /* 1. override ranges (highest priority) */
    for (int i = 0; i < g_n_ovr; i++) {
        if (cp >= g_ovr[i].lo && cp <= g_ovr[i].hi)
            return g_ovr[i].slot;
    }

    /* 2. cache lookup */
    if (cp < 0x10000) {
        int8_t cached = g_bmp_map[cp];
        if (cached != -2) return (int)cached;
    } else {
        for (int i = 0; i < g_n_smp; i++) {
            if (g_smp[i].cp == cp) return (int)g_smp[i].slot;
        }
    }

    /* 3. probe primary font (BMP only) */
    if (cp < 0x10000 && g_probe_dc) {
        /* Note: probe DC has the primary HFONT selected at init time. */
        wchar_t wc = (wchar_t)cp;
        WORD gi = 0;
        if (GetGlyphIndicesW(g_probe_dc, &wc, 1, &gi,
                             GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR
            && gi != 0xFFFF) {
            g_bmp_map[cp] = -1;
            winfb_logf(WINFB_LOG_TRACE, "probe U+%04X -> primary", cp);
            return -1;
        }
    }

    /* 4. probe fallback slots */
    for (int i = 0; i < g_n_slots; i++) {
        HFONT hf = winfb_hfont(i, false, false, false);
        if (!hf) continue;
        HFONT old = SelectObject(g_probe_dc, hf);

        wchar_t wc_buf[2];
        int wc_len;
        if (cp < 0x10000) {
            wc_buf[0] = (wchar_t)cp;
            wc_len = 1;
        } else {
            uint32_t v = cp - 0x10000;
            wc_buf[0] = (wchar_t)(0xD800 + (v >> 10));
            wc_buf[1] = (wchar_t)(0xDC00 + (v & 0x3FF));
            wc_len = 2;
        }
        WORD gi[2] = { 0, 0 };
        DWORD ret = GetGlyphIndicesW(g_probe_dc, wc_buf, wc_len, gi,
                                     GGI_MARK_NONEXISTING_GLYPHS);
        SelectObject(g_probe_dc, old);

        bool ok = (ret != GDI_ERROR) && (gi[0] != 0xFFFF)
                  && (wc_len == 1 || gi[1] != 0xFFFF);
        if (ok) {
            if (cp < 0x10000) {
                g_bmp_map[cp] = (int8_t)i;
            } else if (g_n_smp < WINFB_SMP_CAP) {
                g_smp[g_n_smp].cp   = cp;
                g_smp[g_n_smp].slot = (int8_t)i;
                g_n_smp++;
            }
            winfb_logf(WINFB_LOG_INFO, "map U+%04X -> slot %d \"%s\"",
                       cp, i, g_slots[i].name);
            return i;
        }
    }

    /* 5. all-miss: record as primary (shows .notdef but avoids re-probing) */
    if (cp < 0x10000) {
        g_bmp_map[cp] = -1;
    } else if (g_n_smp < WINFB_SMP_CAP) {
        g_smp[g_n_smp].cp   = cp;
        g_smp[g_n_smp].slot = -1;
        g_n_smp++;
    }
    winfb_logf(WINFB_LOG_TRACE, "probe U+%04X -> all-miss (primary .notdef)", cp);
    return -1;
}

/* ===================================================================
 *  Lifecycle
 * =================================================================== */

static void winfb_init(HDC hdc, const LOGFONT *primary,
                       int cell_w, int cell_h,
                       const char *fallback_csv,
                       const char **override_lines, int n_override)
{
    winfb_cleanup();  /* idempotent */

    g_base_lf = *primary;
    g_cell_w  = cell_w;
    g_cell_h  = cell_h;
    g_n_slots = 0;
    g_n_smp   = 0;
    g_n_ovr   = 0;
    g_initialised = false;

    /* create a compatible DC for glyph probing */
    g_probe_dc = CreateCompatibleDC(hdc);
    if (!g_probe_dc) {
        winfb_logf(WINFB_LOG_ERROR, "winfb_init: CreateCompatibleDC failed");
        return;
    }

    /* select the primary font into the probe DC; the HFONT is retained for
     * the DC's lifetime and deleted by winfb_cleanup. */
    g_probe_primary_hfont = CreateFontIndirect(primary);
    if (!g_probe_primary_hfont) {
        winfb_logf(WINFB_LOG_ERROR,
                   "winfb_init: CreateFontIndirect failed for primary font; aborting init");
        DeleteDC(g_probe_dc);
        g_probe_dc = NULL;
        return;
    }
    SelectObject(g_probe_dc, g_probe_primary_hfont);

    /* Raster (non-TrueType) primary fonts: GetGlyphIndicesW always fails on
     * them, so the primary-font probe in winfb_lookup_slot could never
     * confirm a glyph and every character -- ASCII included -- would drift
     * to the first fallback font.  Probe one letter up front: if the
     * primary font can't answer, disable the subsystem for this font. */
    {
        wchar_t wc = L'A';
        WORD gi = 0;
        if (GetGlyphIndicesW(g_probe_dc, &wc, 1, &gi,
                             GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) {
            winfb_logf(WINFB_LOG_WARN,
                       "winfb_init: primary font \"%s\" is not glyph-probeable "
                       "(raster font?) - fallback disabled for this font",
                       primary->lfFaceName);
            winfb_cleanup();
            return;
        }
    }

    /* reset BMP cache to "not probed" */
    memset(g_bmp_map, -2, sizeof(g_bmp_map));  /* -2 == 0xFE as int8 */

    /* parse user fallback list */
    bool replace_builtins = false;
    parse_fallback_csv(fallback_csv, &replace_builtins);

    /* if not replacing, append built-in defaults after the user's slots,
     * skipping any builtin the user already listed (case-insensitive) so
     * the user's ordering takes precedence. */
    if (!replace_builtins) {
        for (int b = 0; g_default_fallbacks[b]; b++) {
            if (g_n_slots >= WINFB_MAX_SLOTS) break;
            bool dup = false;
            for (int j = 0; j < g_n_slots; j++) {
                if (stricmp(g_slots[j].name, g_default_fallbacks[b]) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            copy_face(g_slots[g_n_slots].name, g_default_fallbacks[b]);
            memset(g_slots[g_n_slots].hfont, 0, sizeof(HFONT) * WINFB_ATTR_DIM);
            g_slots[g_n_slots].installed = false;
            g_n_slots++;
        }
    }

    /* parse overrides BEFORE the install check, so override-added slots
     * get the same installed-check + compact treatment. */
    if (override_lines && n_override > 0)
        parse_overrides(override_lines, n_override);

    /* verify which fallback fonts are actually installed */
    for (int i = 0; i < g_n_slots; i++) {
        g_slots[i].installed = is_font_installed(hdc, g_slots[i].name);
        if (g_slots[i].installed) {
            winfb_logf(WINFB_LOG_INFO, "fallback slot %d: \"%s\" (installed)",
                       i, g_slots[i].name);
        } else {
            winfb_logf(WINFB_LOG_WARN,
                       "fallback slot %d: \"%s\" NOT installed, skipping",
                       i, g_slots[i].name);
        }
    }

    /* compact: remove uninstalled fonts, adjust override slot indices */
    int dst = 0;
    int remap[WINFB_MAX_SLOTS];
    for (int i = 0; i < g_n_slots; i++) {
        remap[i] = -1;
        if (g_slots[i].installed) {
            if (dst != i) g_slots[dst] = g_slots[i];
            remap[i] = dst;
            dst++;
        }
    }
    g_n_slots = dst;
    for (int i = 0; i < g_n_ovr; i++) {
        g_ovr[i].slot = (g_ovr[i].slot >= 0 && g_ovr[i].slot < WINFB_MAX_SLOTS)
                        ? remap[g_ovr[i].slot] : -1;
        if (g_ovr[i].slot < 0) {
            /* override pointed to an uninstalled font, drop it */
            g_ovr[i] = g_ovr[g_n_ovr - 1];
            g_n_ovr--;
            i--;
        }
    }

    winfb_logf(WINFB_LOG_INFO,
               "winfb_init: primary=\"%s\" cell=%dx%d fallback_slots=%d overrides=%d",
               primary->lfFaceName, cell_w, cell_h, g_n_slots, g_n_ovr);

    g_initialised = true;
}

void winfb_reinit_from_config(HDC hdc, const LOGFONT *primary,
                              int cell_w, int cell_h)
{
    if (!g_active_flag) {
        winfb_cleanup();
        return;
    }

    /* split the stored override= value on ';' into line pointers */
    const char *ovr_lines[WINFB_OVR_CAP];
    int n_ovr = 0;
    char ovr_buf[sizeof(g_cfg_overrides)];
    memcpy(ovr_buf, g_cfg_overrides, sizeof(ovr_buf));
    char *p = ovr_buf;
    while (*p && n_ovr < WINFB_OVR_CAP) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        ovr_lines[n_ovr++] = p;
        char *sep = strchr(p, ';');
        if (!sep) break;
        *sep = '\0';
        p = sep + 1;
    }

    winfb_init(hdc, primary, cell_w, cell_h,
               g_cfg_fallback[0] ? g_cfg_fallback : NULL,
               n_ovr > 0 ? ovr_lines : NULL, n_ovr);
}

void winfb_cleanup(void)
{
    /* free fallback HFONTs and clear caches */
    if (g_initialised) {
        for (int i = 0; i < g_n_slots; i++) {
            for (int j = 0; j < WINFB_ATTR_DIM; j++) {
                if (g_slots[i].hfont[j]) {
                    DeleteObject(g_slots[i].hfont[j]);
                    g_slots[i].hfont[j] = NULL;
                }
            }
        }
        memset(g_bmp_map, -2, sizeof(g_bmp_map));
        g_n_smp = 0;
    }

    /* tear down the probe DC first (deselects its font), then delete the
     * primary HFONT retained for the DC's lifetime. */
    if (g_probe_dc) {
        SelectObject(g_probe_dc, GetStockObject(SYSTEM_FONT));
        DeleteDC(g_probe_dc);
        g_probe_dc = NULL;
    }
    if (g_probe_primary_hfont) {
        DeleteObject(g_probe_primary_hfont);
        g_probe_primary_hfont = NULL;
    }

    g_n_slots = 0;
    g_n_smp   = 0;
    g_n_ovr   = 0;
    g_initialised = false;

    winfb_logf(WINFB_LOG_INFO, "winfb_cleanup: released all resources");
}

/* ===================================================================
 *  Splitter: wbuf -> runs grouped by fallback slot
 * =================================================================== */

int winfb_split(const wchar_t *wbuf, int len,
                WinFB_Run *out, int max_runs)
{
    if (len <= 0 || max_runs <= 0) return 0;
    if (!g_initialised || g_n_slots == 0) {
        out[0].start = 0;
        out[0].len   = len;
        out[0].slot  = -1;
        return 1;
    }

    int nruns = 0;
    int i = 0;

    while (i < len) {
        /* decode codepoint (handle surrogate pairs) */
        uint32_t cp;
        int advance;
        if (i + 1 < len
            && wbuf[i]   >= 0xD800 && wbuf[i]   <= 0xDBFF
            && wbuf[i+1] >= 0xDC00 && wbuf[i+1] <= 0xDFFF) {
            cp = 0x10000
                 + ((uint32_t)(wbuf[i]   - 0xD800) << 10)
                 +  (uint32_t)(wbuf[i+1] - 0xDC00);
            advance = 2;
        } else {
            cp = (uint32_t)wbuf[i];
            advance = 1;
        }

        int slot = winfb_lookup_slot(cp);

        /* extend the current run if the slot matches, else start a new one;
         * when the run table is nearly full, spend the last entry on a
         * primary-font run covering everything left, so the output always
         * spans the whole buffer (a .notdef box beats dropped text). */
        if (nruns > 0 && out[nruns-1].slot == slot) {
            out[nruns-1].len += advance;
        } else if (nruns < max_runs - 1) {
            out[nruns].start = i;
            out[nruns].len   = advance;
            out[nruns].slot  = slot;
            nruns++;
        } else {
            out[nruns].start = i;
            out[nruns].len   = len - i;
            out[nruns].slot  = -1;
            nruns++;
            break;
        }
        i += advance;
    }

    winfb_logf(WINFB_LOG_DEBUG, "split len=%d runs=%d", len, nruns);
    return nruns;
}

/* ===================================================================
 *  Run drawer: paint each run with its chosen HFONT
 * =================================================================== */

void winfb_draw_runs(HDC hdc, int x, int y, const RECT *line_box,
                     const wchar_t *wbuf, int len, const int *lpDx,
                     const WinFB_Run *runs, int nruns,
                     bool opaque,
                     bool bold, bool italic, bool underline)
{
    (void)len;

    int orig_bkmode = GetBkMode(hdc);
    HFONT primary_hf = (HFONT)GetCurrentObject(hdc, OBJ_FONT);

    int x_cursor = x;

    for (int r = 0; r < nruns; r++) {
        const WinFB_Run *run = &runs[r];

        if (run->slot >= 0) {
            HFONT hf = winfb_hfont(run->slot, bold, italic, underline);
            if (hf) SelectObject(hdc, hf);
            /* If hf is NULL (HFONT creation failed) we draw with the
             * primary font -- better than skipping the glyphs. */
        }

        UINT eto_flags = ETO_CLIPPED;
        if (opaque && r == 0) eto_flags |= ETO_OPAQUE;

        ExtTextOutW(hdc, x_cursor, y, eto_flags, line_box,
                    wbuf + run->start, run->len,
                    lpDx ? lpDx + run->start : NULL);

        /* restore the primary font after a non-primary run */
        if (run->slot >= 0) {
            SelectObject(hdc, primary_hf);
        }

        /* advance the cursor by the run's cell widths.  A NULL lpDx means
         * the variable-pitch path, which draws one char per call and thus
         * one run -- no advance needed. */
        if (lpDx) {
            for (int k = 0; k < run->len; k++) {
                x_cursor += lpDx[run->start + k];
            }
        }

        /* after the first run, don't re-erase the background */
        if (r == 0) SetBkMode(hdc, TRANSPARENT);
    }

    /* restore bkmode for the caller */
    SetBkMode(hdc, orig_bkmode);
}
