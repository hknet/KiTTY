/*
 * Implementation of Filename for Windows.
 */

#include <wchar.h>

#include "putty.h"

/*
 * KiTTY: %VAR% expansion (cyd01/KiTTY#472).
 *
 * A Filename's wpath/cpath/utf8path stay exactly what was typed or loaded -
 * that is what write_setting_filename() saves and what the config box's
 * file-select box displays (dlg_filesel_set() shows fn->wpath), so expanding
 * here would turn the FIRST save after any edit into the resolved path,
 * silently losing the %VAR% form. Expansion instead happens on demand, right
 * before a path is handed to a real Win32 API - filename_expand_wstr() /
 * filename_expand_str() below, used by f_open() and by every other place
 * that opens, stats or launches a Filename-typed setting directly.
 *
 * ExpandEnvironmentStringsW leaves an unresolved %name% (and a lone '%')
 * untouched, so a filename that never had a variable in it is unaffected;
 * this is a single, non-recursive pass, so a '%' that arrives INSIDE an
 * expanded value (e.g. a variable whose value itself contains '%') is never
 * expanded again.
 */
static wchar_t *expand_env_wstr(const wchar_t *in)
{
    DWORD need = ExpandEnvironmentStringsW(in, NULL, 0);
    wchar_t *out;

    if (!need)
        return dupwcs(in);             /* expansion failed: use it literally */

    out = snewn(need, wchar_t);
    if (!ExpandEnvironmentStringsW(in, out, need)) {
        sfree(out);
        return dupwcs(in);
    }
    return out;
}

wchar_t *filename_expand_wstr(const Filename *fn)
{
    return expand_env_wstr(fn->wpath);
}

char *filename_expand_str(const Filename *fn)
{
    wchar_t *w = expand_env_wstr(fn->wpath);
    char *c = dup_wc_to_mb(DEFAULT_CODEPAGE, w, "?");
    sfree(w);
    return c;
}

Filename *filename_from_str(const char *str)
{
    Filename *fn = snew(Filename);
    fn->cpath = dupstr(str);
    fn->wpath = dup_mb_to_wc(DEFAULT_CODEPAGE, fn->cpath);
    fn->utf8path = encode_wide_string_as_utf8(fn->wpath);
    return fn;
}

Filename *filename_from_wstr(const wchar_t *str)
{
    Filename *fn = snew(Filename);
    fn->wpath = dupwcs(str);
    fn->cpath = dup_wc_to_mb(DEFAULT_CODEPAGE, fn->wpath, "?");
    fn->utf8path = encode_wide_string_as_utf8(fn->wpath);
    return fn;
}

Filename *filename_from_utf8(const char *ustr)
{
    Filename *fn = snew(Filename);
    fn->utf8path = dupstr(ustr);
    fn->wpath = decode_utf8_to_wide_string(fn->utf8path);
    fn->cpath = dup_wc_to_mb(DEFAULT_CODEPAGE, fn->wpath, "?");
    return fn;
}

Filename *filename_copy(const Filename *fn)
{
    Filename *newfn = snew(Filename);
    newfn->cpath = dupstr(fn->cpath);
    newfn->wpath = dupwcs(fn->wpath);
    newfn->utf8path = dupstr(fn->utf8path);
    return newfn;
}

const char *filename_to_str(const Filename *fn)
{
    return fn->cpath;                  /* FIXME */
}

const wchar_t *filename_to_wstr(const Filename *fn)
{
    return fn->wpath;
}

bool filename_equal(const Filename *f1, const Filename *f2)
{
    /* wpath is primary: two filenames refer to the same file if they
     * have the same wpath */
    return !wcscmp(f1->wpath, f2->wpath);
}

bool filename_is_null(const Filename *fn)
{
    return !*fn->wpath;
}

void filename_free(Filename *fn)
{
    sfree(fn->wpath);
    sfree(fn->cpath);
    sfree(fn->utf8path);
    sfree(fn);
}

void filename_serialise(BinarySink *bs, const Filename *f)
{
    put_asciz(bs, f->utf8path);
}
Filename *filename_deserialise(BinarySource *src)
{
    const char *utf8 = get_asciz(src);
    return filename_from_utf8(utf8);
}

char filename_char_sanitise(char c)
{
    if (strchr("<>:\"/\\|?*", c))
        return '.';
    return c;
}

FILE *f_open(const Filename *fn, const char *mode, bool isprivate)
{
#ifdef LEGACY_WINDOWS
    /* Fallback for legacy pre-NT windows, where as far as I can see
     * _wfopen just doesn't work at all */
    init_winver();
    if (osPlatformId == VER_PLATFORM_WIN32_WINDOWS ||
        osPlatformId == VER_PLATFORM_WIN32s)
        return fopen(fn->cpath, mode);
#endif

    wchar_t *wmode = dup_mb_to_wc(DEFAULT_CODEPAGE, mode);
    /* KiTTY: %VAR% expansion (cyd01/KiTTY#472) - expand right before the
     * actual open, so the Filename itself (and anything saved from it)
     * stays literal. */
    wchar_t *wpath = expand_env_wstr(fn->wpath);
    FILE *fp = _wfopen(wpath, wmode);
    sfree(wpath);
    sfree(wmode);
    return fp;
}
