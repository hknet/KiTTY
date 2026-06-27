#include "putty.h"
#include "storage.h"

#ifdef MOD_PERSO
/* KiTTY helpers (putty.c does not include kitty.h) */
extern char *SetSessPath(const char *);
extern int  GetDirectoryBrowseFlag(void);
extern void load_open_settings_forced(char *filename, Conf *conf); /* kitty_settings_load.c */
extern char *kitty_cli_loginscript; /* kitty_bridge.c: -loginscript, consumed post-create */
/* do-and-exit / pre-window utility switches (kitty modules; putty.c lacks kitty.h) */
extern char KiTTYClassName[];                       /* kitty.c: window class name */
extern int  SendCommandAllWindows(HWND hwnd, char *cmd); /* kitty.c */
extern void RunPuttyEd(HWND hwnd, char *filename);  /* kitty_win.c: session-file editor */
extern void SaveDump(void);                         /* kitty_savedump.c (MOD_SAVEDUMP) */
extern int  SetTextToClipboard(const char *buf);    /* kitty_win.c */
extern void mungestr(const char *in, char *out);    /* kitty_commun.c */
extern int  existfile(const char *filename);        /* kitty_tools.c */
extern void CreateFileAssoc(void);                  /* kitty_registry.c: .ktx file association */
extern void CreateSSHHandler(void);                 /* kitty_registry.c: telnet/ssh/putty URL handlers */
extern int  kitty_get_last_session(char *buf, int buflen); /* storage.c: remember-last-session */
#endif

extern bool sesslist_demo_mode;
extern Filename *dialog_box_demo_screenshot_filename;
static strbuf *demo_terminal_data = NULL;
static Filename *terminal_demo_screenshot_filename;

const unsigned cmdline_tooltype =
    TOOLTYPE_HOST_ARG |
    TOOLTYPE_PORT_ARG |
    TOOLTYPE_NO_VERBOSE_OPTION;

#ifdef MOD_NETDEBUG
extern void kitty_netdbg_ts(const char *msg);   /* kitty.c: startup checkpoint logger */
#define NETDBG_TS(m) kitty_netdbg_ts(m)
#else
#define NETDBG_TS(m) ((void)0)
#endif

