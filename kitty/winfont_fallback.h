/*
 * winfont_fallback.h -- per-codepoint font fallback for KiTTY (GDI).
 *
 * Ported from upstream PR cyd01/KiTTY#555 (author: blreay), adapted to the
 * 0.84 tree: configuration is stored here and fed from kitty.ini
 * [FontFallback] by LoadParameters (kitty.c), and windows/window.c hooks the
 * module around its single general_textout call site.
 *
 * When the primary terminal font lacks a glyph (box drawing, Nerd Font PUA,
 * CJK, symbols...), the module probes a configurable list of fallback fonts
 * (GetGlyphIndicesW on a private probe DC, results cached) and draws those
 * characters from the first font that has them.  Cell widths stay
 * Unicode-driven; fallback glyphs are clipped to their cells.  Monochrome
 * GDI rendering only -- no colour emoji.
 */

#ifndef WINFONT_FALLBACK_H
#define WINFONT_FALLBACK_H

#include <windows.h>
#include <stdbool.h>

/* Maximum per-font runs a single text chunk is split into; window.c sizes
 * its stack array with this. If a chunk needs more (pathological per-char
 * font alternation), the tail is drawn with the primary font. */
#define WINFB_MAX_RUNS 64

/* One run returned by winfb_split. */
typedef struct {
    int start;   /* index into wbuf */
    int len;     /* wchar count (a surrogate pair counts as 2) */
    int slot;    /* -1 = primary font, 0..N-1 = fallback slot */
} WinFB_Run;

/* ---- configuration (called from LoadParameters, kitty.c) ---- */

/* [FontFallback] active= master switch (default yes). */
void SetFontFallbackFlag(int flag);
int  GetFontFallbackFlag(void);

/* Store the [FontFallback] string settings in one shot.  Any argument may
 * be NULL or empty ("unset").
 *   fallback_csv -- "fallback=": comma-separated font names; leading '!'
 *                   replaces the built-in defaults instead of preceding them
 *   overrides    -- "override=": "lo-hi:Font" or "cp:Font" hex ranges,
 *                   ';'-separated
 *   log_level    -- "log=": off|error|warn|info|debug|trace
 *   log_file     -- "logfile=": log path (default fontfallback.log next to
 *                   the executable) */
void winfb_config_set(const char *fallback_csv, const char *overrides,
                      const char *log_level, const char *log_file);

/* ---- lifecycle (called from windows/window.c) ---- */

/* (Re)build the fallback state against the current primary font, from the
 * stored configuration.  Call at the end of init_fonts, with its DC.
 * Cleans up any previous state first; no-op (beyond cleanup) when
 * active=no or the primary font cannot be glyph-probed (raster fonts). */
void winfb_reinit_from_config(HDC hdc, const LOGFONT *primary,
                              int cell_w, int cell_h);

/* Free all GDI resources and caches (deinit_fonts / shutdown). */
void winfb_cleanup(void);

/* Flush and close the log file (cleanup_exit). */
void winfb_log_close(void);

/* ---- rendering (called from do_text_internal) ---- */

/* Split wbuf[0..len-1] into contiguous runs sharing one font slot.
 * Always covers the full range.  Returns the number of runs written
 * (<= max_runs).  When the module is inactive this returns a single
 * primary-font run, so the caller's fast path is unaffected. */
int winfb_split(const wchar_t *wbuf, int len,
                WinFB_Run *out, int max_runs);

/* Draw the runs, selecting each fallback run's HFONT (lazily created per
 * bold/italic/underline variant) and restoring the DC's font afterwards.
 * Mirrors ExtTextOutW semantics: ETO_CLIPPED into line_box, ETO_OPAQUE on
 * the first run only when opaque, lpDx as per-char advances (may be NULL
 * for the single-character variable-pitch path). */
void winfb_draw_runs(HDC hdc, int x, int y, const RECT *line_box,
                     const wchar_t *wbuf, int len, const int *lpDx,
                     const WinFB_Run *runs, int nruns,
                     bool opaque,
                     bool bold, bool italic, bool underline);

/* The configured slot list, for a painter with its own text engine
 * (windows/paint-d2d.c): how many fallback fonts are configured, and the
 * name of each in order. Empty until winfb_reinit_from_config ran. */
int winfb_slot_count(void);
const char *winfb_slot_name(int i);

#endif /* WINFONT_FALLBACK_H */
