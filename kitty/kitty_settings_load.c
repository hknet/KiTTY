/*
 * kitty_settings_load.c — load a KiTTY ".ktx" session file into a Conf.
 *
 * Ported from KiTTY 0.76b kitty_settings.c (load_open_settings_forced + the
 * read_setting_*_forced / gpp*_forced / gprefs_forced / read_clip_setting_forced
 * helpers) onto PuTTY 0.84. This is the read-side counterpart of
 * kitty_settings_forced.c (which holds the write side). The two files share no
 * symbols: the write helpers live only in kitty_settings_forced.c, the read
 * helpers only here, so both link into the same target without collision.
 *
 * 0.84 adaptations:
 *  - decryptstring is now 3-arg (mode, st, key); bridged through GetCryptSaltFlag()
 *    exactly as kitty_settings_forced.c bridges cryptstring. The per-line
 *    "decrypt only lines that don't end in backslash" auto-detection from the
 *    original is preserved (more robust than gating on the global CryptFileFlag,
 *    which need not be set when a file is loaded).
 *  - key2val + the cipher/kex/hostkey name tables are static in settings.c and
 *    not exported; kept as local copies here (in sync with settings.c /
 *    kitty_settings_forced.c).
 *  - gprefs_from_str is static in settings.c; a private copy is kept here.
 *  - filename arg stays char*; the -kload caller (putty.c) dups the const
 *    cmdline string before passing it in.
 */
#include "putty.h"         /* first: pulls winsock2.h before windows.h on Windows */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "kitty.h"
#include "kitty_defs.h"     /* KITTY_DEFAULT_SESSION */
#include "kitty_commun.h"  /* GetCryptSaltFlag */
#include "kitty_crypt.h"   /* decryptstring */

/* CryptFileFlag lives in kitty_bridge.c (same as the write side). */
extern int CryptFileFlag;
int decryptstring(const int mode, char *st, const char *key);
/* unmungestr is declared in kitty_commun.h (const char *in). */

/* Constants that were file-local in the original KiTTY tree. */
#ifndef PRINT_TO_CLIPBOARD_STRING
#define PRINT_TO_CLIPBOARD_STRING "Windows clipboard"  /* KiTTY 0.76b putty.h:33 */
#endif
#ifndef GSS_DEF_REKEY_MINS
#define GSS_DEF_REKEY_MINS 2     /* ssh/gss.h: default minutes between GSS cache checks */
#endif

/* ---- read-side forward declarations (load body calls helpers defined below) ---- */
int read_setting_i_forced(void *handle, const char *key, int defvalue);
char *read_setting_s_forced(void *handle, const char *key);
Filename *read_setting_filename_forced(void *handle, const char *key);
FontSpec *read_setting_fontspec_forced(void *handle, const char *name);
static bool gppb_raw_forced(void *sesskey, const char *name, bool def);
static void gppb_forced(void *sesskey, const char *name, bool def, Conf *conf, int primary);
static void gppi_forced(void *handle, const char *name, int def, Conf *conf, int primary);
static int gppi_raw_forced(void *handle, const char *name, int def);
static void gppfile_forced(void *handle, const char *name, Conf *conf, int primary);
static void gpps_forced(void *handle, const char *name, const char *def, Conf *conf, int primary);
static char *gpps_raw_forced(void *handle, const char *name, const char *def);
static void gppfont_forced(void *handle, const char *name, Conf *conf, int primary);
static int gppmap_forced(void *handle, const char *name, Conf *conf, int primary);
static void gprefs_forced(void *sesskey, const char *name, const char *def, const struct keyvalwhere *mapping, int nvals, Conf *conf, int primary);
static void read_clip_setting_forced(void *sesskey, char *savekey, int def, Conf *conf, int confkey, int strconfkey);

/* ---- key2val + name tables + gprefs_from_str (static in settings.c) ---- */
static int key2val(const struct keyvalwhere *mapping, int nmaps, char *key) {
    int i;
    for (i = 0; i < nmaps; i++)
        if (!strcmp(mapping[i].s, key)) return mapping[i].v;
    return -1;
}

static const struct keyvalwhere ciphernames[] = {
    { "aes",        CIPHER_AES,             -1, -1 },
    { "chacha20",   CIPHER_CHACHA20,        CIPHER_AES, +1 },
    { "aesgcm",     CIPHER_AESGCM,          CIPHER_CHACHA20, +1 },
    { "3des",       CIPHER_3DES,            -1, -1 },
    { "WARN",       CIPHER_WARN,            -1, -1 },
    { "des",        CIPHER_DES,             -1, -1 },
    { "blowfish",   CIPHER_BLOWFISH,        -1, -1 },
    { "arcfour",    CIPHER_ARCFOUR,         -1, -1 },
};

static const struct keyvalwhere kexnames[] = {
    { "ntru-curve25519",    KEX_NTRU_HYBRID, -1, +1 },
    { "mlkem-curve25519",   KEX_MLKEM_25519_HYBRID, KEX_NTRU_HYBRID, +1 },
    { "mlkem-nist",         KEX_MLKEM_NIST_HYBRID, KEX_MLKEM_25519_HYBRID, +1 },
    { "ecdh",               KEX_ECDH,       -1, +1 },
    { "dh-gex-sha1",        KEX_DHGEX,      -1, -1 },
    { "dh-group14-sha1",    KEX_DHGROUP14,  -1, -1 },
    { "dh-group1-sha1",     KEX_DHGROUP1,   KEX_WARN, +1 },
    { "rsa",                KEX_RSA,        KEX_WARN, -1 },
    { "dh-group15-sha512",  KEX_DHGROUP15,  KEX_DHGROUP14, -1 },
    { "dh-group16-sha512",  KEX_DHGROUP16,  KEX_DHGROUP15, -1 },
    { "dh-group17-sha512",  KEX_DHGROUP17,  KEX_DHGROUP16, +1 },
    { "dh-group18-sha512",  KEX_DHGROUP18,  KEX_DHGROUP17, +1 },
    { "WARN",               KEX_WARN,       -1, -1 }
};

static const struct keyvalwhere hknames[] = {
    { "ed25519",    HK_ED25519,             -1, +1 },
    { "ed448",      HK_ED448,               -1, +1 },
    { "ecdsa",      HK_ECDSA,               -1, -1 },
    { "dsa",        HK_DSA,                 -1, -1 },
    { "rsa",        HK_RSA,                 -1, -1 },
    { "WARN",       HK_WARN,                -1, -1 },
};

static void gprefs_from_str(const char *str,
                            const struct keyvalwhere *mapping, int nvals,
                            Conf *conf, int primary)
{
    char *commalist = dupstr(str);
    char *p, *q;
    int i, j, n, v, pos;
    unsigned long seen = 0;            /* bitmap for weeding dups etc */

    n = 0;
    p = commalist;
    while (1) {
        while (*p && *p == ',') p++;
        if (!*p)
            break;                     /* no more words */

        q = p;
        while (*p && *p != ',') p++;
        if (*p) *p++ = '\0';

        v = key2val(mapping, nvals, q);
        if (v != -1 && !(seen & (1 << v))) {
            seen |= (1 << v);
            conf_set_int_int(conf, primary, n, v);
            n++;
        }
    }

    sfree(commalist);

    while (n < nvals) {
        for (i = 0; i < nvals; i++) {
            assert(mapping[i].v >= 0);
            assert(mapping[i].v < 32);

            if (!(seen & (1 << mapping[i].v))) {
                /*
                 * This element needs adding. But can we add it yet?
                 */
                if (mapping[i].vrel != -1 && !(seen & (1 << mapping[i].vrel)))
                    continue;          /* nope */

                /*
                 * OK, we can work out where to add this element, so
                 * do so.
                 */
                if (mapping[i].vrel == -1) {
                    pos = (mapping[i].where < 0 ? n : 0);
                } else {
                    for (j = 0; j < n; j++)
                        if (conf_get_int_int(conf, primary, j) ==
                            mapping[i].vrel)
                            break;
                    assert(j < n);     /* implied by (seen & (1<<vrel)) */
                    pos = (mapping[i].where < 0 ? j : j+1);
                }

                /*
                 * And add it.
                 */
                for (j = n-1; j >= pos; j--)
                    conf_set_int_int(conf, primary, j+1,
                                     conf_get_int_int(conf, primary, j));
                conf_set_int_int(conf, primary, pos, mapping[i].v);
                seen |= (1 << mapping[i].v);
                n++;
            }
        }
    }
}

