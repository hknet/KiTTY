#include "putty.h"
#include "storage.h"

#ifdef MOD_PERSO
/* KiTTY helpers (putty.c does not include kitty.h) */
extern char *SetSessPath(const char *);
extern int  GetDirectoryBrowseFlag(void);
extern void load_open_settings_forced(char *filename, Conf *conf); /* kitty_settings_load.c */
extern char *kitty_cli_loginscript; /* kitty_bridge.c: -loginscript, consumed post-create */
/* -exportall <dir> / -importdir <dir>: whole-store move; stashed here and run
 * just before the config box (storage backend is initialised by then), then
 * exit. kitty_export_all_to_dir/kitty_import_dir are the no-UI cores. */
int  kitty_export_all_to_dir(const char *dir, int *failOut);
int  kitty_import_dir(const char *dir, int *failOut, int *proxyOut,
                      int *skippedOut, int overwrite);
static char *kitty_cli_exportdir = NULL;
static char *kitty_cli_importdir = NULL;
/* Bundle transport protection for the do-and-exit paths above. The password is
 * taken from a FILE, never from argv: a command-line password is visible in the
 * process list, in Task Manager and in shell history. (A password sitting in a
 * file readable by the same account is its own compromise - it is the caller's
 * job to place and remove it.) The DPAPI switch is for a scripted local backup,
 * which needs no password at all because the bundle never leaves the machine. */
static char *kitty_cli_bundlepw = NULL;
static bool kitty_cli_bundle_thispc = false;
/* do-and-exit / pre-window utility switches (kitty modules; putty.c lacks kitty.h) */
extern char KiTTYClassName[];                       /* kitty.c: window class name */
extern int  SendCommandAllWindows(HWND hwnd, char *cmd); /* kitty.c */
extern void RunPuttyEd(HWND hwnd, char *filename);  /* kitty_win.c: session-file editor */
extern int  SetTextToClipboard(const char *buf);    /* kitty_win.c */
extern void mungestr(const char *in, char *out);    /* kitty_commun.c */
extern int  existfile(const char *filename);        /* kitty_tools.c */
extern void CreateFileAssoc(void);                  /* kitty_registry.c: .ktx file association */
extern void CreateSSHHandler(void);                 /* kitty_registry.c: telnet/ssh/putty URL handlers */
extern int  kitty_get_last_session(char *buf, int buflen); /* storage.c: remember-last-session */
extern int  GetLoadLastSessionFlag(void);           /* kitty.c: [ConfigBox] loadlastsession */
extern void SetQuickConnectMode(const int flag);    /* kitty.c: #23 quick connect */

static void kitty_settings_load_hook(const char *section, Conf *conf, bool exists)
{
    /* KiTTY: remember the real saved session name so placeholders like %%s
     * can be expanded in the window title. Keep shared settings.c free of
     * KiTTY policy: it only calls this hook when the KiTTY GUI target registers
     * it. */
    if (exists && section && *section &&
        strcmp(section, "Default Settings") != 0)
        conf_set_str(conf, CONF_sessionname, section);
}
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

#ifdef MOD_PERSO
    settings_set_load_hook(kitty_settings_load_hook);
#endif
    do_defaults(NULL, conf);
    NETDBG_TS("cmdline: after do_defaults");

    p = handle_restrict_acl_cmdline_prefix(cmdline);
#ifdef MOD_PERSO
    /* KiTTY: consume a "&K<handle>:<size>" master-key token from a parent KiTTY
     * (launcher-mpw-sharing) BEFORE the session is loaded, so the child unlocks
     * from the inherited key instead of re-prompting. No token -> p unchanged. */
    { extern char *kitty_mpw_import_inherit_blob(char *); p = kitty_mpw_import_inherit_blob(p); }
