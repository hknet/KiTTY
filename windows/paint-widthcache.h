/*
 * paint-widthcache.h - the character widths a painter has measured, kept.
 *
 * The terminal needs the width of every changed non-ASCII cell (terminal.c
 * do_paint -> window.c wintw_char_width -> kp_char_width), which is a font
 * call per cell: SelectObject + GetCharWidth32 under GDI, a glyph lookup +
 * glyph metrics under DirectWrite. The result depends only on the font, the
 * code point and the narrow/wide flag. Each painter keeps what it measured
 * in this table and returns repeats from it.
 *
 * Direct-mapped: a slot holds the last (font, code point, wide) that hashed
 * to it. A collision simply measures again. Emptied whenever the window
 * takes new fonts (the painter's fonts_changed): HFONT values are recycled,
 * so an old entry could describe another font.
 */
#ifndef KITTY_PAINT_WIDTHCACHE_H
#define KITTY_PAINT_WIDTHCACHE_H

#include <stdint.h>
#include <string.h>

#define KITTY_WIDTHCACHE_SLOTS 4096        /* a power of two */

typedef struct KittyWidthEntry {
    HFONT font;
    unsigned ch;
    int width;
    unsigned char wide, used;
} KittyWidthEntry;

typedef struct KittyWidthCache {
    KittyWidthEntry *e;                    /* NULL until the first answer */
} KittyWidthCache;

static inline KittyWidthEntry *kitty_wc_slot(KittyWidthCache *c, HFONT f,
                                             unsigned ch, bool wide)
{
    uintptr_t x = (uintptr_t)f ^ ((uintptr_t)ch * 2654435761u) ^
                  (wide ? 0x9e3779b9u : 0);
    x ^= x >> 15;
    return &c->e[x & (KITTY_WIDTHCACHE_SLOTS - 1)];
}

static inline bool kitty_wc_get(KittyWidthCache *c, HFONT f, unsigned ch,
                                bool wide, int *width)
{
    KittyWidthEntry *e;
    if (!c->e)
        return false;
    e = kitty_wc_slot(c, f, ch, wide);
    if (!e->used || e->font != f || e->ch != ch || e->wide != (wide ? 1 : 0))
        return false;
    *width = e->width;
    return true;
}

static inline void kitty_wc_put(KittyWidthCache *c, HFONT f, unsigned ch,
                                bool wide, int width)
{
    KittyWidthEntry *e;
    if (!c->e) {
        c->e = snewn(KITTY_WIDTHCACHE_SLOTS, KittyWidthEntry);
        memset(c->e, 0, KITTY_WIDTHCACHE_SLOTS * sizeof(KittyWidthEntry));
    }
    e = kitty_wc_slot(c, f, ch, wide);
    e->font = f;
    e->ch = ch;
    e->width = width;
    e->wide = wide ? 1 : 0;
    e->used = 1;
}

static inline void kitty_wc_clear(KittyWidthCache *c)
{
    if (c->e)
        memset(c->e, 0, KITTY_WIDTHCACHE_SLOTS * sizeof(KittyWidthEntry));
}

static inline void kitty_wc_free(KittyWidthCache *c)
{
    sfree(c->e);
    c->e = NULL;
}

#endif