/* ================= extracted from KiTTY 0.76b kitty_settings.c ================= */

/* ---- load_open_settings_forced (kitty_settings.c:465-1122) ---- */
void load_open_settings_forced(char *filename, Conf *conf) {
	FILE *sesskey ;
	if( (sesskey=fopen(filename,"r")) == NULL ) { 
		char buffer[1024] ;
		snprintf(buffer,sizeof(buffer),"File %s not found !",filename);
		MessageBox(NULL, buffer, "Error", MB_OK|MB_ICONERROR) ; return ; 
		}
	Conf * confDef ;
	confDef = conf_new() ;
	do_defaults( KITTY_DEFAULT_SESSION , confDef);
		
// BEGIN COPY/PASTE
    int i;
    char *prot;
#ifdef MOD_PERSO
    /*
     * HACK: PuTTY-url
     * Set font quality to cleartype on Windows Vista and above
     */
    OSVERSIONINFO versioninfo;
    versioninfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&versioninfo);
#endif
    conf_set_bool(conf, CONF_ssh_subsys, false); /* FIXME: load this properly */
    conf_set_str(conf, CONF_remote_cmd, "");
    conf_set_str(conf, CONF_remote_cmd2, "");
    conf_set_str(conf, CONF_ssh_nc_host, "");

    gpps_forced(sesskey, "HostName", "", conf, CONF_host);
    gppfile_forced(sesskey, "LogFileName", conf, CONF_logfilename);
    gppi_forced(sesskey, "LogType", 0, conf, CONF_logtype);
    gppi_forced(sesskey, "LogFileClash", LGXF_ASK, conf, CONF_logxfovr);
    gppb_forced(sesskey, "LogFlush", true, conf, CONF_logflush);
    gppb_forced(sesskey, "LogHeader", true, conf, CONF_logheader);
    gppb_forced(sesskey, "SSHLogOmitPasswords", true, conf, CONF_logomitpass);
    gppb_forced(sesskey, "SSHLogOmitData", false, conf, CONF_logomitdata);

    prot = gpps_raw_forced(sesskey, "Protocol", "default");
    conf_set_int(conf, CONF_protocol, be_default_protocol);
    conf_set_int(conf, CONF_port, 0);
    {
        const struct BackendVtable *vt = backend_vt_from_name(prot);
        if (vt) {
            conf_set_int(conf, CONF_protocol, vt->protocol);
	    gppi_forced(sesskey, "PortNumber", vt->default_port, conf, CONF_port);
	}
    }
    sfree(prot);

    /* Address family selection */
    gppi_forced(sesskey, "AddressFamily", ADDRTYPE_UNSPEC, conf, CONF_addressfamily);

    /* The CloseOnExit numbers are arranged in a different order from
     * the standard FORCE_ON / FORCE_OFF / AUTO. */
    i = gppi_raw_forced(sesskey, "CloseOnExit", 1); conf_set_int(conf, CONF_close_on_exit, (i+1)%3);
    gppb_forced(sesskey, "WarnOnClose", true, conf, CONF_warn_on_close);
    {
	/* This is two values for backward compatibility with 0.50/0.51 */
	int pingmin, pingsec;
	pingmin = gppi_raw_forced(sesskey, "PingInterval", 0);
	pingsec = gppi_raw_forced(sesskey, "PingIntervalSecs", 0);
	conf_set_int(conf, CONF_ping_interval, pingmin * 60 + pingsec);
    }
    gppb_forced(sesskey, "TCPNoDelay", true, conf, CONF_tcp_nodelay);
    gppb_forced(sesskey, "TCPKeepalives", false, conf, CONF_tcp_keepalives);
    gpps_forced(sesskey, "TerminalType", "xterm", conf, CONF_termtype);
    gpps_forced(sesskey, "TerminalSpeed", "38400,38400", conf, CONF_termspeed);
    if (gppmap_forced(sesskey, "TerminalModes", conf, CONF_ttymodes)) {
	/*
	 * Backwards compatibility with old saved settings.
	 *
	 * From the invention of this setting through 0.67, the set of
	 * terminal modes was fixed, and absence of a mode from this
	 * setting meant the user had explicitly removed it from the
	 * UI and we shouldn't send it.
	 *
	 * In 0.68, the IUTF8 mode was added, and in handling old
	 * settings we inadvertently removed the ability to not send
	 * a mode. Any mode not mentioned was treated as if it was
	 * set to 'auto' (A).
	 *
	 * After 0.68, we added explicit notation to the setting format
	 * when the user removes a known terminal mode from the list.
	 *
	 * So: if any of the modes from the original set is missing, we
	 * assume this was an intentional removal by the user and add
	 * an explicit removal ('N'); but if IUTF8 (or any other mode
	 * added after 0.67) is missing, we assume that its absence is
	 * due to the setting being old rather than intentional, and
	 * add it with its default setting.
	 *
	 * (This does mean that if a 0.68 user explicitly removed IUTF8,
	 * we add it back; but removing IUTF8 had no effect in 0.68, so
	 * we're preserving behaviour, which is the best we can do.)
	 */
	for (i = 0; ttymodes[i]; i++) {
	    if (!conf_get_str_str_opt(conf, CONF_ttymodes, ttymodes[i])) {
		/* Mode not mentioned in setting. */
		const char *def;
		if (!strcmp(ttymodes[i], "IUTF8")) {
		    /* Any new modes we add in future should be treated
		     * this way too. */
		    def = "A";  /* same as new-setting default below */
		} else {
		    /* One of the original modes. Absence is probably
		     * deliberate. */
		    def = "N";  /* don't send */
		}
		conf_set_str_str(conf, CONF_ttymodes, ttymodes[i], def);
	    }
	}
    } else {
	/* This hardcodes a big set of defaults in any new saved
	 * sessions. Let's hope we don't change our mind. */
	for (i = 0; ttymodes[i]; i++)
	    conf_set_str_str(conf, CONF_ttymodes, ttymodes[i], "A");
    }

    /* proxy settings */
    gpps_forced(sesskey, "ProxyExcludeList", "", conf, CONF_proxy_exclude_list);
    i = gppi_raw_forced(sesskey, "ProxyDNS", 1); conf_set_int(conf, CONF_proxy_dns, (i+1)%3);
    gppb_forced(sesskey, "ProxyLocalhost", false, conf, CONF_even_proxy_localhost);
    gppi_forced(sesskey, "ProxyMethod", -1, conf, CONF_proxy_type);
    if (conf_get_int(conf, CONF_proxy_type) == -1) {
        int i;
        i = gppi_raw_forced(sesskey, "ProxyType", 0);
        if (i == 0)
            conf_set_int(conf, CONF_proxy_type, PROXY_NONE);
        else if (i == 1)
            conf_set_int(conf, CONF_proxy_type, PROXY_HTTP);
        else if (i == 3)
            conf_set_int(conf, CONF_proxy_type, PROXY_TELNET);
        else if (i == 4)
            conf_set_int(conf, CONF_proxy_type, PROXY_CMD);
        else {
            i = gppi_raw_forced(sesskey, "ProxySOCKSVersion", 5);
            if (i == 5)
                conf_set_int(conf, CONF_proxy_type, PROXY_SOCKS5);
            else
                conf_set_int(conf, CONF_proxy_type, PROXY_SOCKS4);
        }
    }
    gpps_forced(sesskey, "ProxyHost", "proxy", conf, CONF_proxy_host);
    gppi_forced(sesskey, "ProxyPort", 80, conf, CONF_proxy_port);
    gpps_forced(sesskey, "ProxyUsername", "", conf, CONF_proxy_username);
    gpps_forced(sesskey, "ProxyPassword", "", conf, CONF_proxy_password);
    gpps_forced(sesskey, "ProxyTelnetCommand", "connect %host %port\\n",
	 conf, CONF_proxy_telnet_command);
    gppi_forced(sesskey, "ProxyLogToTerm", FORCE_OFF, conf, CONF_proxy_log_to_term);
    gppmap_forced(sesskey, "Environment", conf, CONF_environmt);
    gpps_forced(sesskey, "UserName", "", conf, CONF_username);
    gppb_forced(sesskey, "UserNameFromEnvironment", false,
         conf, CONF_username_from_env);
    gpps_forced(sesskey, "LocalUserName", "", conf, CONF_localusername);
    gppb_forced(sesskey, "NoPTY", false, conf, CONF_nopty);
    gppb_forced(sesskey, "Compression", false, conf, CONF_compression);
    gppb_forced(sesskey, "TryAgent", true, conf, CONF_tryagent);
    gppb_forced(sesskey, "AgentFwd", false, conf, CONF_agentfwd);
    gppb_forced(sesskey, "ChangeUsername", false, conf, CONF_change_username);
