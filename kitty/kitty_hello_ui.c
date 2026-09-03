/*
 * kitty_hello_ui.c - the shared Hello-keys UI (see the header). The
 * dialog template is built in memory (DialogBoxIndirectParam): three
 * apps carrying three copies of the same .rc dialog was how the wording
 * drifted apart in the first place.
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "kitty_hello_ui.h"
#include "kitty_text.h"     /* the printout window's wordings */

#define HUI_ID_NOTE 100
#define HUI_ID_TEXT 101
#define HUI_ID_COPY 102

struct hui_show {
    const char *appname;
    const char *text;
    int sidebound;
};

/*
 * In-memory DLGTEMPLATE plumbing. Everything is WORD-aligned; the
 * template speaks UTF-16 for every string.
 */
struct hui_tpl {
    unsigned char buf[2048];
    size_t len;
};

static void hui_put(struct hui_tpl *t, const void *data, size_t len)
{
    if (t->len + len <= sizeof(t->buf)) {
        memcpy(t->buf + t->len, data, len);
        t->len += len;
    }
}

static void hui_put_w(struct hui_tpl *t, WORD w)
{
    hui_put(t, &w, sizeof(w));
}

static void hui_put_dw(struct hui_tpl *t, DWORD dw)
{
    hui_put(t, &dw, sizeof(dw));
}

static void hui_put_wstr(struct hui_tpl *t, const WCHAR *s)
{
    hui_put(t, s, (wcslen(s) + 1) * sizeof(WCHAR));
}

static void hui_align(struct hui_tpl *t)
{
    while (t->len & 3)
        t->buf[t->len++] = 0;
}

static void hui_item(struct hui_tpl *t, DWORD style, short x, short y,
                     short cx, short cy, WORD id, WORD cls,
                     const WCHAR *text)
{
    hui_align(t);
    hui_put_dw(t, style | WS_CHILD | WS_VISIBLE);
    hui_put_dw(t, 0);                  /* exstyle */
    hui_put_w(t, (WORD)x);
    hui_put_w(t, (WORD)y);
    hui_put_w(t, (WORD)cx);
    hui_put_w(t, (WORD)cy);
    hui_put_w(t, id);
    hui_put_w(t, 0xFFFF);              /* system class follows */
    hui_put_w(t, cls);                 /* 0x80 button, 0x81 edit, 0x82 static */
    hui_put_wstr(t, text);
    hui_put_w(t, 0);                   /* no creation data */
}

static const DLGTEMPLATE *hui_build_printout(struct hui_tpl *t)
{
    memset(t, 0, sizeof(*t));
    hui_put_dw(t, DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION |
                  WS_SYSMENU);
    hui_put_dw(t, 0);                  /* exstyle */
    hui_put_w(t, 4);                   /* item count */
    hui_put_w(t, 0);                   /* x */
    hui_put_w(t, 0);                   /* y */
    hui_put_w(t, 360);                 /* cx (DLU) */
    hui_put_w(t, 130);                 /* cy */
    hui_put_w(t, 0);                   /* no menu */
    hui_put_w(t, 0);                   /* default window class */
    hui_put_wstr(t, L"");              /* title set at WM_INITDIALOG */
    hui_put_w(t, 9);                   /* font size */
    hui_put_wstr(t, L"Segoe UI");      /* like every converted RC template */

    hui_item(t, SS_LEFT, 10, 10, 340, 56, HUI_ID_NOTE, 0x82, L"");
    hui_item(t, ES_READONLY | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
             10, 70, 340, 14, HUI_ID_TEXT, 0x81, L"");
    hui_item(t, BS_PUSHBUTTON | WS_TABSTOP, 10, 108, 90, 14,
             HUI_ID_COPY, 0x80, L"&Copy to clipboard");
    hui_item(t, BS_DEFPUSHBUTTON | WS_TABSTOP, 260, 108, 90, 14,
             (WORD)IDOK, 0x80, L"I have &stored it");
    return (const DLGTEMPLATE *)t->buf;
}

static INT_PTR CALLBACK hui_printout_proc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    struct hui_show *s = (struct hui_show *)
        GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_INITDIALOG: {
        char *note, *title;
        s = (struct hui_show *)lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)s);
        title = dupprintf(KT_HELLO_UI_TITLE_FMT, s->appname,
                          s->sidebound ? KT_HELLO_UI_RECOVERY_CODE : KT_HELLO_UI_PRINTED_SECRET);
        SetWindowTextA(hwnd, title);
        sfree(title);
        note = dupprintf(s->sidebound ?
            KT_HELLO_UI_NOTE_CODE_FMT :
            KT_HELLO_UI_NOTE_SECRET_FMT, s->appname);
        SetDlgItemTextA(hwnd, HUI_ID_NOTE, note);
        sfree(note);
        SetDlgItemTextA(hwnd, HUI_ID_TEXT, s->text);
        return 1;
      }
      case WM_COMMAND:
        switch (LOWORD(wParam)) {
          case HUI_ID_COPY: {
            char *tx = GetDlgItemText_alloc(hwnd, HUI_ID_TEXT);
            if (tx && OpenClipboard(hwnd)) {
                size_t len = strlen(tx) + 1;
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
                EmptyClipboard();
                if (h) {
                    void *m = GlobalLock(h);
                    if (m) {
                        memcpy(m, tx, len);
                        GlobalUnlock(h);
                        SetClipboardData(CF_TEXT, h);
                    }
                }
                CloseClipboard();
            }
            burnstr(tx);
            return 0;
          }
          case IDOK:
            SetDlgItemTextA(hwnd, HUI_ID_TEXT, "");
            EndDialog(hwnd, 1);
            return 0;
          case IDCANCEL: {
            /* X and Esc are NOT a quiet "stored it". */
            char *q = dupprintf(KT_HELLO_UI_NOT_STORED_Q);
            char *cap = dupprintf(KT_HELLO_UI_NOT_STORED_CAP_FMT, s->appname);
            int r = MessageBoxA(hwnd, q, cap,
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            sfree(q);
            sfree(cap);
            if (r == IDYES) {
                SetDlgItemTextA(hwnd, HUI_ID_TEXT, "");
                EndDialog(hwnd, 1);
            }
            return 0;
          }
        }
        return 0;
      case WM_CLOSE:
        SendMessage(hwnd, WM_COMMAND, IDCANCEL, 0);
        return 0;
    }
    return 0;
}

void kitty_hello_ui_show_printout(HWND owner, const char *appname,
                                  const char *text, int sidebound)
{
    struct hui_tpl tpl;
    struct hui_show s;
    s.appname = appname;
    s.text = text;
    s.sidebound = sidebound;
    DialogBoxIndirectParamA(GetModuleHandleA(NULL), hui_build_printout(&tpl),
                            owner, hui_printout_proc, (LPARAM)&s);
}
