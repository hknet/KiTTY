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

int GetConfigBoxHeight(void)       { return 16; } /* == stock-fit rows -> extra_rows 0 */
int GetConfigBoxWindowHeight(void) { return 0; }  /* no explicit window-height override */
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
