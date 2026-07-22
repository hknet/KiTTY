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

int GetConfigBoxHeight(void)       { return 16; } /* == stock-fit rows -> extra_rows 0 */
int GetConfigBoxWindowHeight(void) { return 0; }  /* no explicit window-height override */
int kitty_proxy_choice_shown(void) { return 0; }  /* no Proxy-choice droplist row */

struct dlgcontrol;
struct dlgcontrol *kitty_config_session_filter_ctrl(void) { return 0; } /* no Ctrl+F jump */
