/*
 * kitty_config_int.h - PRIVATE to the kitty_config_*.c files: the statics
 * that cross between the sequencer, the Session tab, the Application tab and
 * the shared engine, and the macros and types they share. Nothing outside
 * kitty/kitty_config*.c includes this; kitty_config.h is the public face.
 */

#ifndef KITTY_CONFIG_INT_H
#define KITTY_CONFIG_INT_H

#ifndef INIT_SECTION
#define INIT_SECTION "KiTTY"
#endif

/* The panel paths more than one of these files names. Everything under
 * "Application/" is the second tab; KSET_PATH names a KiTTY++ Settings leaf. */
#define KCFG_PATH_HOSTKEYS      "Application/Security/Host keys"
#define KCFG_PATH_APPLICATIONS  "Application/Security/Applications"
#define KCFG_PATH_INIVIEW       "Application/KiTTY++ Settings/Storage & Backup/KiTTY.ini"
#define KCFG_PATH_OLD_FOLDERS   "Application/Migration/old KiTTY Folders"
#define KCFG_PATH_AUTOTEXT      "Application/KiTTY++ Settings/Keys & Mouse/Shortcuts/AutoText"
#define KCFG_PATH_SHORTCUTS     "Application/KiTTY++ Settings/Keys & Mouse/Shortcuts"
#define KCFG_PATH_WORKPLACE     "Application/Workplace Proxy"
#define KCFG_PATH_KSET          "Application/KiTTY++ Settings/"
#define KSET_PATH(leaf)         KCFG_PATH_KSET leaf
extern struct wpmode_data *kitty_wpmode_active;   /* kitty_config.c */
/*
 * KiTTY: Storage & Backup > KiTTY.ini - a read-only view of the configuration
 * file that follows the file on disk, with the shipped example beside it for
 * copying. One instance per configuration
 * box; the pointer is what the once-a-second poll and the fill hook use.
 */
struct iniview_data {
    dlgcontrol *show;                  /* "Show:" droplist */
    dlgcontrol *view;                  /* the read-only box - the panel's FILL control */
    dlgcontrol *editbtn;
    char ini_path[MAX_PATH * 2];       /* "" when KiTTY runs without a file */
    char example_path[MAX_PATH * 2];   /* "" when not installed beside the exe */
    int showing;                       /* 0 = kitty.ini, 1 = the example */
    FILETIME shown_write;              /* last-write time of the text on screen */
    bool shown_valid;
};
extern struct iniview_data *kitty_iniview_active;   /* kitty_config.c */
/*
 * KiTTY: Migration > old KiTTY Folders - sessions in FILES, imported from a
 * folder tree. The engine is
 * kitty_migrate.c; this is the panel's state for one configuration box.
 */
struct migf_data {
    dlgcontrol *folderbox;             /* the folder to scan */
    dlgcontrol *target;                /* editable combo: the folder imports go to */
    dlgcontrol *listbox;               /* Session | Path | Target | State - the FILL control */
    dlgcontrol *banner;                /* the result line */
    struct kitty_folder_scan *found;   /* the last scan, or NULL */
    char root[MAX_PATH * 2];
    char folder[256];                  /* the target folder as typed/chosen */
};
extern struct migf_data *kitty_migf_active;   /* kitty_config.c */
/* What Verify found out about one stored key, kept beside the store's own
 * list (which is re-enumerated on every refresh): matched by host, port and
 * type. `status` is a column word: OK, MISMATCH, not offered, unreachable,
 * no klink, klink failed, not stored. */
