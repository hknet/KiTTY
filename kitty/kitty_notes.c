/*
 * kitty_notes.c: the application notification.
 *
 * One note, stored once for the whole installation, shown by the FIRST window
 * every KiTTY++ process opens - the terminal, the launcher, or the
 * configuration box - in the notice window near the clock (kitty_notice.c),
 * with no timeout, until it is clicked.
 *
 * It used to be a modal MessageBox raised from the startup code before any
 * window existed, which stopped the program dead at every single start. The
 * startup code now only marks the notice as owed; whichever window comes up
 * first shows it and settles the flag, so a process that opens a terminal and
 * then a configuration box still shows it exactly once.
 *
 * ONE on the desktop at a time. The showing process holds a named mutex in
 * the logon session while the notice is up, keyed to the TEXT: a second
 * KiTTY++ started while the first one's notice is still there shows nothing
 * and stays quiet. A click, or the process ending, releases the mutex, so the
 * next start shows the note again. A later notice from the same process does
 * NOT release it: the note is parked by kitty_notice.c and comes back when
 * that notice goes (which is also why the slot must stay held meanwhile).
 *
 * SHOW ONCE ([KiTTY] notesonce). Off, the above is the whole story. On, the
 * CLICK also marks the note as seen for this desktop - a manual-reset event
 * Local\KiTTYNotes.<hash>.Seen, held open by the clicking process, and by the
 * running launcher, which is told to open one of its own. A process that
 * finds that event shows nothing and takes no slot. The mark dies with the
 * last holder, so "seen" lasts as long as the launcher runs, or as long as
 * the clicking process does when there is no launcher. A changed note is a
 * different hash, so it is a different mark and the new text is shown.
 *
 * STORAGE. [KiTTY] notes, one line, with the escapes the typed-text shortcuts
 * use and no others:
 *
 *   \n  line feed        \t  tab
 *   \r  carriage return  \\  a backslash
 *
 * and a backslash before anything else yields that character. Leading and
 * trailing spaces, and a note that is itself wrapped in quotes, are protected
 * by wrapping the stored value in double quotes: the kitty.ini reader trims
 * whitespace around an UNQUOTED value and strips one wrapping quote pair,
 * keeping what is between the quotes exactly as written (kitty/mini/mini.c,
 * mini_clean_value), so the wrap is precisely what survives it. The registry
 * twin holds the same wrapped one-line form, and the decoder takes the wrap
 * off again there.
 *
 * THE REGISTRY TWIN is the value the note has always lived in. In registry
 * save mode WriteParameter/ReadParameterN drop the section and use a flat
 * value named after the key under KiTTY's base key - and registry value names
 * are case-insensitive, so "notes" IS the old "Notes" value, with no
 * conversion step and nothing to migrate. That old value holds RAW text with
 * real CRLF line breaks in it (a MessageBox displayed it), so the decoder
 * passes real line breaks through unchanged and only expands the escapes: an
 * installed KiTTY keeps its note, and the first edit writes it back in the
 * one-line form.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kitty_notes.h"
#include "kitty_notice.h"
#include "kitty_inikeys.h"  /* KI_NOTES */
#include "kitty_text.h"     /* KT_CAP_KITTYPP: the notice's title */

/* kitty.c: the suite's global-parameter store (registry, or kitty.ini in
 * portable mode). Declared rather than included so this file keeps its short
 * include list; it is compiled only into the KiTTY targets. */
extern int WriteParameter(const char *key, const char *name, char *value);
extern int ReadParameterN(const char *key, const char *name, char *value, size_t size);
#ifndef INIT_SECTION
#define INIT_SECTION "KiTTY"
#endif

/* ReadParameterN answers into a 4096-byte buffer of its own, so that is the
 * ceiling on the STORED (escaped) note however long the field is. */
#define NOTES_STORED_MAX 4096
/* Escapes expand to at most one character each, so the decoded form never
 * needs more room than the stored one - except for the CR this adds to every
 * bare line feed, hence the doubling. */