#ifndef NO_GSSAPI
    gppb_forced(sesskey, "GssapiFwd", false, conf, CONF_gssapifwd);
#endif
    gprefs_forced(sesskey, "Cipher", "\0",
	   ciphernames, CIPHER_MAX, conf, CONF_ssh_cipherlist);
    {
	/* Backward-compatibility: before 0.58 (when the "KEX"
	 * preference was first added), we had an option to
	 * disable gex under the "bugs" panel after one report of
	 * a server which offered it then choked, but we never got
	 * a server version string or any other reports. */
	const char *default_kexes,
		   *normal_default = "ecdh,dh-gex-sha1,dh-group14-sha1,rsa,"
		       "WARN,dh-group1-sha1",
		   *bugdhgex2_default = "ecdh,dh-group14-sha1,rsa,"
		       "WARN,dh-group1-sha1,dh-gex-sha1";
	char *raw;
	i = 2 - gppi_raw_forced(sesskey, "BugDHGEx2", 0);
	if (i == FORCE_ON)
            default_kexes = bugdhgex2_default;
	else
            default_kexes = normal_default;
	/* Migration: after 0.67 we decided we didn't like
	 * dh-group1-sha1. If it looks like the user never changed
	 * the defaults, quietly upgrade their settings to demote it.
	 * (If they did, they're on their own.) */
	raw = gpps_raw_forced(sesskey, "KEX", default_kexes);
	assert(raw != NULL);
	/* Lack of 'ecdh' tells us this was saved by 0.58-0.67
	 * inclusive. If it was saved by a later version, we need
	 * to leave it alone. */
	if (strcmp(raw, "dh-group14-sha1,dh-group1-sha1,rsa,"
		   "WARN,dh-gex-sha1") == 0) {
	    /* Previously migrated from BugDHGEx2. */
	    sfree(raw);
	    raw = dupstr(bugdhgex2_default);
	} else if (strcmp(raw, "dh-gex-sha1,dh-group14-sha1,"
			  "dh-group1-sha1,rsa,WARN") == 0) {
	    /* Untouched old default setting. */
	    sfree(raw);
	    raw = dupstr(normal_default);
	}
	/* (For the record: after 0.70, the default algorithm list
	 * very briefly contained the string 'gss-sha1-krb5'; this was
	 * never used in any committed version of code, but was left
	 * over from a pre-commit version of GSS key exchange.
	 * Mentioned here as it is remotely possible that it will turn
	 * up in someone's saved settings in future.) */

        gprefs_from_str(raw, kexnames, KEX_MAX, conf, CONF_ssh_kexlist);
	sfree(raw);
    }
    gprefs_forced(sesskey, "HostKey", "ed25519,ecdsa,rsa,dsa,WARN",
           hknames, HK_MAX, conf, CONF_ssh_hklist);
    gppb_forced(sesskey, "PreferKnownHostKeys", true, conf, CONF_ssh_prefer_known_hostkeys);
    gppi_forced(sesskey, "RekeyTime", 60, conf, CONF_ssh_rekey_time);
#ifndef NO_GSSAPI
    gppi_forced(sesskey, "GssapiRekey", GSS_DEF_REKEY_MINS, conf, CONF_gssapirekey);
#endif
    gpps_forced(sesskey, "RekeyBytes", "1G", conf, CONF_ssh_rekey_data);
    {
	/* SSH-2 only by default */
	int sshprot = gppi_raw_forced(sesskey, "SshProt", 3);
	/* Old sessions may contain the values corresponding to the fallbacks
	 * we used to allow; migrate them */
	if (sshprot == 1)      sshprot = 0; /* => "SSH-1 only" */
	else if (sshprot == 2) sshprot = 3; /* => "SSH-2 only" */
	conf_set_int(conf, CONF_sshprot, sshprot);
    }
    gpps_forced(sesskey, "LogHost", "", conf, CONF_loghost);
    gppb_forced(sesskey, "SSH2DES", false, conf, CONF_ssh2_des_cbc);
    gppb_forced(sesskey, "SshNoAuth", false, conf, CONF_ssh_no_userauth);
    gppb_forced(sesskey, "SshNoTrivialAuth", false, conf, CONF_ssh_no_trivial_userauth);
    gppb_forced(sesskey, "SshBanner", true, conf, CONF_ssh_show_banner);
    gppb_forced(sesskey, "AuthTIS", false, conf, CONF_try_tis_auth);
    gppb_forced(sesskey, "AuthKI", true, conf, CONF_try_ki_auth);
#ifndef NO_GSSAPI
    gppb_forced(sesskey, "AuthGSSAPI", true, conf, CONF_try_gssapi_auth);
    gppb_forced(sesskey, "AuthGSSAPIKEX", true, conf, CONF_try_gssapi_kex);
    gprefs_forced(sesskey, "GSSLibs", "\0",
	   gsslibkeywords, ngsslibs, conf, CONF_ssh_gsslist);
    gppfile_forced(sesskey, "GSSCustom", conf, CONF_ssh_gss_custom);