void gui_term_process_cmdline(Conf *conf, char *cmdline)
{
    char *p;
    bool special_launchable_argument = false;
    bool demo_config_box = false;

    NETDBG_TS("cmdline: enter");
    settings_set_default_protocol(be_default_protocol);
    /* Find the appropriate default port. */
    {
        const struct BackendVtable *vt =
            backend_vt_from_proto(be_default_protocol);
        settings_set_default_port(0); /* illegal */
        if (vt)
            settings_set_default_port(vt->default_port);
    }
    conf_set_int(conf, CONF_logtype, LGTYP_NONE);

    do_defaults(NULL, conf);
    NETDBG_TS("cmdline: after do_defaults");

    p = handle_restrict_acl_cmdline_prefix(cmdline);

    if (handle_special_sessionname_cmdline(p, conf)) {
        if (!conf_launchable(conf) && !do_config(conf)) {
            cleanup_exit(0);
        }
        special_launchable_argument = true;
    } else if (handle_special_filemapping_cmdline(p, conf)) {
        special_launchable_argument = true;
    } else if (!*p) {
        /* Do-nothing case for an empty command line - or rather,
         * for a command line that's empty _after_ we strip off
         * the &R prefix. */
    } else {
        /*
         * Otherwise, break up the command line and deal with
         * it sensibly.
         */
        CmdlineArgList *arglist = cmdline_arg_list_from_GetCommandLineW();
        size_t arglistpos = 0;
        while (arglist->args[arglistpos]) {
            CmdlineArg *arg = arglist->args[arglistpos++];
            CmdlineArg *nextarg = arglist->args[arglistpos];
            const char *p = cmdline_arg_to_str(arg);
            int ret = cmdline_process_param(arg, nextarg, 1, conf);
            if (ret == -2) {
                cmdline_error("option \"%s\" requires an argument", p);
            } else if (ret == 2) {
                arglistpos++;          /* skip next argument */
            } else if (ret == 1) {
                continue;          /* nothing further needs doing */
#ifdef MOD_PERSO
            } else if (!strcmp(p, "-fullscreen")) {
                conf_set_int(conf, CONF_fullscreen, 1);
            } else if (!strcmp(p, "-xpos")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                int x = atoi(cmdline_arg_to_str(arglist->args[arglistpos++]));
                if (x >= 0) {
                    conf_set_int(conf, CONF_xpos, x);
                    if (conf_get_int(conf, CONF_ypos) < 0)
                        conf_set_int(conf, CONF_ypos, 0);
                    conf_set_bool(conf, CONF_save_windowpos, true);
                }
            } else if (!strcmp(p, "-ypos")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                int y = atoi(cmdline_arg_to_str(arglist->args[arglistpos++]));
                if (y >= 0) {
                    conf_set_int(conf, CONF_ypos, y);
                    if (conf_get_int(conf, CONF_xpos) < 0)
                        conf_set_int(conf, CONF_xpos, 0);
                    conf_set_bool(conf, CONF_save_windowpos, true);
                }
            } else if (!strcmp(p, "-hwndparent")) {
                /* KiTTY #554: embed the terminal as a child of the given host
                 * window (mRemoteNG / Remote4Support). The value is the parent
                 * window handle as a DECIMAL integer, matching the PuTTYNG /
                 * Remote4Support forks. The reparent itself happens in window.c
                 * right after the window is created. */
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                {
                    extern HWND kitty_hwnd_parent;
                    const char *hv = cmdline_arg_to_str(arglist->args[arglistpos++]);
                    kitty_hwnd_parent =
                        (HWND)(intptr_t)_strtoi64(hv, NULL, 10);
                }
            } else if (!strcmp(p, "-title")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                conf_set_str(conf, CONF_wintitle,
                             cmdline_arg_to_str(arglist->args[arglistpos++]));
            } else if (!strcmp(p, "-folder")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                const char *fld = cmdline_arg_to_str(arglist->args[arglistpos++]);
                conf_set_str(conf, CONF_folder, fld);
                if (GetDirectoryBrowseFlag()) SetSessPath(fld);
            } else if (!strcmp(p, "-cmd")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                conf_set_str(conf, CONF_autocommand,
                             cmdline_arg_to_str(arglist->args[arglistpos++]));
            } else if (!strcmp(p, "-codepage")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                conf_set_str(conf, CONF_line_codepage,
                             cmdline_arg_to_str(arglist->args[arglistpos++]));
            } else if (!strcmp(p, "-rcmd")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                /* CONF_remote_cmd is STR_AMBI; conf_set_str is safe (conf.c
                 * asserts STR||STR_AMBI, stores utf8=false). */
                conf_set_str(conf, CONF_remote_cmd,
                             cmdline_arg_to_str(arglist->args[arglistpos++]));
            } else if (!strcmp(p, "-log")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                Filename *fn = cmdline_arg_to_filename(arglist->args[arglistpos++]);
                conf_set_filename(conf, CONF_logfilename, fn);
                filename_free(fn);                    /* conf_set_filename copies */
                conf_set_int(conf, CONF_logtype, 1);  /* 0.76b literal; 1 == LGTYP_ASCII */
                conf_set_int(conf, CONF_logxfovr, 1); /* 1 == LGXF_OVR (overwrite) */
                conf_set_bool(conf, CONF_logflush, true);
            } else if (!strcmp(p, "-kload") || !strcmp(p, "-loadfile")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                /* Load a KiTTY .ktx session file into conf (read-side of the
                 * forced settings; load_open_settings_forced takes char*). */
                char *kf = dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
                if (strlen(kf) > 0) {
                    load_open_settings_forced(kf, conf);
                    special_launchable_argument = true;
                }
                sfree(kf);
            } else if (!strcmp(p, "-loginscript")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                /* Defer: ReadInitScript writes the GLOBAL conf, which is NULL
                 * until kitty_set_active_seat. Stash the path; window.c runs it
                 * from a post-window-create hook. */
                sfree(kitty_cli_loginscript);
                kitty_cli_loginscript =
                    dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
            } else if (!strcmp(p, "-classname")) {
                /* Set the window class name. Runs after InitWinMain (so it
                 * overrides the kitty.ini KiClassName default) but before the
                 * window is created, so it takes effect on the class. */
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                const char *cn = cmdline_arg_to_str(arglist->args[arglistpos++]);
                if (cn && *cn) {
                    strncpy(KiTTYClassName, cn, 127);
                    KiTTYClassName[127] = '\0';
                    appname = KiTTYClassName;
                }
            } else if (!strcmp(p, "-mungestr")) {
                /* Utility: print the munged form of a string and quit. */
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                const char *in = cmdline_arg_to_str(arglist->args[arglistpos++]);
                char *b = snewn(4 * strlen(in) + 1, char);
                mungestr(in, b);
                MessageBox(NULL, b, "mungestr", MB_OK);
                SetTextToClipboard(b);
                sfree(b);
                cleanup_exit(0);
            } else if (!strcmp(p, "-sendcmd")) {
                /* Send a command to all running KiTTY windows, then quit. */
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                char *cmd = dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
                if (strlen(cmd) > 0)
                    SendCommandAllWindows(NULL, cmd);
                sfree(cmd);
                cleanup_exit(0);
            } else if (!strcmp(p, "-edit")) {
                /* Open the KiTTY session-file editor on a file, then quit. */
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                char *ef = dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
                if (existfile(ef))
                    RunPuttyEd(NULL, ef);
                else
                    MessageBox(NULL, "Unable to find requested file",
                               "Error", MB_OK | MB_ICONERROR);
                sfree(ef);
                cleanup_exit(0);
            } else if (!strcmp(p, "-savedump")) {
                /* Dump the full KiTTY configuration to a file, then quit. */
                SaveDump();
                cleanup_exit(0);
            } else if (!strcmp(p, "-fileassoc")) {
                /* Register the KiTTY .ktx file association, then quit.
                 * Writes HKCR\kitty.connect.1 + the extension key (redirected
                 * to HKCU\Software\Classes when not elevated). */
                CreateFileAssoc();
                cleanup_exit(0);
            } else if (!strcmp(p, "-sshhandler")) {
                /* Register KiTTY as the telnet/ssh/putty URL protocol handler,
                 * then quit. */
                CreateSSHHandler();
                cleanup_exit(0);
#endif
            } else if (!strcmp(p, "-cleanup")) {
                /*
                 * `putty -cleanup'. Remove all registry
                 * entries associated with PuTTY, and also find
                 * and delete the random seed file.
                 */
                char *s1, *s2;
                s1 = dupprintf("This procedure will remove ALL Registry entries\n"
                               "associated with %s, and will also remove\n"
                               "the random seed file. (This only affects the\n"
                               "currently logged-in user.)\n"
                               "\n"
                               "THIS PROCESS WILL DESTROY YOUR SAVED SESSIONS.\n"
                               "Are you really sure you want to continue?",
                               appname);
                s2 = dupprintf("%s Warning", appname);
                if (message_box(NULL, s1, s2,
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2,
                                false, HELPCTXID(option_cleanup)) == IDYES) {
                    cleanup_all();
                }
                sfree(s1);
                sfree(s2);
                exit(0);
            } else if (!strcmp(p, "-pgpfp")) {
                pgp_fingerprints_msgbox(NULL);
                exit(0);
            } else if (has_ca_config_box &&
                       (!strcmp(p, "-host-ca") || !strcmp(p, "--host-ca") ||
                        !strcmp(p, "-host_ca") || !strcmp(p, "--host_ca"))) {
                show_ca_config_box(NULL);
                exit(0);
            } else if (!strcmp(p, "-demo-config-box")) {
                if (!arglist->args[arglistpos]) {
                    cmdline_error("%s expects an output filename", p);
                } else {
                    demo_config_box = true;
                    dialog_box_demo_screenshot_filename =
                        cmdline_arg_to_filename(arglist->args[arglistpos++]);
                }
            } else if (!strcmp(p, "-demo-terminal")) {
                if (!arglist->args[arglistpos] ||
                    !arglist->args[arglistpos+1]) {
                    cmdline_error("%s expects input and output filenames", p);
                } else {
                    const char *infile =
                        cmdline_arg_to_str(arglist->args[arglistpos++]);
                    terminal_demo_screenshot_filename =
                        cmdline_arg_to_filename(arglist->args[arglistpos++]);
                    FILE *fp = fopen(infile, "rb");
                    if (!fp)
                        cmdline_error("can't open input file '%s'", infile);
                    demo_terminal_data = strbuf_new();
                    char buf[4096];
                    int retd;
                    while ((retd = fread(buf, 1, sizeof(buf), fp)) > 0)
                        put_data(demo_terminal_data, buf, retd);
                    fclose(fp);
                }
            } else if (*p != '-') {
                cmdline_error("unexpected argument \"%s\"", p);
            } else {
                cmdline_error("unknown option \"%s\"", p);
            }
        }
    }

    NETDBG_TS("cmdline: before cmdline_run_saved");
    cmdline_run_saved(conf);
    NETDBG_TS("cmdline: after cmdline_run_saved");

    if (demo_config_box) {
        sesslist_demo_mode = true;
        load_open_settings(NULL, conf);
        conf_set_str(conf, CONF_host, "demo-server.example.com");
        do_config(conf);
        cleanup_exit(0);
    } else if (demo_terminal_data) {
        /* Ensure conf will cause an immediate session launch */
        load_open_settings(NULL, conf);
        conf_set_str(conf, CONF_host, "demo-server.example.com");
        conf_set_int(conf, CONF_close_on_exit, FORCE_OFF);
    } else {
        /*
         * Bring up the config dialog if the command line hasn't
         * (explicitly) specified a launchable configuration.
         */
        if (!(special_launchable_argument || cmdline_host_ok(conf))) {
#ifdef MOD_PERSO
            /* KiTTY: auto-load the last-used session into the config box so it
             * opens pre-filled (and the saved-session list auto-selects it).
             * Only if it still exists. */
            {
                char lastsess[512];
                if (kitty_get_last_session(lastsess, sizeof(lastsess)) &&
                    *lastsess && strcmp(lastsess, "Default Settings") != 0) {
                    struct sesslist sl;
                    int i, found = 0;
                    get_sesslist(&sl, true);
                    for (i = 0; i < sl.nsessions; i++)
                        if (!strcmp(sl.sessions[i], lastsess)) { found = 1; break; }
                    get_sesslist(&sl, false);
                    if (found)
                        load_settings(lastsess, conf);
                }
            }
#endif
            NETDBG_TS("cmdline: before do_config (config box)");
            if (!do_config(conf))
                cleanup_exit(0);
            NETDBG_TS("cmdline: after do_config (user closed config box)");
        }
    }

    NETDBG_TS("cmdline: before prepare_session");
    prepare_session(conf);
    NETDBG_TS("cmdline: after prepare_session / return");
}

