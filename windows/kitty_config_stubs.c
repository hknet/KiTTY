/*
 * Link-time stubs for the stock PuTTY GUI variants (putty / puttytel / pterm).
 *
 * Those targets compile KiTTY's shared, modified windows/dialog.c (via the
 * guiterminal library) but do NOT link the KiTTY config/proxy sources
 * (kitty/kitty.c, kitty/kitty_proxy.c) that define these accessors. Without
 * these stubs they fail to link. The values chosen make the KiTTY config-box
 * height override and the named-proxy Proxy-choice row inert, so those targets
 * render the stock config box.
 *
 * The real implementations, driving the actual KiTTY behaviour, live in
 * kitty/kitty.c (GetConfigBoxHeight / GetConfigBoxWindowHeight) and
 * kitty/kitty_proxy.c (kitty_proxy_choice_shown) and are linked into the
 * kitty / kitty_portable targets instead of this file.
 */

#include <stdbool.h>   /* kitty_config_select_root_folder returns bool */
#include <windows.h>   /* the themed-box stubs fall back to MessageBoxA */

int GetConfigBoxHeight(void)       { return 16; } /* == stock-fit rows -> extra_rows 0 */
int GetConfigBoxWindowHeight(void) { return 0; }  /* no explicit window-height override */
int GetConfigBoxWindowWidth(void)  { return 0; }  /* nor an explicit width */

/* The stock box is not resizable, so it is never dragged to a new size and
 * there is nothing to remember. */
void kitty_cfgbox_store_size(int w, int h) { (void)w; (void)h; }

/* No panel in the stock box places any of its own controls: the saved-session
 * button column is a KiTTY arrangement. */
void kitty_config_panel_placed(const char *path) { (void)path; }
struct dlgcontrol *kitty_config_panel_fill_ctrl(const char *path) { (void)path; return 0; }

/* No named-proxy panel here, so nothing on it can be unsaved. */
bool kitty_proxy_panel_dirty(void) { return false; }
int kitty_proxy_choice_shown(void) { return 0; }  /* no Proxy-choice droplist row */

/* Inline-first security prompts: 1 = keep the confirmation MODAL, i.e. the
 * stock PuTTY message box. The real flags (kitty/kitty_commun.c) are driven by
 * the [KiTTY] modal*confirmation settings, which these targets do not read. */
int GetModalNewHostKeyConfirmationFlag(void)     { return 1; }
int GetModalChangedHostKeyConfirmationFlag(void) { return 1; }
int GetModalWeakKeyConfirmationFlag(void)        { return 1; }

struct dlgcontrol;
struct dlgcontrol *kitty_config_session_filter_ctrl(void) { return 0; } /* no Ctrl+F jump */

/* dialog.c asks this for every static in the config box, so that KiTTY can draw
 * the proxy-override caption bold while an override is armed. The stock variants
 * have no proxy override and no such caption, so nothing is ever bold. */
bool kitty_red_caption(const char *text) { return false; }

/* Likewise for the bold-but-uncoloured captions: the stock variants have no
 * workplace proxy mode, so no caption of theirs is ever bold. */
bool kitty_bold_caption(const char *text) { return false; }

/* The config box polls once a second so that switching workplace proxy mode
 * from the tray reaches an open box. No such mode here, so nothing to poll. */
struct dlgparam;
void kitty_cfgbox_workplace_poll(struct dlgparam *dp) { }

/* And nothing ever asks the stock variants to open on a particular panel. */
const char *kitty_cfgbox_wanted_panel(void) { return 0; }

/* Ctrl+G resets the session-folder filter to the root list; the stock variants
 * have no folders, so there is nothing to reset. dialog.c only reaches this
 * after the accessor above returned non-NULL, which the stub never does — it
 * exists purely to satisfy the link. */
struct dlgparam;
bool kitty_config_select_root_folder(struct dlgparam *dp) { (void)dp; return false; }
/* Likewise: no folder rows in the stock variants, so no rename to end. */
void kitty_config_end_folder_rename(struct dlgparam *dp) { (void)dp; }

/* The stock variants remember no Category-tree folds: nothing is ever
 * recorded, no override ever answers, and saving writes nowhere. */
void kitty_cfgtree_set_fold(const char *path, int expanded, int default_expanded)
{ (void)path; (void)expanded; (void)default_expanded; }
int kitty_cfgtree_get_fold(const char *path) { (void)path; return -1; }
void kitty_cfgtree_folds_save(void) { }

/* The stock variants have no named-proxy pre-set loader to pin. */
void kitty_config_proxy_pin_presets(void) { }

/* ...and nothing pinned to the panel bottom at all. */
void kitty_config_pin_bottoms(void) { }

/* The stock variants carry no KiTTY theme engine and no IDD_CONFIRMBOX
 * template, so the themed boxes window.c now calls degrade to the classic
 * MessageBox with the same words and the same defaults. */
void kitty_info_box(HWND owner, const char *caption, const char *text,
                    const char *warn_red)
{
    (void)warn_red;
    MessageBoxA(owner, text, caption, MB_OK | MB_ICONERROR);
}
int kitty_confirm_box(HWND owner, const char *caption, const char *text,
                      const char *warn_red)
{
    (void)warn_red;
    return MessageBoxA(owner, text, caption,
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}
int kitty_confirm_box_yes(HWND owner, const char *caption, const char *text,
                          const char *warn_red)
{
    (void)warn_red;
    return MessageBoxA(owner, text, caption,
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1) == IDYES;
}

/* The stock variants have no themed boxes at all, so the routed MessageBox
 * calls in shared files (windows/controls.c) stay real MessageBoxes here. */
int kitty_message_box(HWND owner, const char *text, const char *caption,
                      unsigned type)
{
    return MessageBoxA(owner, text, caption, type);
}

/* The template-review gallery (-demo-templates) shows KiTTY's dialog dress;
 * the stock variants have none to review. */
void kitty_demo_templates(void) { }