#define NOTES_TEXT_MAX (NOTES_STORED_MAX * 2)

static char notes_running[NOTES_TEXT_MAX];
static int notes_have_running = 0;
static int notes_pending = 0;
static HANDLE notes_mutex = NULL;
/* The note this process actually put on screen, as its hash. A click counts
 * as having read THAT, whatever the field has been edited to since. */
static unsigned int notes_shown_hash = 0;

/* ---- the escape pair ---- */

/* Would the stored form be changed by the kitty.ini reader unless it is
 * wrapped in quotes? It trims whitespace around a value, and strips one pair
 * of quotes that wraps the whole value. Both cases are decided by the FIRST
 * and LAST characters, which is what makes the wrap reversible: the decoder
 * applies exactly the same test to what is left inside. */
static int notes_needs_wrap(const char *s)
{
    size_t len = s ? strlen(s) : 0;
    char first, last;
    if (len == 0)
        return 0;
    first = s[0];
    last = s[len - 1];
    if (first == ' ' || first == '\t' || last == ' ' || last == '\t')
        return 1;
    if (len >= 2 && (first == '"' || first == '\'') && last == first)
        return 1;
    return 0;
}

void kitty_notes_encode(const char *text, char *out, size_t outsize)
{
    char body[NOTES_STORED_MAX];
    size_t o = 0;
    const char *p;
    if (!out || outsize == 0)
        return;
    out[0] = '\0';
    if (!text)
        return;

    for (p = text; *p; p++) {
        char esc[2];
        int n;
        switch (*p) {
          case '\\': esc[0] = '\\'; esc[1] = '\\'; n = 2; break;
          case '\n': esc[0] = '\\'; esc[1] = 'n';  n = 2; break;
          case '\r': esc[0] = '\\'; esc[1] = 'r';  n = 2; break;
          case '\t': esc[0] = '\\'; esc[1] = 't';  n = 2; break;
          default:   esc[0] = *p;                  n = 1; break;
        }
        if (o + (size_t)n + 1 > sizeof(body))
            break;                      /* truncate on a whole escape */
        memcpy(body + o, esc, (size_t)n);
        o += (size_t)n;
    }
    body[o] = '\0';

    if (notes_needs_wrap(body))
        snprintf(out, outsize, "\"%s\"", body);
    else
        snprintf(out, outsize, "%s", body);
}

/* Decodes into CRLF line endings: a Win32 edit box needs them, and DrawText
 * is happy either way. A CRLF already in the input (the old registry value)
 * stays one CRLF; a bare CR or LF becomes one. */
static void notes_put(char *out, size_t outsize, size_t *o, char c)
{
    if (*o + 1 < outsize)
        out[(*o)++] = c;
}

void kitty_notes_decode(const char *stored, char *out, size_t outsize)
{
    char unwrapped[NOTES_STORED_MAX];
    size_t o = 0;
    const char *p;
    size_t len;
    if (!out || outsize == 0)
        return;
    out[0] = '\0';
    if (!stored)
        return;

    /* Undo the encoder's wrap, and only that: the inner text must be the kind
     * of text the encoder would have wrapped. In kitty.ini the reader has
     * already taken the pair off, so there is nothing here to do; in the
     * registry nothing did, so this is where it happens. */
    len = strlen(stored);
    if (len >= 2 && stored[0] == '"' && stored[len - 1] == '"' &&
        len - 2 < sizeof(unwrapped)) {
        memcpy(unwrapped, stored + 1, len - 2);
        unwrapped[len - 2] = '\0';
        if (notes_needs_wrap(unwrapped))
            stored = unwrapped;
    }

    for (p = stored; *p; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
              case 'n': c = '\n'; break;
              case 'r': c = '\r'; break;
              case 't': c = '\t'; break;
              default:  c = *p;   break;   /* \\, and anything else literally */
            }
            if (c == '\n') { notes_put(out, outsize, &o, '\r'); notes_put(out, outsize, &o, '\n'); }
            else if (c == '\r') { notes_put(out, outsize, &o, '\r'); notes_put(out, outsize, &o, '\n'); if (p[1] == '\\' && p[2] == 'n') p += 2; }
            else notes_put(out, outsize, &o, c);
            continue;
        }
        if (c == '\r') {
            notes_put(out, outsize, &o, '\r');
            notes_put(out, outsize, &o, '\n');
            if (p[1] == '\n') p++;          /* one CRLF, not two line breaks */
            continue;
        }
        if (c == '\n') {
            notes_put(out, outsize, &o, '\r');
            notes_put(out, outsize, &o, '\n');
            continue;
        }
        notes_put(out, outsize, &o, c);
    }
    out[o] = '\0';
}