const struct BackendVtable *backend_vt_from_conf(Conf *conf)
{
    if (demo_terminal_data) {
        return &null_backend;
    }

    /*
     * Select protocol. This is farmed out into a table in a
     * separate file to enable an ssh-free variant.
     */
    const struct BackendVtable *vt = backend_vt_from_proto(
        conf_get_int(conf, CONF_protocol));
    if (!vt) {
        char *str = dupprintf("%s Internal Error", appname);
        MessageBox(NULL, "Unsupported protocol number found",
                   str, MB_OK | MB_ICONEXCLAMATION);
        sfree(str);
        cleanup_exit(1);
    }
    return vt;
}

const wchar_t *get_app_user_model_id(void)
{
#ifdef MOD_PERSO
    /* KiTTY: must match the AppUserModelID the installer puts on the pinned
     * shortcuts ("kappernet.KiTTY"); otherwise the running window's taskbar
     * button groups under an unregistered AUMID and shows a blank icon even
     * though its window icon is valid. */
    return L"kappernet.KiTTY";
#else
    return L"SimonTatham.PuTTY";
#endif
}

static void demo_terminal_screenshot(void *ctx, unsigned long now)
{
    HWND hwnd = (HWND)ctx;
    char *err = save_screenshot(hwnd, terminal_demo_screenshot_filename);
    if (err) {
        MessageBox(hwnd, err, "Demo screenshot failure", MB_OK | MB_ICONERROR);
        sfree(err);
    }
    cleanup_exit(0);
}

void gui_terminal_ready(HWND hwnd, Seat *seat, Backend *backend)
{
    if (demo_terminal_data) {
        ptrlen data = ptrlen_from_strbuf(demo_terminal_data);
        seat_stdout(seat, data.ptr, data.len);
        schedule_timer(TICKSPERSEC, demo_terminal_screenshot, (void *)hwnd);
    }
}