struct hk_verdict {
    char *host; int port; char *keytype;
    char *status, *sha256, *md5, *error, *when;
};
struct hk_data {
    dlgcontrol *listbox;
    dlgcontrol *banner;
    dlgcontrol *detail;                 /* the selected key, in full */
    dlgcontrol *verify;
    struct kitty_hostkey_list *keys;
    int sort_col;                       /* header column the list is sorted by */
    bool sort_desc;
    /* Verify: the run in the background and what it found */
    struct kitty_hkv_run *run;
    dlgparam *dlg;
    UINT_PTR timer;
    LONG *shown_state;                  /* per job: the state the list shows */
    struct hk_verdict *verdicts;
    int nverdicts;
    size_t verdicts_alloc;
};
extern struct hk_data *kitty_hk_active;   /* kitty_config.c */
extern int kitty_cfgbox_loaded_deliberate;   /* kitty_config.c */
#define HOST_BOX_TITLE "Host Name (or IP address)"
#define PORT_BOX_TITLE "Port"
void conf_radiobutton_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
#define CHECKBOX_INVERT (1<<30)
void conf_checkbox_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void conf_editbox_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void conf_filesel_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void conf_fontsel_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void config_host_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void config_port_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
struct hostport {
    dlgcontrol *host, *port, *protradio, *protlist;
    bool mid_refresh;
};
void config_protocols_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void loggingbuttons_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void numeric_keypad_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void cipherlist_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void gsslist_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void kexlist_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void hklist_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void printerbox_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void kitty_printclip_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void codepage_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void sshbug_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void sshbug_handler_manual_only(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
struct charclass_data {
    dlgcontrol *listbox, *editbox, *button;
};
void charclass_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
struct colour_data {
    dlgcontrol *listbox, *redit, *gedit, *bedit, *button;
};
void colour_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
struct ttymodes_data {
    dlgcontrol *valradio, *valbox, *setbutton, *listbox;
};
void ttymodes_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
struct environ_data {
    dlgcontrol *varbox, *valbox, *addbutton, *rembutton, *listbox;
};
void environ_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
struct portfwd_data {
    dlgcontrol *addbutton, *rembutton, *listbox;
    dlgcontrol *sourcebox, *destbox, *direction;
#ifndef NO_IPV6
    dlgcontrol *addressfamily;
#endif
};
void portfwd_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
struct manual_hostkey_data {
    dlgcontrol *addbutton, *rembutton, *listbox, *keybox;
};
void manual_hostkey_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void clipboard_control(struct controlset *s, const char *label, char shortcut, int percentage, HelpCtx helpctx, int setting, int strsetting);   /* kitty_config_upstream.c */
void serial_parity_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void serial_flow_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void proxy_type_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_upstream.c */
void host_ca_button_handler(dlgcontrol *ctrl, dlgparam *dp, void *data, int event);   /* kitty_config_upstream.c */
void kitty_notify_launcher_sessions_changed(void);   /* kitty_config_shared.c */
void kitty_checkbox_int_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_shared.c */
#define KITTY_PROXY_LABEL_ACTIVE "PROXY OVERRIDE ACTIVE:"
#define KITTY_LINESPC_LABEL "Line spacing (100-300 %)"
extern bool g_linespc_out_of_range;   /* kitty_config_shared.c */
#define INIT_SECTION "KiTTY"
struct wpmode_data {
    char *name; unsigned int minutes;
    /* Whether `name` is a choice the USER made in this box, as opposed to
     * whatever happened to be preselected. Only a real choice may outrank the
     * stored selection when the list is rebuilt - without this distinction the
     * box latched onto its own preselected row and then ignored the launcher
     * changing the selection, showing one proxy while the store held another
     *. */
    bool picked;
    dlgcontrol *list; dlgcontrol *hours; dlgcontrol *button; dlgcontrol *state;
    /* One line under the state: blank while proxies exist, otherwise it says
     * named proxies have to be configured first (the controls grey with it). */
    dlgcontrol *noproxy;
};
#define KITTY_WORKPLACE_STATE_OFF "Workplace proxy mode is off."
void kitty_dlg_droplist_fit(dlgcontrol *ctrl, dlgparam *dlg);   /* kitty_config_shared.c */
void kitty_dlg_enable_button(dlgcontrol *ctrl, dlgparam *dlg, bool enabled);   /* kitty_config_shared.c */
extern int cfgwin_refreshing;   /* kitty_config_shared.c */
int kitty_kset_backupcount(const char *key);   /* kitty_config_shared.c */
void kitty_kset_backupcount_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_shared.c */
extern dlgcontrol *kset_sshver_preview;   /* kitty_config_shared.c */
const struct kset_key *kset_find(const char *key);   /* kitty_config_shared.c */
void kset_write(const struct kset_key *k, const char *text);   /* kitty_config_shared.c */
void kset_defer_write(const struct kset_key *k, const char *text);   /* kitty_config_shared.c */
int kset_get_int(const struct kset_key *k);   /* kitty_config_shared.c */
void kitty_kset_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_shared.c */
const char *kitty_pxload_name_at(int row);   /* kitty_config_session.c */
struct sessionsaver_data {
    dlgcontrol *editbox, *listbox, *loadbutton, *savebutton, *delbutton;
    dlgcontrol *okbutton, *cancelbutton;
    dlgcontrol *startbutton;     /* KiTTY: open session without closing config box */
    dlgcontrol *folderlist;      /* KiTTY: editable session-folder combo */
    dlgcontrol *createbutton, *delfolderbutton; /* KiTTY folder mgmt */
    dlgcontrol *commentbox;      /* KiTTY: read-only comment of selected session */
    struct sesslist sesslist;
    bool midsession;
    int midsession_level_set;    /* the mid-session list has been pointed at the
                                  * running session's own folder (once only) */
    char *savedsession;     /* the current contents of ssd->editbox */
    char *newfolder;        /* typed folder name in ssd->folderlist combo */
    char *searchfilter;     /* live type-to-search filter from saved-session edit */
    int suppress_edit_valchange; /* set while programmatically updating editbox */
    int suppress_list_selchange; /* set while programmatically selecting list rows */
    /* The folder named by the currently selected FOLDER ROW, or NULL when the
     * selection is a session, "..", or nothing. Not a mode the user turns on:
     * it is set by the selection event and cleared when the list is rebuilt, so
     * it cannot be left armed by a path nobody thought of. It is what makes the
     * Save button read "Rename". */
    char *selected_folder;
    const char *save_button_label;   /* last caption set on the Save button */
    /* The folder name WE put in the name box when a folder row was selected,
     * kept so it can be taken back out again if the user never touched it. A
     * folder name is not a save target: left behind after the rename ends, one
     * click on Save would write the running session into a NEW session named
     * after the folder. Cleared the moment the text differs, which is what
     * stops this from eating a search someone is typing. */
    char *folder_text_in_box;
    int suppress_folder_valchange; /* set while programmatically updating folderlist */
    int folder_new_selected;     /* the synthetic "<new folder...>" row is picked */
    char *folder_at_load;        /* folder the loaded session came from, or NULL:
                                  * Save only re-files when this changed */
    int folder_action;           /* what ssd->createbutton currently does */
    /* KiTTY folder navigation: Ctrl+G asked for a search that SPANS folders.
     * Ordinary typing narrows the current level only, so without this the root
     * level - which holds just the unfiled sessions in this mode - would be the
     * whole of what Ctrl+G could find, and "search everywhere" would be the one
     * thing it could not do. Armed by Ctrl+G; disarmed when the level changes,
     * or when focus comes BACK to the search box after having left it for
     * anything other than the session list (see search_left below). Emptying
     * the box while still in it does NOT disarm: deleting a mistyped search to
     * correct it is part of the same search. */
    int search_all;
    /* Focus went from the search box to some OTHER control (not the session
     * list) while search_all was armed. Deliberately NOT an immediate disarm:
     * the list still shows cross-folder rows, and every row-to-session mapping
     * must keep using the mode the list was BUILT with, or a click on Load
     * after picking a result would act on the wrong row. The disarm happens
     * when focus returns to the search box - the old search is over, the list
     * is rebuilt for the current level, and the two can never disagree. */
    int search_left;
    const char *folder_button_label; /* its current label, to avoid redundant sets */
    /* KiTTY folder navigation: the folder names currently drawn as rows, in row
     * order. A folder row's id indexes THIS, not FolderList, because the two are
     * not the same set - see kitty_rebuild_folder_rows(). Rebuilt on every list
     * refresh and freed with the dialog. */
    char **folderrows;
    int nfolderrows;
    int initial_focus_set;
    /* KiTTY: the session whose settings are actually in the box, as opposed to
     * the name sitting in the edit box. They part company the moment you click a
     * name in the list, because that only copies the NAME across - which is the
     * whole reason the Save guard exists. NULL = nothing loaded in this box. */
    char *loaded_from;
};
extern struct sessionsaver_data *kitty_session_ssd;   /* kitty_config_session.c */
int kitty_config_session_rows(void);   /* kitty_config_session.c */
extern struct sessionsaver_data *session_filter_ssd;   /* kitty_config_session.c */
void scb_panel_session(struct controlbox *b, bool midsession);   /* kitty_config_session.c */
void scb_panel_logging(struct controlbox *b, bool midsession, int protocol);   /* kitty_config_session.c */
void scb_panel_scripting(struct controlbox *b, bool midsession);   /* kitty_config_session.c */
void scb_panel_terminal(struct controlbox *b);   /* kitty_config_session.c */
void scb_panel_window(struct controlbox *b, bool midsession, int protocol);   /* kitty_config_session.c */
void scb_panel_selection(struct controlbox *b);   /* kitty_config_session.c */
void scb_panel_connection(struct controlbox *b, bool midsession, int protocol);   /* kitty_config_session.c */
void scb_panel_proxy(struct controlbox *b, bool midsession);   /* kitty_config_session.c */
void scb_panel_ssh(struct controlbox *b, bool midsession, int protocol, int protcfginfo);   /* kitty_config_session.c */
void scb_panel_serial(struct controlbox *b, bool midsession, int protocol);   /* kitty_config_session.c */
void scb_panel_other_protocols(struct controlbox *b, bool midsession, int protocol);   /* kitty_config_session.c */
void scb_panel_transfers(struct controlbox *b);   /* kitty_config_session.c */
void scb_panel_zmodem(struct controlbox *b);   /* kitty_config_session.c */
void scb_panel_comment(struct controlbox *b);   /* kitty_config_session.c */
void kitty_wpmode_state_label(struct wpmode_data *wd, dlgparam *dlg);   /* kitty_config_app.c */
void kitty_wpmode_grey(struct wpmode_data *wd, dlgparam *dlg);   /* kitty_config_app.c */
void kitty_wpmode_button_label(dlgcontrol *ctrl, dlgparam *dlg);   /* kitty_config_app.c */
void kitty_wpmode_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_app.c */
void kitty_proxyedit_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_app.c */
void kitty_iniview_place(void);   /* kitty_config_app.c */
extern HWND hk_splitter;   /* kitty_config_app.c */
void hk_place_splitter(struct hk_data *hk);   /* kitty_config_app.c */
void kitty_foreignnotice_handler(dlgcontrol *ctrl, dlgparam *dlg, void *data, int event);   /* kitty_config_app.c */
const char *scb_title_appname(void);   /* kitty_config_app.c */
void kitty_iniview_poll(dlgparam *dlg);   /* kitty_config_app.c */
dlgcontrol *kitty_sc_fill_ctrl(bool autotext);   /* kitty_config_app.c */
void scb_panel_shortcut_editor(struct controlbox *b, const char *path);   /* kitty_config_app.c */
void scb_app_footer(struct controlbox *b, const char *path);   /* kitty_config_app.c */
void scb_panel_application(struct controlbox *b, bool midsession);   /* kitty_config_app.c */
void scb_panel_applications(struct controlbox *b);   /* kitty_config_apps.c: Security > Applications */
dlgcontrol *kitty_apps_fill_ctrl(void);   /* kitty_config_apps.c: its list, for the fill hook */

#endif /* KITTY_CONFIG_INT_H */