#endif

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
            } else if (!strcmp(p, "-mpwkey")) {
                /* KiTTY: inherited master-key handle from a parent KiTTY
                 * (launcher-mpw-sharing). Consume it BEFORE any later -load so the
                 * session's stored password decrypts without a prompt. */
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires an argument", p);
                { extern void kitty_mpw_consume_handle_str(const char *);
                  kitty_mpw_consume_handle_str(
                      cmdline_arg_to_str(arglist->args[arglistpos++])); }
            } else if (!strcmp(p, "-fullscreen")) {
                conf_set_int(conf, CONF_fullscreen, 1);
            } else if (!strcmp(p, "-send-to-tray")) {
                /* KiTTY: start the session straight in the systray (tunnels).
                 * Sets the process-global rather than CONF_sendtotray so it
                 * cannot be undone by a "-load" appearing later on the command
                 * line - which is exactly the order the launcher writes into
                 * the shortcuts it creates ("-load NAME -send-to-tray"). */
                { extern void SetAutoSendToTray(const int flag);
                  SetAutoSendToTray(1); }
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
            } else if (!strcmp(p, "-exportall")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires a directory argument", p);
                sfree(kitty_cli_exportdir);
                kitty_cli_exportdir =
                    dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
            } else if (!strcmp(p, "-importdir")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires a directory argument", p);
                sfree(kitty_cli_importdir);
                kitty_cli_importdir =
                    dupstr(cmdline_arg_to_str(arglist->args[arglistpos++]));
            } else if (!strcmp(p, "-bundlepwfile")) {
                if (!arglist->args[arglistpos])
                    cmdline_error("option \"%s\" requires a file argument", p);
                {
                    const char *path = cmdline_arg_to_str(arglist->args[arglistpos++]);
                    FILE *fp = fopen(path, "r");
                    if (!fp)
                        cmdline_error("unable to open bundle-password file '%s'", path);
                    else {
                        char *pw = chomp(fgetline(fp));
                        fclose(fp);
                        if (!pw || !pw[0])
                            cmdline_error("unable to read a password from file '%s'", path);
                        else {
                            sfree(kitty_cli_bundlepw);
                            kitty_cli_bundlepw = pw;
                            pw = NULL;
                        }
                        if (pw) sfree(pw);
                    }
                }
            } else if (!strcmp(p, "-bundlethispc")) {
                kitty_cli_bundle_thispc = true;
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

#ifdef MOD_PERSO
    /* Whole-store export/import (do-and-exit). Runs here, after the storage
     * backend is initialised, so it targets the active store (registry or
     * portable). The bundle carries its OWN protection - a password from
     * -bundlepwfile, or -bundlethispc for a local-only backup - so neither path
     * touches the store's master password. */
    extern void kitty_set_bundle_passphrase(const char *pass);
    extern void kitty_set_bundle_dpapi_only(int on);
    extern void kitty_set_bundle_import(int on);
    extern void kitty_clear_bundle_context(void);
    extern int  kitty_bundle_wrap_failed(void);
    extern int  kitty_bundle_needs_password(const char *dir);
    if (kitty_cli_exportdir) {
        int fail = 0, n, wrapfailed;
        char msg[700];
        /* No silent fallback, exactly as in the GUI: without one of the two
         * switches this used to CREATE a master password for the user's own
         * store as a side effect. Say what to pass instead of doing that. */
        if (!kitty_cli_bundlepw && !kitty_cli_bundle_thispc) {
            MessageBoxA(NULL,
                "Say how the exported sessions should be protected:\n\n"
                "  -bundlepwfile <file>   password (first line of the file);\n"
                "                         the bundle then imports on any PC\n"
                "  -bundlethispc          no password; the bundle imports only\n"
                "                         with this Windows account on this PC\n\n"
                "Nothing was exported.",
                "KiTTY session export", MB_OK | MB_ICONWARNING);
            cleanup_exit(1);
        }
        if (kitty_cli_bundle_thispc) kitty_set_bundle_dpapi_only(1);
        else                         kitty_set_bundle_passphrase(kitty_cli_bundlepw);
        n = kitty_export_all_to_dir(kitty_cli_exportdir, &fail);
        wrapfailed = kitty_bundle_wrap_failed();
        kitty_clear_bundle_context();
        snprintf(msg, sizeof(msg), "Exported %d session(s), %d failed, to:\n%s%s",
                 n, fail, kitty_cli_exportdir,
                 (kitty_cli_bundle_thispc || wrapfailed)
                   ? "\n\nThese sessions can only be imported with this Windows "
                     "account on this PC."
                   : "");
        MessageBoxA(NULL, msg, "KiTTY session export",
                    MB_OK | ((fail || wrapfailed) ? MB_ICONWARNING : MB_ICONINFORMATION));
        cleanup_exit(fail ? 1 : 0);
    }
    if (kitty_cli_importdir) {
        int fail = 0, prox = 0, n;
        char msg[700];
        /* A password-protected bundle with no password given would fall through
         * to the master-password prompt - which is interactive, and this path
         * exists for scripts. Refuse with the switch name instead. */
        if (kitty_bundle_needs_password(kitty_cli_importdir) && !kitty_cli_bundlepw) {
            MessageBoxA(NULL,
                "These exported sessions are password-protected. Supply the "
                "import password with:\n\n"
                "  -bundlepwfile <file>   (the password on the first line)\n\n"
                "Nothing was imported.",
                "KiTTY session import", MB_OK | MB_ICONWARNING);
            cleanup_exit(1);
        }
        if (kitty_cli_bundlepw) {
            kitty_set_bundle_import(1);
            kitty_set_bundle_passphrase(kitty_cli_bundlepw);
        }
        n = kitty_import_dir(kitty_cli_importdir, &fail, &prox, NULL, 1);
        kitty_clear_bundle_context();
        snprintf(msg, sizeof(msg), "Imported %d session(s), %d prox(ies), %d failed, from:\n%s",
                 n, prox, fail, kitty_cli_importdir);
        MessageBoxA(NULL, msg, "KiTTY session import",
                    MB_OK | (fail ? MB_ICONWARNING : MB_ICONINFORMATION));
        cleanup_exit(fail ? 1 : 0);
    }
#endif

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
            /* KiTTY: prompt once, up front, to unlock the portable master
             * password (launcher-mpw-sharing "unlock at startup") BEFORE the
             * last-session pre-fill below, so the pre-fill decrypts the stored
             * password into conf - an unlocked store bypasses the defer, so the
             * pre-filled session can be Opened directly without retyping it.
             * No-op in registry mode / already unlocked (e.g. a key inherited
             * from a parent KiTTY) / when no master password is set. */
            { extern int kitty_mpw_startup_unlock(void); kitty_mpw_startup_unlock(); }
            /* KiTTY: auto-load the last-used session into the config box so it
             * opens pre-filled (and the saved-session list auto-selects it).
             * Only if it still exists.
             *
             * Two ways out of it, both quick connect (hknet/KiTTY#23): the box
             * comes up on Default Settings with the caret in Host Name, so a
             * typed host always starts from the same known configuration. Set
             * [ConfigBox] loadlastsession=no to work that way permanently; or
             * simply load "Default Settings" once, which is remembered like any
             * other session and arms the mode until another session is loaded.
             * The second needs no setting and no switching back and forth. */
            {
                char lastsess[512];
                bool havelast = kitty_get_last_session(lastsess,
                                                       sizeof(lastsess)) &&
                                *lastsess;

                if (!GetLoadLastSessionFlag() ||
                    (havelast && !strcmp(lastsess, "Default Settings"))) {
                    SetQuickConnectMode(1);
                } else if (havelast) {
                    struct sesslist sl;
                    int i, found = 0;
                    get_sesslist(&sl, true);
                    for (i = 0; i < sl.nsessions; i++)
                        if (!strcmp(sl.sessions[i], lastsess)) { found = 1; break; }
                    get_sesslist(&sl, false);
                    if (found) {
                        /* Defer any master-password prompt: the startup pre-fill
                         * must not force an unlock before the config box even
                         * appears. A protected password loads locked (empty);
                         * an explicit Load / "show password" / connect prompts
                         * at the real point of use. */
                        extern void kitty_set_defer_mpw_prompt(int);
                        kitty_set_defer_mpw_prompt(1);
                        load_settings(lastsess, conf);
                        kitty_set_defer_mpw_prompt(0);
                    }
                }
            }
#endif
#ifdef MOD_PERSO
            /* KiTTY: register a BARE relaunch (no arguments -> reopens the
             * configuration box) BEFORE we show it, so that if an in-place MSI
             * upgrade closes this window while the config box is open - the
             * user-reported case of "the update was started from the config
             * window and it never came back" - the Restart Manager brings the
             * config box back, like it does the tray apps. If the user then Opens
             * a saved session, the registration just before prepare_session below
             * upgrades this to -load "NAME"; an ad-hoc/host-typed Open leaves the
             * bare config-box relaunch in place (we can't restore a live ad-hoc
             * connection, but a reappearing window beats a silent vanish). */
            RegisterApplicationRestart(L"", 0);
#endif
            NETDBG_TS("cmdline: before do_config (config box)");
            if (!do_config(conf))
                cleanup_exit(0);
#ifdef MOD_PERSO
            /* KiTTY: back up the config store when the startup config box is
             * committed (Open) - portable: a dated Backups\ folder; registry:
             * kitty084.sav. Previously only a mid-session "Change Settings" apply
             * did this, so a portable user who never reconfigures mid-session
             * got no backup. Self-skips when disabled / no sav target. */
            { extern void SaveRegistryKey(void); SaveRegistryKey(); }
#endif
            NETDBG_TS("cmdline: after do_config (user closed config box)");
        }
    }