#endif
    gppb_forced(sesskey, "SshNoShell", false, conf, CONF_ssh_no_shell);
    gppfile_forced(sesskey, "PublicKeyFile", conf, CONF_keyfile);
    gpps_forced(sesskey, "RemoteCommand", "", conf, CONF_remote_cmd);
    gppb_forced(sesskey, "RFCEnviron", false, conf, CONF_rfc_environ);
    gppb_forced(sesskey, "PassiveTelnet", false, conf, CONF_passive_telnet);
    gppb_forced(sesskey, "BackspaceIsDelete", true, conf, CONF_bksp_is_delete);
    gppi_forced(sesskey, "EnterSendsCrLf", 0, conf, CONF_enter_sends_crlf);
    gppb_forced(sesskey, "RXVTHomeEnd", false, conf, CONF_rxvt_homeend); /* 0.84: BOOL */
    gppi_forced(sesskey, "LinuxFunctionKeys", 0, conf, CONF_funky_type);
    gppb_forced(sesskey, "NoApplicationKeys", false, conf, CONF_no_applic_k);
    gppb_forced(sesskey, "NoApplicationCursors", false, conf, CONF_no_applic_c);
    gppb_forced(sesskey, "NoMouseReporting", false, conf, CONF_no_mouse_rep);
    gppb_forced(sesskey, "NoRemoteResize", false, conf, CONF_no_remote_resize);
    gppb_forced(sesskey, "NoAltScreen", false, conf, CONF_no_alt_screen);
    gppb_forced(sesskey, "NoRemoteWinTitle", false, conf, CONF_no_remote_wintitle);
    gppb_forced(sesskey, "NoRemoteClearScroll", false,
         conf, CONF_no_remote_clearscroll);
    {
	/* Backward compatibility */
	int no_remote_qtitle = gppi_raw_forced(sesskey, "NoRemoteQTitle", 1);
	/* We deliberately interpret the old setting of "no response" as
	 * "empty string". This changes the behaviour, but hopefully for
	 * the better; the user can always recover the old behaviour. */
	gppi_forced(sesskey, "RemoteQTitleAction",
	     no_remote_qtitle ? TITLE_EMPTY : TITLE_REAL,
	     conf, CONF_remote_qtitle_action);
    }
    gppb_forced(sesskey, "NoDBackspace", false, conf, CONF_no_dbackspace);
    gppb_forced(sesskey, "NoRemoteCharset", false, conf, CONF_no_remote_charset);
    gppb_forced(sesskey, "ApplicationCursorKeys", false, conf, CONF_app_cursor);
    gppb_forced(sesskey, "ApplicationKeypad", false, conf, CONF_app_keypad);
    gppb_forced(sesskey, "NetHackKeypad", false, conf, CONF_nethack_keypad);
    gppb_forced(sesskey, "AltF4", true, conf, CONF_alt_f4);
    gppb_forced(sesskey, "AltSpace", false, conf, CONF_alt_space);
    gppb_forced(sesskey, "AltOnly", false, conf, CONF_alt_only);
    gppb_forced(sesskey, "ComposeKey", false, conf, CONF_compose_key);
    gppb_forced(sesskey, "CtrlAltKeys", true, conf, CONF_ctrlaltkeys);
#ifdef OSX_META_KEY_CONFIG
    gppb_forced(sesskey, "OSXOptionMeta", true, conf, CONF_osx_option_meta);
    gppb_forced(sesskey, "OSXCommandMeta", false, conf, CONF_osx_command_meta);
#endif
    gppb_forced(sesskey, "TelnetKey", false, conf, CONF_telnet_keyboard);
    gppb_forced(sesskey, "TelnetRet", true, conf, CONF_telnet_newline);
    gppi_forced(sesskey, "LocalEcho", AUTO, conf, CONF_localecho);
    gppi_forced(sesskey, "LocalEdit", AUTO, conf, CONF_localedit);
#if (defined MOD_PERSO) && (!defined FLJ)
    gpps_forced(sesskey, "Answerback", "KiTTY", conf, CONF_answerback);
#else
    gpps_forced(sesskey, "Answerback", "PuTTY", conf, CONF_answerback);
#endif
    gppb_forced(sesskey, "AlwaysOnTop", false, conf, CONF_alwaysontop);
    gppb_forced(sesskey, "FullScreenOnAltEnter", false,
         conf, CONF_fullscreenonaltenter);
    gppb_forced(sesskey, "HideMousePtr", false, conf, CONF_hide_mouseptr);
    gppb_forced(sesskey, "SunkenEdge", false, conf, CONF_sunken_edge);
    gppi_forced(sesskey, "WindowBorder", 1, conf, CONF_window_border);
#ifdef MOD_FAR2L
    gppi_forced(sesskey, "CurType", 1, conf, CONF_cursor_type);
    gppb_forced(sesskey, "BlinkCur", true, conf, CONF_blink_cur);
#else
    gppi_forced(sesskey, "CurType", 0, conf, CONF_cursor_type);
    gppb_forced(sesskey, "BlinkCur", false, conf, CONF_blink_cur);
#endif
    /* pedantic compiler tells me I can't use conf, CONF_beep as an int * :-) */
    gppi_forced(sesskey, "Beep", 1, conf, CONF_beep);
    gppi_forced(sesskey, "BeepInd", 0, conf, CONF_beep_ind);
    gppfile_forced(sesskey, "BellWaveFile", conf, CONF_bell_wavefile);
    gppb_forced(sesskey, "BellOverload", true, conf, CONF_bellovl);
    gppi_forced(sesskey, "BellOverloadN", 5, conf, CONF_bellovl_n);
    i = gppi_raw_forced(sesskey, "BellOverloadT", 2*TICKSPERSEC
#ifdef PUTTY_UNIX_H
				   *1000
#endif
				   );
    conf_set_int(conf, CONF_bellovl_t, i
#ifdef PUTTY_UNIX_H
		 / 1000
#endif
		 );
    i = gppi_raw_forced(sesskey, "BellOverloadS", 5*TICKSPERSEC
#ifdef PUTTY_UNIX_H
				   *1000
#endif
				   );
    conf_set_int(conf, CONF_bellovl_s, i
#ifdef PUTTY_UNIX_H
		 / 1000
#endif
		 );
    gppi_forced(sesskey, "ScrollbackLines", 2000, conf, CONF_savelines);
    gppb_forced(sesskey, "DECOriginMode", false, conf, CONF_dec_om);
    gppb_forced(sesskey, "AutoWrapMode", true, conf, CONF_wrap_mode);
    gppb_forced(sesskey, "LFImpliesCR", false, conf, CONF_lfhascr);
    gppb_forced(sesskey, "CRImpliesLF", false, conf, CONF_crhaslf);
    gppb_forced(sesskey, "DisableArabicShaping", false, conf, CONF_no_arabicshaping);
    gppb_forced(sesskey, "DisableBidi", false, conf, CONF_no_bidi);
    gppb_forced(sesskey, "WinNameAlways", true, conf, CONF_win_name_always);
    gpps_forced(sesskey, "WinTitle", "", conf, CONF_wintitle);
    gppi_forced(sesskey, "TermWidth", 80, conf, CONF_width);
    gppi_forced(sesskey, "TermHeight", 24, conf, CONF_height);
    gppfont_forced(sesskey, "Font", conf, CONF_font);
#ifdef MOD_PERSO
    /*
     * HACK: PuTTY-url
     * Set font quality to cleartype on Windows Vista and higher
     */
    if (versioninfo.dwMajorVersion >= 6) {
        gppi_forced(sesskey, "FontQuality", FQ_CLEARTYPE, conf, CONF_font_quality);
    } else {
        gppi_forced(sesskey, "FontQuality", FQ_DEFAULT, conf, CONF_font_quality);
    }
#else
    gppi_forced(sesskey, "FontQuality", FQ_DEFAULT, conf, CONF_font_quality);
#endif
    gppi_forced(sesskey, "FontVTMode", VT_UNICODE, conf, CONF_vtmode);
    gppb_forced(sesskey, "UseSystemColours", false, conf, CONF_system_colour);
    gppb_forced(sesskey, "TryPalette", false, conf, CONF_try_palette);
    gppb_forced(sesskey, "ANSIColour", true, conf, CONF_ansi_colour);
    gppb_forced(sesskey, "Xterm256Colour", true, conf, CONF_xterm_256_colour);
    gppb_forced(sesskey, "TrueColour", true, conf, CONF_true_colour);
    i = gppi_raw_forced(sesskey, "BoldAsColour", 1); conf_set_int(conf, CONF_bold_style, i+1);

