/*
 * kitty_iconpick.c - the icon picker of Window > Title & Icon (hknet/KiTTY#62):
 * a modal box with a grid of every icon the session can choose by number,
 * each labelled with that number, the current one selected. OK or a double
 * click hands the number back, Cancel or Esc -1.
 *
 * Two sources:
 *  - the built-in set (lib NULL): the icon library KiTTY++ loaded at its
 *    start (kitty++.dll, else kitty.dll, else the exe), resource ids 1-999 -
 *    number n shows resource n, as SetNewIcon does;
 *  - a library in the session's IconeFile (.dll, .exe, .icl): its icons in
 *    the order Windows lists them, number n = icon n - 1
 *    (kitty_icon_from_library).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"

#include <windows.h>
#include <commctrl.h>

#include "kitty_iconpick.h"
#include "kitty_sessicon.h"
#include "kitty_text.h"
#include "kitty_theme.h"
#include "kitty_rc_additions.h"   /* IDD_ICONPICK, IDC_IP_* */

#include "kitty_int.h"            /* hInstIcons, kitty_icon_library_path */

struct ip_ctx {
    const char *lib;              /* NULL = the built-in set */
    int cur;                      /* the number now set (0 = 1) */
    int *nums;                    /* item -> number */
    int n;
    HIMAGELIST il;
    int result;
};

/* The icons as the list's images, at the large-icon size of this monitor. */
static void ip_fill(HWND dlg, struct ip_ctx *c)
{
    HWND lv = GetDlgItem(dlg, IDC_IP_LIST);
    int px = GetSystemMetrics(SM_CXICON), i, sel = -1;
    HINSTANCE lib = hInstIcons ? hInstIcons : GetModuleHandle(NULL);
    int want = c->cur > 0 ? c->cur : 1;
    /* the built-in set is ids 1-50 (its dark forms, 1001 and up, are no
     * choice of their own) */
    int cap = c->lib ? kitty_icon_library_count(c->lib) : 50;
    char *src;
    c->il = ImageList_Create(px, px, ILC_COLOR32 | ILC_MASK, 16, 16);
    ListView_SetImageList(lv, c->il, LVSIL_NORMAL);
    c->nums = snewn(cap > 0 ? cap : 1, int);
    c->n = 0;
    for (i = 0; i < cap; i++) {
        HICON h;
        int num;
        LVITEMA it;
        char label[16];
        if (c->lib) {
            h = kitty_icon_from_library(c->lib, i, px);
            num = i + 1;
        } else {
            h = (HICON)LoadImageA(lib, MAKEINTRESOURCE(i + 1), IMAGE_ICON, px, px, 0);
            num = i + 1;
        }
        if (!h)
            continue;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        it.iItem = c->n;
        it.iImage = ImageList_AddIcon(c->il, h);
        DestroyIcon(h);
        sprintf(label, "%d", num);
        it.pszText = label;
        it.lParam = num;
        if (SendMessageA(lv, LVM_INSERTITEMA, 0, (LPARAM)&it) < 0)
            continue;
        if (num == want)
            sel = c->n;
        c->nums[c->n++] = num;
    }
    if (c->n == 0) {
        src = dupstr(KT_ICONPICK_NONE);
    } else if (c->lib) {
        src = dupprintf(KT_ICONPICK_FROM_FMT, c->lib);
    } else {
        const char *p = kitty_icon_library_path();
        src = dupprintf(KT_ICONPICK_FROM_FMT, p ? p : KT_ICONPICK_THIS_PROGRAM);
    }
    SetDlgItemTextA(dlg, IDC_IP_SOURCE, src);
    sfree(src);
    if (sel < 0 && c->n)
        sel = 0;
    if (sel >= 0) {
        ListView_SetItemState(lv, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(lv, sel, FALSE);
    }
    EnableWindow(GetDlgItem(dlg, IDOK), c->n > 0);
}

static int ip_selected(HWND dlg)
{
    HWND lv = GetDlgItem(dlg, IDC_IP_LIST);
    int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    LVITEMA it;
    if (i < 0)
        return -1;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = i;
    if (!SendMessageA(lv, LVM_GETITEMA, 0, (LPARAM)&it))
        return -1;
    return (int)it.lParam;
}

static INT_PTR CALLBACK ip_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    struct ip_ctx *c = (struct ip_ctx *)GetWindowLongPtr(dlg, GWLP_USERDATA);
    switch (msg) {
      case WM_INITDIALOG:
        c = (struct ip_ctx *)lp;
        SetWindowLongPtr(dlg, GWLP_USERDATA, lp);
        SetWindowTextA(dlg, KT_ICONPICK_TITLE);
        SetDlgItemTextA(dlg, IDOK, KT_ICONPICK_OK);
        SetDlgItemTextA(dlg, IDCANCEL, KT_ICONPICK_CANCEL);
        ListView_SetExtendedListViewStyle(GetDlgItem(dlg, IDC_IP_LIST),
                                          LVS_EX_DOUBLEBUFFER);
        ip_fill(dlg, c);
        SetFocus(GetDlgItem(dlg, IDC_IP_LIST));
        return FALSE;
      case WM_NOTIFY:
        if (((LPNMHDR)lp)->idFrom == IDC_IP_LIST &&
            ((LPNMHDR)lp)->code == NM_DBLCLK && ip_selected(dlg) >= 0) {
            c->result = ip_selected(dlg);
            EndDialog(dlg, 1);
            return TRUE;
        }
        break;
      case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            c->result = ip_selected(dlg);
            EndDialog(dlg, c->result >= 0);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            c->result = -1;
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
      case WM_DESTROY:
        if (c && c->il) {
            ListView_SetImageList(GetDlgItem(dlg, IDC_IP_LIST), NULL, LVSIL_NORMAL);
            ImageList_Destroy(c->il);
            c->il = NULL;
        }
        break;
    }
    return FALSE;
}

int kitty_icon_pick(HWND owner, const char *lib, int cur)
{
    struct ip_ctx c;
    memset(&c, 0, sizeof(c));
    c.lib = (lib && *lib) ? lib : NULL;
    c.cur = cur;
    c.result = -1;
    DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDD_ICONPICK),
                    owner, ip_proc, (LPARAM)&c);
    sfree(c.nums);
    return c.result;
}