/* ---- the note ---- */

int kitty_notes_get(char *buf, size_t size)
{
    char stored[NOTES_STORED_MAX];
    if (!buf || size == 0)
        return 0;
    buf[0] = '\0';
    if (notes_have_running) {
        snprintf(buf, size, "%s", notes_running);
        return buf[0] ? 1 : 0;
    }
    stored[0] = '\0';
    if (!ReadParameterN(INIT_SECTION, KI_NOTES, stored, sizeof(stored)) || !stored[0])
        return 0;
    kitty_notes_decode(stored, buf, size);
    return buf[0] ? 1 : 0;
}

void kitty_notes_set_running(const char *text)
{
    if (!text) {
        notes_have_running = 0;
        notes_running[0] = '\0';
        return;
    }
    snprintf(notes_running, sizeof(notes_running), "%s", text);
    notes_have_running = 1;
}

void kitty_notes_mark_pending(void)
{
    notes_pending = 1;
}

/* FNV-1a over the note: an identifier, not a secret. Two processes showing
 * the SAME note must agree on the name; a note that has been edited is a
 * different notice and may appear while the old one is still up. */
static unsigned int notes_hash(const char *text)
{
    unsigned int h = 2166136261u;
    const char *p;
    for (p = text; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return h;
}

/* Directory of the running EXE, lowercased, without a trailing slash: what
 * makes the "seen" broadcast reach only launchers of THIS install. */
static int notes_exedir(char *out, int len)
{
    char path[MAX_PATH + 1];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash;
    if (n == 0 || n >= MAX_PATH)
        return 0;
    path[n] = '\0';
    slash = strrchr(path, '\\');
    if (!slash)
        return 0;
    *slash = '\0';
    if ((int)strlen(path) >= len)
        return 0;
    strcpy(out, path);
    CharLowerA(out);
    return 1;
}

/* Install-keyed, the same rule the workplace broadcast follows:
 * RegisterWindowMessage on one string gives every process the same id, and
 * the install hash keeps a second installation's launcher out of it. */
unsigned int kitty_notes_seen_message(void)
{
    static UINT msg = 0;
    if (!msg) {
        char dir[MAX_PATH + 1], name[96];
        if (!notes_exedir(dir, sizeof(dir)))
            dir[0] = '\0';
        snprintf(name, sizeof(name), "KiTTYNotesSeen.%08x", notes_hash(dir));
        msg = RegisterWindowMessageA(name);
    }
    return msg;
}

/* [KiTTY] notesonce: does a click silence the note for this desktop? */
static int notes_once_enabled(void)
{
    char buf[16] = "";
    if (!ReadParameterN(INIT_SECTION, KI_NOTESONCE, buf, sizeof(buf)) || !buf[0])
        return 0;
    return (!_stricmp(buf, "yes") || !_stricmp(buf, "1") ||
            !_stricmp(buf, "true") || !_stricmp(buf, "on")) ? 1 : 0;
}

static void notes_seen_name(unsigned int hash, char *out, size_t size)
{
    snprintf(out, size, "Local\\KiTTYNotes.%08x.Seen", hash);
}

/* This process's share of the mark, and which note it is for. Kept for the
 * process's lifetime: the event exists while at least one holder does, and
 * that is exactly how long "the user has seen it" is meant to last.
 *
 * ONE at a time, but not for ever: edit the note and it is a different note
 * with a different name, and holding the old one open helps nobody. */
static HANDLE notes_seen = NULL;
static unsigned int notes_seen_hash = 0;

static void notes_seen_take(unsigned int hash)
{
    char name[80];
    if (notes_seen) {
        if (notes_seen_hash == hash)
            return;                     /* already holding this one */
        CloseHandle(notes_seen);
        notes_seen = NULL;
    }
    notes_seen_name(hash, name, sizeof(name));
    /* Manual-reset, unsignalled, no security descriptor: its NAME is the
     * whole message, and it is never waited on. XP has CreateEventA. */
    notes_seen = CreateEventA(NULL, TRUE, FALSE, name);
    notes_seen_hash = notes_seen ? hash : 0;
}

/* The running launcher was told the note was seen: take a share of the mark
 * so it outlives the process that clicked. Called from kitty_launcher.c.
 * `hash` names the note that was actually on screen; 0 falls back to the note
 * the store holds now. */
void kitty_notes_seen_hold(unsigned int hash)
{
    if (!hash) {
        char text[NOTES_TEXT_MAX];
        if (!kitty_notes_get(text, sizeof(text)))
            return;
        hash = notes_hash(text);
    }
    notes_seen_take(hash);
}

static int notes_seen_already(unsigned int hash)
{
    char name[80];
    HANDLE h;
    notes_seen_name(hash, name, sizeof(name));
    h = OpenEventA(SYNCHRONIZE, FALSE, name);
    if (!h)
        return 0;
    CloseHandle(h);
    return 1;
}

/* The notice is finished with - clicked, or gone without being read. Either
 * way this process is no longer showing the note, so the desktop slot goes
 * back: holding it with nothing on screen would silence every other KiTTY++.
 * (A notice merely PARKED behind a later one does not come here at all -
 * see kitty_notice.c - so the slot stays held while the note is waiting.) */
static void notes_notice_closed(void *ctx, int clicked)
{
    (void)ctx;
    if (clicked && notes_shown_hash && notes_once_enabled()) {
        /* The note that was ON SCREEN, not whatever the store holds now: the
         * configuration box can have edited the text while its own notice was
         * still up, and what was read is what has been seen. */
        UINT msg = kitty_notes_seen_message();
        notes_seen_take(notes_shown_hash);
        /* And hand the mark to a running launcher, so that it survives this
         * process. The hash travels in wParam for the same reason. */
        if (msg)
            PostMessageA(HWND_BROADCAST, msg,
                         (WPARAM)notes_shown_hash, 0);
    }
    if (notes_mutex) {
        CloseHandle(notes_mutex);
        notes_mutex = NULL;
    }
}

void kitty_notes_show_pending(HWND owner)
{
    char text[NOTES_TEXT_MAX], name[64];
    unsigned int hash;
    HANDLE m;

    (void)owner;
    if (!notes_pending)
        return;
    notes_pending = 0;          /* first, so two windows cannot both show it */

    if (!kitty_notes_get(text, sizeof(text)))
        return;
    hash = notes_hash(text);

    /* Seen FIRST, and without taking the slot: somebody has already read this
     * note on this desktop and asked not to be shown it again. */
    if (notes_seen_already(hash))
        return;

    /* Local\ = this logon session only. */
    snprintf(name, sizeof(name), "Local\\KiTTYNotes.%08x", hash);
    m = CreateMutexA(NULL, FALSE, name);
    if (!m)
        return;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* Another KiTTY++ is showing this note right now. */
        CloseHandle(m);
        return;
    }
    notes_mutex = m;
    notes_shown_hash = hash;    /* what a click will count as having read */

    /* The same green the other informational notices use, and no click
     * action: the note is something to read, not something to act on. */
    kitty_notice_show_ex(KT_CAP_KITTYPP, text, RGB(0, 100, 0),
                         KITTY_NOTICE_STICKY, NULL, 0,
                         notes_notice_closed, NULL);
}