#ifdef MOD_TUTTYCOLOR
    gppi_forced(sesskey, "BoldAsColourTest", 1, conf, CONF_bold_colour);
    gppi_forced(sesskey, "UnderlinedAsColour", 0, conf, CONF_under_colour);
    gppi_forced(sesskey, "SelectedAsColour", 0, conf, CONF_sel_colour);
    for (i = 0; i < 34; i++) {
	static const char *const defaults[34] = {
	    "187,187,187", "255,255,255", "0,0,0", "85,85,85", "0,0,0",
	    "0,255,0", "0,0,0", "85,85,85", "187,0,0", "255,85,85",
	    "0,187,0", "85,255,85", "187,187,0", "255,255,85", "0,0,187",
	    "85,85,255", "187,0,187", "255,85,255", "0,187,187",
	    "85,255,255", "187,187,187", "255,255,255", "187,187,187",
	    "0,0,0", "0,0,0", "187,0,0", "0,187,0", "187,187,0", "0,0,187",
	    "187,0,187", "0,187,187", "187,187,187", "0,0,0", "187,187,187"
	};
#else
    for (i = 0; i < 22; i++) {
	static const char *const defaults[] = {
	    "187,187,187", "255,255,255", "0,0,0", "85,85,85", "0,0,0",
	    "0,255,0", "0,0,0", "85,85,85", "187,0,0", "255,85,85",
	    "0,187,0", "85,255,85", "187,187,0", "255,255,85", "0,0,187",
	    "85,85,255", "187,0,187", "255,85,255", "0,187,187",
	    "85,255,255", "187,187,187", "255,255,255"
	};
#endif
	char buf[20], *buf2;
	int c0, c1, c2;
	sprintf(buf, "Colour%d", i);
	buf2 = gpps_raw_forced(sesskey, buf, defaults[i]);
	if (sscanf(buf2, "%d,%d,%d", &c0, &c1, &c2) == 3) {
	    conf_set_int_int(conf, CONF_colours, i*3+0, c0);
	    conf_set_int_int(conf, CONF_colours, i*3+1, c1);
	    conf_set_int_int(conf, CONF_colours, i*3+2, c2);
	}
	sfree(buf2);
    }
    gppb_forced(sesskey, "RawCNP", false, conf, CONF_rawcnp);
    gppb_forced(sesskey, "UTF8linedraw", false, conf, CONF_utf8linedraw);
    gppb_forced(sesskey, "PasteRTF", false, conf, CONF_rtf_paste);
    gppi_forced(sesskey, "MouseIsXterm", 0, conf, CONF_mouse_is_xterm);
    gppb_forced(sesskey, "RectSelect", false, conf, CONF_rect_select);
    gppb_forced(sesskey, "PasteControls", false, conf, CONF_paste_controls);
    gppb_forced(sesskey, "MouseOverride", true, conf, CONF_mouse_override);
    for (i = 0; i < 256; i += 32) {
	static const char *const defaults[] = {
	    "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0",
	    "0,1,2,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1,1",
	    "1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,1,1,2",
	    "1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1",
	    "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1",
	    "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1",
	    "2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,2,2,2,2,2,2,2,2",
	    "2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1,2,2,2,2,2,2,2,2"
	};
	char buf[20], *buf2, *p;
	int j;
	sprintf(buf, "Wordness%d", i);
	buf2 = gpps_raw_forced(sesskey, buf, defaults[i / 32]);
	p = buf2;
	for (j = i; j < i + 32; j++) {
	    char *q = p;
	    while (*p && *p != ',')
		p++;
	    if (*p == ',')
		*p++ = '\0';
	    conf_set_int_int(conf, CONF_wordness, j, atoi(q));
	}
	sfree(buf2);
    }
    gppb_forced(sesskey, "MouseAutocopy", CLIPUI_DEFAULT_AUTOCOPY,
         conf, CONF_mouseautocopy);
    read_clip_setting_forced(sesskey, "MousePaste", CLIPUI_DEFAULT_MOUSE,
                      conf, CONF_mousepaste, CONF_mousepaste_custom);
    read_clip_setting_forced(sesskey, "CtrlShiftIns", CLIPUI_DEFAULT_INS,
                      conf, CONF_ctrlshiftins, CONF_ctrlshiftins_custom);
    read_clip_setting_forced(sesskey, "CtrlShiftCV", CLIPUI_NONE,
                      conf, CONF_ctrlshiftcv, CONF_ctrlshiftcv_custom);
    /*
     * The empty default for LineCodePage will be converted later
     * into a plausible default for the locale.
     */
    gpps_forced(sesskey, "LineCodePage", "", conf, CONF_line_codepage);
    gppb_forced(sesskey, "CJKAmbigWide", false, conf, CONF_cjk_ambig_wide);
    gppb_forced(sesskey, "UTF8Override", true, conf, CONF_utf8_override);
    gpps_forced(sesskey, "Printer", "", conf, CONF_printer);
#ifdef MOD_PRINTCLIP
    if( !strcmp( conf_get_str(conf,CONF_printer),PRINT_TO_CLIPBOARD_STRING) ) { conf_set_int(conf,CONF_printclip,1) ; }
    else { conf_set_int(conf,CONF_printclip,0) ; }
#endif
    gppb_forced(sesskey, "CapsLockCyr", false, conf, CONF_xlat_capslockcyr);
    gppb_forced(sesskey, "ScrollBar", true, conf, CONF_scrollbar);
    gppb_forced(sesskey, "ScrollBarFullScreen", false,
         conf, CONF_scrollbar_in_fullscreen);
    gppb_forced(sesskey, "ScrollOnKey", false, conf, CONF_scroll_on_key);
    gppb_forced(sesskey, "ScrollOnDisp", true, conf, CONF_scroll_on_disp);
    gppb_forced(sesskey, "EraseToScrollback", true, conf, CONF_erase_to_scrollback);
    gppi_forced(sesskey, "LockSize", 0, conf, CONF_resize_action);
    gppb_forced(sesskey, "BCE", true, conf, CONF_bce);
    gppb_forced(sesskey, "BlinkText", false, conf, CONF_blinktext);
    gppb_forced(sesskey, "X11Forward", false, conf, CONF_x11_forward);
    gpps_forced(sesskey, "X11Display", "", conf, CONF_x11_display);
    gppi_forced(sesskey, "X11AuthType", X11_MIT, conf, CONF_x11_auth);
    gppfile_forced(sesskey, "X11AuthFile", conf, CONF_xauthfile);

    gppb_forced(sesskey, "LocalPortAcceptAll", false, conf, CONF_lport_acceptall);
    gppb_forced(sesskey, "RemotePortAcceptAll", false, conf, CONF_rport_acceptall);
    gppmap_forced(sesskey, "PortForwardings", conf, CONF_portfwd);
    i = gppi_raw_forced(sesskey, "BugIgnore1", 0); conf_set_int(conf, CONF_sshbug_ignore1, 2-i);
    i = gppi_raw_forced(sesskey, "BugPlainPW1", 0); conf_set_int(conf, CONF_sshbug_plainpw1, 2-i);
    i = gppi_raw_forced(sesskey, "BugRSA1", 0); conf_set_int(conf, CONF_sshbug_rsa1, 2-i);
    i = gppi_raw_forced(sesskey, "BugIgnore2", 0); conf_set_int(conf, CONF_sshbug_ignore2, 2-i);
    {
	int i;
	i = gppi_raw_forced(sesskey, "BugHMAC2", 0); conf_set_int(conf, CONF_sshbug_hmac2, 2-i);
	if (2-i == AUTO) {
	    i = gppi_raw_forced(sesskey, "BuggyMAC", 0);
	    if (i == 1)
		conf_set_int(conf, CONF_sshbug_hmac2, FORCE_ON);
	}
    }
    i = gppi_raw_forced(sesskey, "BugDeriveKey2", 0); conf_set_int(conf, CONF_sshbug_derivekey2, 2-i);
    i = gppi_raw_forced(sesskey, "BugRSAPad2", 0); conf_set_int(conf, CONF_sshbug_rsapad2, 2-i);
    i = gppi_raw_forced(sesskey, "BugPKSessID2", 0); conf_set_int(conf, CONF_sshbug_pksessid2, 2-i);
    i = gppi_raw_forced(sesskey, "BugRekey2", 0); conf_set_int(conf, CONF_sshbug_rekey2, 2-i);
    i = gppi_raw_forced(sesskey, "BugMaxPkt2", 0); conf_set_int(conf, CONF_sshbug_maxpkt2, 2-i);
    i = gppi_raw_forced(sesskey, "BugOldGex2", 0); conf_set_int(conf, CONF_sshbug_oldgex2, 2-i);
    i = gppi_raw_forced(sesskey, "BugWinadj", 0); conf_set_int(conf, CONF_sshbug_winadj, 2-i);
    i = gppi_raw_forced(sesskey, "BugChanReq", 0); conf_set_int(conf, CONF_sshbug_chanreq, 2-i);
    conf_set_bool(conf, CONF_ssh_simple, false);
    gppb_forced(sesskey, "StampUtmp", true, conf, CONF_stamp_utmp);
    gppb_forced(sesskey, "LoginShell", true, conf, CONF_login_shell);
    gppb_forced(sesskey, "ScrollbarOnLeft", false, conf, CONF_scrollbar_on_left);
    gppb_forced(sesskey, "ShadowBold", false, conf, CONF_shadowbold);
    gppfont_forced(sesskey, "BoldFont", conf, CONF_boldfont);
    gppfont_forced(sesskey, "WideFont", conf, CONF_widefont);
    gppfont_forced(sesskey, "WideBoldFont", conf, CONF_wideboldfont);
    gppi_forced(sesskey, "ShadowBoldOffset", 1, conf, CONF_shadowboldoffset);
    gpps_forced(sesskey, "SerialLine", "", conf, CONF_serline);
    gppi_forced(sesskey, "SerialSpeed", 9600, conf, CONF_serspeed);
    gppi_forced(sesskey, "SerialDataBits", 8, conf, CONF_serdatabits);
    gppi_forced(sesskey, "SerialStopHalfbits", 2, conf, CONF_serstopbits);
    gppi_forced(sesskey, "SerialParity", SER_PAR_NONE, conf, CONF_serparity);
    gppi_forced(sesskey, "SerialFlowControl", SER_FLOW_XONXOFF, conf, CONF_serflow);
    gpps_forced(sesskey, "WindowClass", "", conf, CONF_winclass);
    gppb_forced(sesskey, "ConnectionSharing", false,
         conf, CONF_ssh_connection_sharing);
    gppb_forced(sesskey, "ConnectionSharingUpstream", true,
         conf, CONF_ssh_connection_sharing_upstream);
    gppb_forced(sesskey, "ConnectionSharingDownstream", true,
         conf, CONF_ssh_connection_sharing_downstream);
    gppmap_forced(sesskey, "SSHManualHostKeys", conf, CONF_ssh_manual_hostkeys);
    
    /*
     * PuTTY 0.75 SUPDUP settings
    gpps_forced(sesskey, "SUPDUPLocation", "The Internet", conf, CONF_supdup_location);
    gppi_forced(sesskey, "SUPDUPCharset", false, conf, CONF_supdup_ascii_set);
    gppb_forced(sesskey, "SUPDUPMoreProcessing", false, conf, CONF_supdup_more);
    gppb_forced(sesskey, "SUPDUPScrolling", false, conf, CONF_supdup_scroll);
     */

/* rutty: scripting is compiled and exposed in current KiTTY builds, so KTX
 * imports must restore it without depending on the historical MOD_RUTTY
 * define. Current 0.84 UI stores the selected script path in CONF_scriptfile
 * (old trees used ScriptFileName/CONF_script_filename). */
	gppfile_forced(sesskey, "Scriptfile", conf, CONF_scriptfile);
	if (filename_to_str(conf_get_filename(conf, CONF_scriptfile))[0] == '\0') {
		Filename *legacy_scriptfile = read_setting_filename_forced(sesskey, "ScriptFileName");
		if (legacy_scriptfile) {
			conf_set_filename(conf, CONF_scriptfile, legacy_scriptfile);
			filename_free(legacy_scriptfile);
		}
	}
	gppi_forced(sesskey, "ScriptMode", 0, conf, CONF_script_mode);
	gppi_forced(sesskey, "ScriptLineDelay", 5, conf, CONF_script_line_delay);
	gppi_forced(sesskey, "ScriptCharDelay", 0, conf, CONF_script_char_delay);
	gpps_forced(sesskey, "ScriptCondLine", ":", conf, CONF_script_cond_line);
	gppi_forced(sesskey, "ScriptCondUse", 0, conf, CONF_script_cond_use);
	gppi_forced(sesskey, "ScriptCRLF", 0, conf, CONF_script_crlf);
	gppi_forced(sesskey, "ScriptEnable", 0, conf, CONF_script_enable);
	gppi_forced(sesskey, "ScriptExcept", 0, conf, CONF_script_except);
	gppi_forced(sesskey, "ScriptTimeout", 15, conf, CONF_script_timeout);
	gpps_forced(sesskey, "ScriptWait", "", conf, CONF_script_waitfor);
	gpps_forced(sesskey, "ScriptHalt", "", conf, CONF_script_halton);
#ifdef MOD_RECONNECT
    gppi_forced(sesskey, "WakeupReconnect", 0, conf, CONF_wakeup_reconnect );
    gppi_forced(sesskey, "FailureReconnect", 0, conf, CONF_failure_reconnect );
#endif
#if (defined MOD_BACKGROUNDIMAGE) && (!defined FLJ)
    gppi_forced(sesskey, "BgOpacity", 50, conf, CONF_bg_opacity );
    gppi_forced(sesskey, "BgSlideshow", 0, conf, CONF_bg_slideshow );
    gppi_forced(sesskey, "BgType", 0, conf, CONF_bg_type );
    gppfile_forced(sesskey, "BgImageFile", conf, CONF_bg_image_filename );
    gppi_forced(sesskey, "BgImageStyle", 0, conf, CONF_bg_image_style );
    gppi_forced(sesskey, "BgImageAbsoluteX", 0, conf, CONF_bg_image_abs_x );
    gppi_forced(sesskey, "BgImageAbsoluteY", 0, conf, CONF_bg_image_abs_y );
    gppi_forced(sesskey, "BgImagePlacement", 0, conf, CONF_bg_image_abs_fixed );
#endif
	/*
	 * HACK: PuttyTray / Nutty
	 * Hyperlink stuff: Load hyperlink settings. In 0.84 the feature is active via
	 * kitty_url.c/window.c without the historical terminal.c hyperlink define,
	 * so KTX import must not depend on that guard.
	 */
	gppi_forced(sesskey, "HyperlinkUnderline", 1, conf, CONF_url_underline);
	gppi_forced(sesskey, "HyperlinkUseCtrlClick", 1, conf, CONF_url_ctrl_click);
	gppi_forced(sesskey, "HyperlinkBrowserUseDefault", 1, conf, CONF_url_defbrowser);
	gppfile_forced(sesskey, "HyperlinkBrowser", conf, CONF_url_browser);
	gppi_forced(sesskey, "HyperlinkRegularExpressionUseDefault", 1, conf, CONF_url_defregex);
	gpps_forced(sesskey, "HyperlinkRegularExpression", "", conf, CONF_url_regex);
#ifdef MOD_ZMODEM
    gppfile_forced(sesskey, "rzCommand", conf, CONF_rzcommand );
    gpps_forced(sesskey, "rzOptions", "-e -v", conf, CONF_rzoptions );
    gppfile_forced(sesskey, "szCommand", conf, CONF_szcommand );
    gpps_forced(sesskey, "szOptions", "-e -v", conf, CONF_szoptions );
    gpps_forced(sesskey, "zDownloadDir", "C:\\", conf, CONF_zdownloaddir );
#endif
#ifdef MOD_PERSO
    gpps_forced(sesskey, "HostAlt", "", conf, CONF_host_alt );
    gppi_forced(sesskey, "TransparencyValue", 0, conf, CONF_transparencynumber ) ;
    if( conf_get_int( conf, CONF_transparencynumber) < -1 ) conf_set_int( conf,CONF_transparencynumber,-1) ;
    if( conf_get_int( conf, CONF_transparencynumber) > 255 ) conf_set_int( conf,CONF_transparencynumber,255) ;
    gppi_forced(sesskey, "SendToTray", 0, conf, CONF_sendtotray );
    gppi_forced(sesskey, "Maximize", 0, conf, CONF_maximize );
    gppi_forced(sesskey, "Fullscreen", 0, conf, CONF_fullscreen );
    gppb_forced(sesskey, "SaveOnExit", false, conf, CONF_saveonexit );
    gppi_forced(sesskey, "Icone", 1, conf, CONF_icone );
    gppfile_forced(sesskey, "IconeFile", conf, CONF_iconefile );
    gppi_forced(sesskey, "WinSCPProtocol", 1, conf, CONF_winscpprot );
    gpps_forced(sesskey, "SFTPConnect", "", conf, CONF_sftpconnect );
    gpps_forced(sesskey, "PSCPOptions", "-r", conf, CONF_pscpoptions );
    gpps_forced(sesskey, "PSCPShell", "", conf, CONF_pscpshell );
    gpps_forced(sesskey, "PSCPRemoteDir", "", conf, CONF_pscpremotedir );
    gpps_forced(sesskey, "WinSCPOptions", "", conf, CONF_winscpoptions );
    gpps_forced(sesskey, "WinSCPRawSettings", "", conf, CONF_winscprawsettings );
    gppfile_forced(sesskey, "Scriptfile", conf, CONF_scriptfile );
    Filename * fn = filename_from_str( "" ) ;
    conf_set_filename(conf,CONF_scriptfile,fn);
    filename_free(fn);
    gpps_forced(sesskey, "ScriptfileContent", "", conf, CONF_scriptfilecontent );
    gpps_forced(sesskey, "AntiIdle", "", conf, CONF_antiidle );
    gpps_forced(sesskey, "LogTimestamp", "", conf, CONF_logtimestamp );
    gpps_forced(sesskey, "Autocommand", "", conf, CONF_autocommand );
    gpps_forced(sesskey, "AutocommandOut", "", conf, CONF_autocommandout );
    gpps_forced(sesskey, "Folder", "", conf, CONF_folder );
    if( strlen(conf_get_str(conf, CONF_folder)) == 0 ) { conf_set_str( conf, CONF_folder, "Default" ) ; }
    gppi_forced(sesskey, "LogTimeRotation", 0, conf, CONF_logtimerotation );
    gppi_forced(sesskey, "TermXPos", -1, conf, CONF_xpos );
    gppi_forced(sesskey, "TermYPos", -1, conf, CONF_ypos );
    gppi_forced(sesskey, "WindowState", 0, conf, CONF_windowstate );
    gppb_forced(sesskey, "SaveWindowPos", false, conf, CONF_save_windowpos ); /* BKG */
    gppb_forced(sesskey, "ForegroundOnBell", false, conf, CONF_foreground_on_bell );
#ifndef MOD_NOPASSWORD
    gpps_forced(sesskey, "Password", "", conf, CONF_password ) ;
    if( strlen(conf_get_str(conf, CONF_password))>0 ) {
	char pst[4096] ;
	if( strlen(conf_get_str(conf, CONF_password))<=4095 ) { strcpy( pst, conf_get_str(conf, CONF_password) ) ; }
	else { memcpy( pst, conf_get_str( conf, CONF_password ), 4095 ) ; pst[4095] = '\0' ; }
	decryptpassword( GetCryptSaltFlag(), pst, conf_get_str(conf, CONF_host), conf_get_str(conf, CONF_termtype) ) ;
	/* (original called DebugGetPassword here: a debug-only dump to a
	 * "kitty.password" file, no effect on conf; dropped in the port.) */
	MASKPASS(GetCryptSaltFlag(),pst);
	conf_set_str( conf, CONF_password, pst ) ;
	memset(pst,0,strlen(pst));
    }
#else
	conf_set_str( conf, CONF_password, "" ) ;
#endif
    gppi_forced(sesskey, "CtrlTabSwitch", 0, conf, CONF_ctrl_tab_switch);
    gpps_forced(sesskey, "Comment", "", conf, CONF_comment );
    gppb_forced(sesskey, "SCPAutoPwd", false, conf, CONF_scp_auto_pwd); /* 0.84: BOOL */
    gppb_forced(sesskey, "NoFocusReporting", true, conf, CONF_no_focus_rep);
    gppi_forced(sesskey, "LinesAtAScroll", 5, conf, CONF_scrolllines);
    gppb_forced(sesskey, "SSHTunnelInTitle", false, conf, CONF_ssh_tunnel_print_in_title);
    gppb_forced(sesskey, "OSC52WarnBeforeClipboardSync", false, conf, CONF_osc52_warn_before_cliboard_sync);
#endif
#ifdef MOD_PORTKNOCKING
	gpps_forced(sesskey, "PortKnocking", "", conf, CONF_portknockingoptions );
#endif
#ifdef MOD_DISABLEALTGR
	gppi_forced(sesskey, "DisableAltGr", 0, conf, CONF_disablealtgr);
#endif
#ifdef MOD_PROXY
	gpps_forced(sesskey, "ProxySelection", "- Session defined proxy -", conf, CONF_proxyselection);
#endif
// END COPY/PASTE
	conf_set_str( conf, CONF_folder, "Default") ;
	fclose(sesskey) ;
		
	conf_free( confDef ) ;
}