#ifdef MOD_PERSO
    /* KiTTY: register this terminal with the MSI Restart Manager so an in-place
     * upgrade relaunches it afterwards, reconnecting the session. We do it HERE
     * rather than from the raw command line in WinMain so that EVERY launch path
     * is covered uniformly: command-line "-load NAME", the "@NAME" shortcut, and
     * - the case the old WinMain check missed - a saved session Opened from the
     * config box (no launchable argument on the command line). conf is fully
     * populated at this point, so CONF_sessionname reliably holds the saved
     * session's name, or is empty for an ad-hoc/host-typed terminal.
     *
     * We rebuild a CLEAN `-load "NAME"` from that name instead of replaying the
     * original command line: launcher-spawned sessions carry a "-mpwkey <handle>"
     * token whose inherited handle is DEAD in the relaunched process (so the
     * replayed relaunch failed to start), plus "-send-to-tray".
     *
     * A session that knows its saved-session name registers as `-load "NAME"`.
     * That includes sessions arriving via the &filemap conf-passing path
     * (kitty_bridge.c / RunConfig: config-box Start, Duplicate Session, "open
     * new with current settings", password-auth sessions): conf_serialise
     * carries every set key, NOT_SAVED ones like CONF_sessionname included, so
     * the child knows its name (measured 2026-07-22: a Start-spawned child
     * registers -load). Only a genuinely unnamed launchable session (ad-hoc
     * host typed into the box) registers a BARE relaunch, which reopens the
     * config box - a blank reopen beats a silent vanish.
     *
     * History, so nobody re-breaks this: the .58 upgrade rollback ("a critical
     * application holds files in use - reboot necessary") was NOT caused by a
     * missing name here - it was the always-on restricted-ACL bug in
     * RunConfig (kitty_launcher.c, fixed alongside this comment): the Restart
     * Manager cannot inspect an ACL-restricted process, classifies it
     * RmCritical, and then neither shows its files-in-use dialog nor closes/
     * restarts ANY window; the installer's CloseApplication step rescues the
     * upgrade but nothing reopens. Registration here only helps once the
     * process is inspectable. Quoting matches the other KiTTY `-load "NAME"`
     * builders (plain double quotes, no escaping). */
    {
        const char *sessname = conf_get_str(conf, CONF_sessionname);
        if (sessname && *sessname) {
            char rcl[2048];
            wchar_t wcl[2048];
            snprintf(rcl, sizeof(rcl), "-load \"%s\"", sessname);
            if (MultiByteToWideChar(CP_ACP, 0, rcl, -1, wcl,
                                    sizeof(wcl)/sizeof(wcl[0])) > 0)
                RegisterApplicationRestart(wcl, 0);
        } else if (conf_launchable(conf)) {
            RegisterApplicationRestart(L"", 0);
        }
    }
#endif

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
