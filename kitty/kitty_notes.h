#ifndef KITTY_NOTES_H
#define KITTY_NOTES_H

#include <windows.h>
#include <stddef.h>

/*
 * The application notification: one note the administrator (or the user)
 * leaves for every KiTTY++ process to show once, in the notice window near
 * the clock. Stored as [KiTTY] notes - ONE line, with escapes for the line
 * breaks. See kitty_notes.c for the escape table and the registry twin.
 */

/* The shared escape pair. Both write at most `outsize` bytes including the
 * terminating NUL; a value that does not fit is truncated. */
void kitty_notes_encode(const char *text, char *out, size_t outsize);
void kitty_notes_decode(const char *stored, char *out, size_t outsize);

/* The note as text, line breaks as CRLF (what a Win32 edit box and DrawText
 * both want). Returns 1 when there is something to show. */
int kitty_notes_get(char *buf, size_t size);

/* What the settings field has just been edited to, so this process agrees
 * with what it wrote to the store. Pass NULL to drop the running copy and
 * read the store again. */
void kitty_notes_set_running(const char *text);

/* Startup owes a notice; the first window of this process shows it. */
void kitty_notes_mark_pending(void);

/* Show it, if it is owed, the desktop is not already showing this note, and
 * nobody has marked it seen. `owner` is only used to place nothing - the
 * notice is ownerless - and may be NULL; it is there so a caller can pass
 * its window without thinking. */
void kitty_notes_show_pending(HWND owner);

/* [KiTTY] notesonce: a click marks the note seen for this desktop, and the
 * mark is handed to the running launcher so that it outlives the process
 * that clicked. The launcher answers this registered window message (a
 * broadcast, keyed to the install) by taking a share of the mark itself; the
 * message's wParam is the hash of the note that was ON SCREEN, so a note
 * edited since the notice went up cannot be marked read by mistake. Pass 0
 * for "whatever the store holds now". */
unsigned int kitty_notes_seen_message(void);
void kitty_notes_seen_hold(unsigned int hash);

#endif