/* ---- read-side helpers (kitty_settings.c:1317-1552) ---- */

/* #541: the original trailing-strip loops indexed buffer[strlen(buffer)-1]
   without a length guard. On a blank or CRLF-only .ini line fgets returns "\n",
   the first strip empties the buffer, and the next strlen()-1 wraps (size_t 0-1
   = SIZE_MAX) -> wild out-of-bounds read/write. These helpers strip safely. */
static void rstrip_eol_forced( char *s ) {
	size_t l = strlen( s ) ;
	while( l > 0 && ( s[l-1]=='\n' || s[l-1]=='\r' ) ) { s[--l] = '\0' ; }
}
static void rstrip_cont_forced( char *s ) {
	size_t l = strlen( s ) ;
	while( l > 0 && ( s[l-1]=='\\' || s[l-1]=='\n' || s[l-1]=='\r' ) ) { s[--l] = '\0' ; }
}

int read_setting_i_forced(void *handle, const char *key, int defvalue) {
	int n = defvalue ;
	char buffer[2048], name[256] ;
	rewind(handle);
	sprintf( name, "%s\\", key ) ;
	while( fgets(buffer,2047,handle)!=NULL ) {
		rstrip_eol_forced( buffer ) ;
		if( strlen(buffer)==0 || buffer[strlen(buffer)-1] != '\\' ) { decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD) ; }
		if( strstr( buffer, name ) == buffer ) {
			rstrip_cont_forced( buffer ) ;
			n = atoi( buffer+strlen(name) ) ;
			break ;
		}
	}
	return n ;
}

char *read_setting_s_forced(void *handle, const char *key) {
	char * loadResult = NULL ;
	char buffer[2048], name[256] ;
	rewind(handle);
	sprintf( name, "%s\\", key ) ;
	
	while( fgets(buffer,2047,handle)!=NULL ) {
		rstrip_eol_forced( buffer ) ;
		if( strlen(buffer)==0 || buffer[strlen(buffer)-1] != '\\' ) { decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD) ; }
		if( strstr( buffer, name ) == buffer ) {
			rstrip_cont_forced( buffer ) ;
			loadResult = (char*) malloc( strlen( buffer+strlen(name) ) + 1 ) ;
			unmungestr( buffer+strlen(name), loadResult, strlen( buffer+strlen(name) ) + 1 ) ;
			break ;
		}
	}
	return loadResult ;
}

Filename *read_setting_filename_forced(void *handle, const char *key) {
	Filename * Result = NULL ;
	char buffer[2048], name[256] ;
	rewind(handle);
	sprintf( name, "%s\\", key ) ;
	while( fgets(buffer,2047,handle)!=NULL ) {
		rstrip_eol_forced( buffer ) ;
		if( strlen(buffer)==0 || buffer[strlen(buffer)-1] != '\\' ) { decryptstring( GetCryptSaltFlag(), buffer, MASTER_PASSWORD) ; }
		if( strstr( buffer, name ) == buffer ) {
			rstrip_cont_forced( buffer ) ;
			unmungestr( buffer+strlen(name), buffer, 2047 ) ;
			Result = filename_from_str( buffer ) ;
			break ;
		}
	}
	return Result ;
}

#include <limits.h>
FontSpec *read_setting_fontspec_forced(void *handle, const char *name)
{
    char *settingname;
    char *fontname;
    FontSpec *ret;
    int isbold, height, charset;

    fontname = read_setting_s_forced(handle, name);
    if (!fontname)
	return NULL;

    settingname = dupcat(name, "IsBold", NULL);
    isbold = read_setting_i_forced(handle, settingname, -1);
    sfree(settingname);
    if (isbold == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "CharSet", NULL);
    charset = read_setting_i_forced(handle, settingname, -1);
    sfree(settingname);
    if (charset == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "Height", NULL);
    height = read_setting_i_forced(handle, settingname, INT_MIN);
    sfree(settingname);
    if (height == INT_MIN) {
        sfree(fontname);
        return NULL;
    }

    ret = fontspec_new(fontname, isbold, height, charset);
    sfree(fontname);
    return ret;
}


static bool gppb_raw_forced(void *sesskey, const char *name, bool def) {
    def = platform_default_b(name, def);
    return sesskey ? read_setting_i_forced(sesskey, name, def) != 0 : def;
}

static void gppb_forced(void *sesskey, const char *name, bool def, Conf *conf, int primary) {
    conf_set_bool(conf, primary, gppb_raw_forced(sesskey, name, def));
}

static void gppi_forced(void *handle, const char *name, int def, Conf *conf, int primary) {
    conf_set_int(conf, primary, gppi_raw_forced(handle, name, def));
}

static int gppi_raw_forced(void *handle, const char *name, int def) {
    def = platform_default_i(name, def);
    return read_setting_i_forced(handle, name, def);
}

static void gppfile_forced(void *handle, const char *name, Conf *conf, int primary) {
    Filename *result = read_setting_filename_forced(handle, name);
    if (!result)
	result = platform_default_filename(name);
    conf_set_filename(conf, primary, result);
    filename_free(result);
}

static void gpps_forced(void *handle, const char *name, const char *def, Conf *conf, int primary) {
    char *val = gpps_raw_forced(handle, name, def);
    conf_set_str(conf, primary, val);
    sfree(val);
}

static char *gpps_raw_forced(void *handle, const char *name, const char *def) {
    char *ret = read_setting_s_forced(handle, name);
    if (!ret)
	ret = platform_default_s(name);
    if (!ret)
	ret = def ? dupstr(def) : NULL;   /* permit NULL as final fallback */
    return ret;
}

static void gppfont_forced(void *handle, const char *name, Conf *conf, int primary) {
    FontSpec *result = read_setting_fontspec_forced(handle, name);
    if (!result)
        result = platform_default_fontspec(name);
    conf_set_fontspec(conf, primary, result);
    fontspec_free(result);
}

static int gppmap_forced(void *handle, const char *name, Conf *conf, int primary) {
    char *buf, *p, *q, *key, *val;

    /*
     * Start by clearing any existing subkeys of this key from conf.
     */
    while ((key = conf_get_str_nthstrkey(conf, primary, 0)) != NULL)
        conf_del_str_str(conf, primary, key);

    /*
     * Now read a serialised list from the settings and unmarshal it
     * into its components.
     */
    buf = gpps_raw_forced(handle, name, NULL);
    if (!buf)
	return FALSE;

    p = buf;
    while (*p) {
	q = buf;
	val = NULL;
	while (*p && *p != ',') {
	    int c = *p++;
	    if (c == '=')
		c = '\0';
	    if (c == '\\')
		c = *p++;
	    *q++ = c;
	    if (!c)
		val = q;
	}
	if (*p == ',')
	    p++;
	if (!val)
	    val = q;
	*q = '\0';

        if (primary == CONF_portfwd && strchr(buf, 'D') != NULL) {
            /*
             * Backwards-compatibility hack: dynamic forwardings are
             * indexed in the data store as a third type letter in the
             * key, 'D' alongside 'L' and 'R' - but really, they
             * should be filed under 'L' with a special _value_,
             * because local and dynamic forwardings both involve
             * _listening_ on a local port, and are hence mutually
             * exclusive on the same port number. So here we translate
             * the legacy storage format into the sensible internal
             * form, by finding the D and turning it into a L.
             */
            char *newkey = dupstr(buf);
            *strchr(newkey, 'D') = 'L';
            conf_set_str_str(conf, primary, newkey, "D");
            sfree(newkey);
        } else {
            conf_set_str_str(conf, primary, buf, val);
        }
    }
    sfree(buf);

    return TRUE;
}

static void gprefs_forced(void *sesskey, const char *name, const char *def, const struct keyvalwhere *mapping, int nvals, Conf *conf, int primary) {
    /*
     * Fetch the string which we'll parse as a comma-separated list.
     */
    char *value = gpps_raw_forced(sesskey, name, def);
    gprefs_from_str(value, mapping, nvals, conf, primary);
    sfree(value);
}

static void read_clip_setting_forced(void *sesskey, char *savekey, int def, Conf *conf, int confkey, int strconfkey) {
    char *setting = read_setting_s_forced(sesskey, savekey);
    int val;

    conf_set_str(conf, strconfkey, "");
    if (!setting) {
        val = def;
    } else if (!strcmp(setting, "implicit")) {
        val = CLIPUI_IMPLICIT;
    } else if (!strcmp(setting, "explicit")) {
        val = CLIPUI_EXPLICIT;
    } else if (!strncmp(setting, "custom:", 7)) {
        val = CLIPUI_CUSTOM;
        conf_set_str(conf, strconfkey, setting + 7);
    } else {
        val = CLIPUI_NONE;
    }
    conf_set_int(conf, confkey, val);
    sfree(setting);
}
