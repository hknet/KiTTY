/*
 * kitty_settings_forced.c — KiTTY "export current settings" to a .ktx file.
 *
 * Ported from KiTTY 0.76b kitty_settings.c (save_open_settings_forced + the
 * write_setting_*_forced / wmap_forced / wprefs_forced / write_clip_setting_forced
 * helpers) onto PuTTY 0.84. Replaces the no-op stub in kitty_bridge.c.
 *
 * 0.84 adaptations:
 *  - Filename has no ->path; use filename_to_str(fn).
 *  - cryptstring is 3-arg (mode, st, key) here; bridged through GetCryptSaltFlag().
 *  - A handful of MOD_PERSO conf keys are not yet ported into 0.84 conf.h;
 *    those individual writes are commented out (marked NOTPORTED) so the rest
 *    of the export remains faithful. Compiled with MOD_PERSO only, so the
 *    MOD_RUTTY/HYPERLINK/ZMODEM/etc. blocks compile out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "putty.h"
#include "kitty.h"
#include "kitty_commun.h"  /* GetCryptSaltFlag, MASKPASS */
#include "kitty_crypt.h"   /* cryptpassword */

/* CryptFileFlag lives in kitty_bridge.c; cryptstring/mungestr in kitty modules. */
extern int CryptFileFlag;
int cryptstring(const int mode, char *st, const char *key);

/* ---- forced writers (write to a plain FILE* in KiTTY .ktx line format) ---- */

void write_setting_i_forced(void *handle, const char *key, int value) {
    char buf[1024];
    sprintf(buf, "%s\\%i\\", key, value);
    if (CryptFileFlag) { cryptstring(GetCryptSaltFlag(), buf, MASTER_PASSWORD); }
    fprintf((FILE*)handle, "%s\n", buf);
    fflush(handle);
}

void write_setting_b_forced(void *handle, const char *key, bool value) {
    write_setting_i_forced(handle, key, value ? 1 : 0);
}

void write_setting_s_forced(void *handle, const char *key, const char *value) {
    char *p = (char*)malloc(3*strlen(value)+256);
    mungestr(value, p);
    char *buf = (char*)malloc(2*(strlen(key)+strlen(p))+10);
    sprintf(buf, "%s\\%s\\", key, p);
    if (CryptFileFlag) { cryptstring(GetCryptSaltFlag(), buf, MASTER_PASSWORD); }
    fprintf((FILE*)handle, "%s\n", buf);
    fflush(handle);
    free(buf);
    free(p);
}

void write_setting_filename_forced(void *handle, const char *key, Filename *value) {
    const char *path = filename_to_str(value);
    char *p = (char*)malloc(3*strlen(path)+256);
    mungestr(path, p);
    char *buf = (char*)malloc(2*(strlen(key)+strlen(p))+10);
    sprintf(buf, "%s\\%s\\", key, p);
    if (CryptFileFlag) { cryptstring(GetCryptSaltFlag(), buf, MASTER_PASSWORD); }
    fprintf((FILE*)handle, "%s\n", buf);
    fflush(handle);
    free(buf);
    free(p);
}

void write_setting_fontspec_forced(void *handle, const char *name, FontSpec *font) {
    char *settingname;
    write_setting_s_forced(handle, name, font->name);
    settingname = dupcat(name, "IsBold", NULL);
    write_setting_i_forced(handle, settingname, font->isbold);
    sfree(settingname);
    settingname = dupcat(name, "CharSet", NULL);
    write_setting_i_forced(handle, settingname, font->charset);
    sfree(settingname);
    settingname = dupcat(name, "Height", NULL);
    write_setting_i_forced(handle, settingname, font->height);
    sfree(settingname);
    fflush(handle);
}

static void wmap_forced(void *handle, char const *outkey, Conf *conf, int primary, int include_values) {
    char *buf, *p, *key, *realkey;
    const char *val, *q;
    int len;

    len = 1;
    for (val = conf_get_str_strs(conf, primary, NULL, &key);
         val != NULL;
         val = conf_get_str_strs(conf, primary, key, &key))
        len += 2 + 2 * (strlen(key) + strlen(val));

    buf = snewn(len, char);
    p = buf;

    for (val = conf_get_str_strs(conf, primary, NULL, &key);
         val != NULL;
         val = conf_get_str_strs(conf, primary, key, &key)) {

        if (primary == CONF_portfwd && !strcmp(val, "D")) {
            char *L;
            realkey = key;
            val = "";
            key = dupstr(key);
            L = strchr(key, 'L');
            if (L) *L = 'D';
        } else {
            realkey = NULL;
        }

        if (p != buf)
            *p++ = ',';
        for (q = key; *q; q++) {
            if (*q == '=' || *q == ',' || *q == '\\')
                *p++ = '\\';
            *p++ = *q;
        }
        if (include_values) {
            *p++ = '=';
            for (q = val; *q; q++) {
                if (*q == '=' || *q == ',' || *q == '\\')
                    *p++ = '\\';
                *p++ = *q;
            }
        }

        if (realkey) {
            free(key);
            key = realkey;
        }
    }
    *p = '\0';
    write_setting_s_forced(handle, outkey, buf);
    sfree(buf);
}

/* val2key + the cipher/kex/hostkey name tables are static in settings.c and
 * not exported; the original KiTTY kitty_settings.c saw them only because it
 * was #include'd into settings.c. As a standalone TU we keep local copies
 * (kept in sync with settings.c). */
static const char *val2key(const struct keyvalwhere *mapping, int nmaps, int val) {
    int i;
    for (i = 0; i < nmaps; i++)
        if (mapping[i].v == val) return mapping[i].s;
    return NULL;
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

static void wprefs_forced(void *sesskey, const char *name, const struct keyvalwhere *mapping, int nvals, Conf *conf, int primary) {
    char *buf, *p;
    int i, maxlen;

    for (maxlen = i = 0; i < nvals; i++) {
        const char *s = val2key(mapping, nvals, conf_get_int_int(conf, primary, i));
        if (s) maxlen += (maxlen > 0 ? 1 : 0) + strlen(s);
    }

    buf = snewn(maxlen + 1, char);
    p = buf;

    for (i = 0; i < nvals; i++) {
        const char *s = val2key(mapping, nvals, conf_get_int_int(conf, primary, i));
        if (s) p += sprintf(p, "%s%s", (p > buf ? "," : ""), s);
    }

    assert(p - buf == maxlen);
    *p = '\0';

    write_setting_s_forced(sesskey, name, buf);
    sfree(buf);
}

static void write_clip_setting_forced(void *sesskey, const char *savekey, Conf *conf, int confkey, int strconfkey) {
    int val = conf_get_int(conf, confkey);
    switch (val) {
      case CLIPUI_NONE:
      default:
        write_setting_s_forced(sesskey, savekey, "none");
        break;
      case CLIPUI_IMPLICIT:
        write_setting_s_forced(sesskey, savekey, "implicit");
        break;
      case CLIPUI_EXPLICIT:
        write_setting_s_forced(sesskey, savekey, "explicit");
        break;
      case CLIPUI_CUSTOM: {
            char *sval = dupcat("custom:", conf_get_str(conf, strconfkey), (const char *)NULL);
            write_setting_s_forced(sesskey, savekey, sval);
            sfree(sval);
        break;
      }
    }
}

/* ---- main entry: export current settings to a KiTTY .ktx file ---- */
void save_open_settings_forced(char *filename, Conf *conf) {
    FILE *sesskey;
    if ((sesskey = fopen(filename, "w")) == NULL) { return; }

    int i;
    const char *p;
    write_setting_i_forced(sesskey, "Present", 1);
    write_setting_s_forced(sesskey, "HostName", conf_get_str(conf, CONF_host));
    write_setting_filename_forced(sesskey, "LogFileName", conf_get_filename(conf, CONF_logfilename));
    write_setting_i_forced(sesskey, "LogType", conf_get_int(conf, CONF_logtype));
    write_setting_i_forced(sesskey, "LogFileClash", conf_get_int(conf, CONF_logxfovr));
    write_setting_b_forced(sesskey, "LogFlush", conf_get_bool(conf, CONF_logflush));
    write_setting_b_forced(sesskey, "LogHeader", conf_get_bool(conf, CONF_logheader));
    write_setting_b_forced(sesskey, "SSHLogOmitPasswords", conf_get_bool(conf, CONF_logomitpass));
    write_setting_b_forced(sesskey, "SSHLogOmitData", conf_get_bool(conf, CONF_logomitdata));
    p = "raw";
    {
        const struct BackendVtable *vt = backend_vt_from_proto(conf_get_int(conf, CONF_protocol));
        if (vt) p = vt->id;
    }
    write_setting_s_forced(sesskey, "Protocol", p);
    write_setting_i_forced(sesskey, "PortNumber", conf_get_int(conf, CONF_port));
    write_setting_i_forced(sesskey, "CloseOnExit", (conf_get_int(conf, CONF_close_on_exit)+2)%3);
    write_setting_b_forced(sesskey, "WarnOnClose", !!conf_get_bool(conf, CONF_warn_on_close));
    write_setting_i_forced(sesskey, "PingInterval", conf_get_int(conf, CONF_ping_interval) / 60);
    write_setting_i_forced(sesskey, "PingIntervalSecs", conf_get_int(conf, CONF_ping_interval) % 60);
    write_setting_b_forced(sesskey, "TCPNoDelay", conf_get_bool(conf, CONF_tcp_nodelay));
    write_setting_b_forced(sesskey, "TCPKeepalives", conf_get_bool(conf, CONF_tcp_keepalives));
    write_setting_s_forced(sesskey, "TerminalType", conf_get_str(conf, CONF_termtype));
    write_setting_s_forced(sesskey, "TerminalSpeed", conf_get_str(conf, CONF_termspeed));
    wmap_forced(sesskey, "TerminalModes", conf, CONF_ttymodes, true);

    write_setting_i_forced(sesskey, "AddressFamily", conf_get_int(conf, CONF_addressfamily));

    write_setting_s_forced(sesskey, "ProxyExcludeList", conf_get_str(conf, CONF_proxy_exclude_list));
    write_setting_i_forced(sesskey, "ProxyDNS", (conf_get_int(conf, CONF_proxy_dns)+2)%3);
    write_setting_b_forced(sesskey, "ProxyLocalhost", conf_get_bool(conf, CONF_even_proxy_localhost));
    write_setting_i_forced(sesskey, "ProxyMethod", conf_get_int(conf, CONF_proxy_type));
    write_setting_s_forced(sesskey, "ProxyHost", conf_get_str(conf, CONF_proxy_host));
    write_setting_i_forced(sesskey, "ProxyPort", conf_get_int(conf, CONF_proxy_port));
    write_setting_s_forced(sesskey, "ProxyUsername", conf_get_str(conf, CONF_proxy_username));
    write_setting_s_forced(sesskey, "ProxyPassword", conf_get_str(conf, CONF_proxy_password));
    write_setting_s_forced(sesskey, "ProxyTelnetCommand", conf_get_str(conf, CONF_proxy_telnet_command));
    write_setting_i_forced(sesskey, "ProxyLogToTerm", conf_get_int(conf, CONF_proxy_log_to_term));
    wmap_forced(sesskey, "Environment", conf, CONF_environmt, true);
    /* CONF_username is STR_AMBI in 0.84; must use conf_get_str_ambi */
    write_setting_s_forced(sesskey, "UserName", conf_get_str_ambi(conf, CONF_username, NULL));
    write_setting_b_forced(sesskey, "UserNameFromEnvironment", conf_get_bool(conf, CONF_username_from_env));
    write_setting_s_forced(sesskey, "LocalUserName", conf_get_str(conf, CONF_localusername));
    write_setting_b_forced(sesskey, "NoPTY", conf_get_bool(conf, CONF_nopty));
    write_setting_b_forced(sesskey, "Compression", conf_get_bool(conf, CONF_compression));
    write_setting_b_forced(sesskey, "TryAgent", conf_get_bool(conf, CONF_tryagent));
    write_setting_b_forced(sesskey, "AgentFwd", conf_get_bool(conf, CONF_agentfwd));
#ifndef NO_GSSAPI
    write_setting_b_forced(sesskey, "GssapiFwd", conf_get_bool(conf, CONF_gssapifwd));
#endif
    write_setting_b_forced(sesskey, "ChangeUsername", conf_get_bool(conf, CONF_change_username));
    wprefs_forced(sesskey, "Cipher", ciphernames, CIPHER_MAX, conf, CONF_ssh_cipherlist);
    wprefs_forced(sesskey, "KEX", kexnames, KEX_MAX, conf, CONF_ssh_kexlist);
    wprefs_forced(sesskey, "HostKey", hknames, HK_MAX, conf, CONF_ssh_hklist);
    write_setting_b_forced(sesskey, "PreferKnownHostKeys", conf_get_bool(conf, CONF_ssh_prefer_known_hostkeys));
    write_setting_i_forced(sesskey, "RekeyTime", conf_get_int(conf, CONF_ssh_rekey_time));
#ifndef NO_GSSAPI
    write_setting_i_forced(sesskey, "GssapiRekey", conf_get_int(conf, CONF_gssapirekey));
#endif
    write_setting_s_forced(sesskey, "RekeyBytes", conf_get_str(conf, CONF_ssh_rekey_data));
    write_setting_b_forced(sesskey, "SshNoAuth", conf_get_bool(conf, CONF_ssh_no_userauth));
    write_setting_b_forced(sesskey, "SshNoTrivialAuth", conf_get_bool(conf, CONF_ssh_no_trivial_userauth));
    write_setting_b_forced(sesskey, "SshBanner", conf_get_bool(conf, CONF_ssh_show_banner));
    write_setting_b_forced(sesskey, "AuthTIS", conf_get_bool(conf, CONF_try_tis_auth));
    write_setting_b_forced(sesskey, "AuthKI", conf_get_bool(conf, CONF_try_ki_auth));
#ifndef NO_GSSAPI
    write_setting_b_forced(sesskey, "AuthGSSAPI", conf_get_bool(conf, CONF_try_gssapi_auth));
    write_setting_b_forced(sesskey, "AuthGSSAPIKEX", conf_get_bool(conf, CONF_try_gssapi_kex));
    wprefs_forced(sesskey, "GSSLibs", gsslibkeywords, ngsslibs, conf, CONF_ssh_gsslist);
    write_setting_filename_forced(sesskey, "GSSCustom", conf_get_filename(conf, CONF_ssh_gss_custom));
#endif
    write_setting_b_forced(sesskey, "SshNoShell", conf_get_bool(conf, CONF_ssh_no_shell));
    write_setting_i_forced(sesskey, "SshProt", conf_get_int(conf, CONF_sshprot));
    write_setting_s_forced(sesskey, "LogHost", conf_get_str(conf, CONF_loghost));
    write_setting_b_forced(sesskey, "SSH2DES", conf_get_bool(conf, CONF_ssh2_des_cbc));
    write_setting_filename_forced(sesskey, "PublicKeyFile", conf_get_filename(conf, CONF_keyfile));
    /* CONF_remote_cmd is STR_AMBI in 0.84; must use conf_get_str_ambi */
    write_setting_s_forced(sesskey, "RemoteCommand", conf_get_str_ambi(conf, CONF_remote_cmd, NULL));
    write_setting_b_forced(sesskey, "RFCEnviron", conf_get_bool(conf, CONF_rfc_environ));
    write_setting_b_forced(sesskey, "PassiveTelnet", conf_get_bool(conf, CONF_passive_telnet));
    write_setting_b_forced(sesskey, "BackspaceIsDelete", conf_get_bool(conf, CONF_bksp_is_delete));
    write_setting_i_forced(sesskey, "EnterSendsCrLf", conf_get_int(conf, CONF_enter_sends_crlf));
    write_setting_b_forced(sesskey, "RXVTHomeEnd", conf_get_bool(conf, CONF_rxvt_homeend));
    write_setting_i_forced(sesskey, "LinuxFunctionKeys", conf_get_int(conf, CONF_funky_type));
    write_setting_b_forced(sesskey, "NoApplicationKeys", conf_get_bool(conf, CONF_no_applic_k));
    write_setting_b_forced(sesskey, "NoApplicationCursors", conf_get_bool(conf, CONF_no_applic_c));
    write_setting_b_forced(sesskey, "NoMouseReporting", conf_get_bool(conf, CONF_no_mouse_rep));
    write_setting_b_forced(sesskey, "NoRemoteResize", conf_get_bool(conf, CONF_no_remote_resize));
    write_setting_b_forced(sesskey, "NoAltScreen", conf_get_bool(conf, CONF_no_alt_screen));
    write_setting_b_forced(sesskey, "NoRemoteWinTitle", conf_get_bool(conf, CONF_no_remote_wintitle));
    write_setting_b_forced(sesskey, "NoRemoteClearScroll", conf_get_bool(conf, CONF_no_remote_clearscroll));
    write_setting_i_forced(sesskey, "RemoteQTitleAction", conf_get_int(conf, CONF_remote_qtitle_action));
    write_setting_b_forced(sesskey, "NoDBackspace", conf_get_bool(conf, CONF_no_dbackspace));
    write_setting_b_forced(sesskey, "NoRemoteCharset", conf_get_bool(conf, CONF_no_remote_charset));
    write_setting_b_forced(sesskey, "ApplicationCursorKeys", conf_get_bool(conf, CONF_app_cursor));
    write_setting_b_forced(sesskey, "ApplicationKeypad", conf_get_bool(conf, CONF_app_keypad));
    write_setting_b_forced(sesskey, "NetHackKeypad", conf_get_bool(conf, CONF_nethack_keypad));
    write_setting_b_forced(sesskey, "AltF4", conf_get_bool(conf, CONF_alt_f4));
    write_setting_b_forced(sesskey, "AltSpace", conf_get_bool(conf, CONF_alt_space));
    write_setting_b_forced(sesskey, "AltOnly", conf_get_bool(conf, CONF_alt_only));
    write_setting_b_forced(sesskey, "ComposeKey", conf_get_bool(conf, CONF_compose_key));
    write_setting_b_forced(sesskey, "CtrlAltKeys", conf_get_bool(conf, CONF_ctrlaltkeys));
#ifdef OSX_META_KEY_CONFIG
    write_setting_b_forced(sesskey, "OSXOptionMeta", conf_get_bool(conf, CONF_osx_option_meta));
    write_setting_b_forced(sesskey, "OSXCommandMeta", conf_get_bool(conf, CONF_osx_command_meta));
#endif
    write_setting_b_forced(sesskey, "TelnetKey", conf_get_bool(conf, CONF_telnet_keyboard));
    write_setting_b_forced(sesskey, "TelnetRet", conf_get_bool(conf, CONF_telnet_newline));
    write_setting_i_forced(sesskey, "LocalEcho", conf_get_int(conf, CONF_localecho));
    write_setting_i_forced(sesskey, "LocalEdit", conf_get_int(conf, CONF_localedit));
    write_setting_s_forced(sesskey, "Answerback", conf_get_str(conf, CONF_answerback));
    write_setting_b_forced(sesskey, "AlwaysOnTop", conf_get_bool(conf, CONF_alwaysontop));
    write_setting_b_forced(sesskey, "FullScreenOnAltEnter", conf_get_bool(conf, CONF_fullscreenonaltenter));
    write_setting_b_forced(sesskey, "HideMousePtr", conf_get_bool(conf, CONF_hide_mouseptr));
    write_setting_b_forced(sesskey, "SunkenEdge", conf_get_bool(conf, CONF_sunken_edge));
    write_setting_i_forced(sesskey, "WindowBorder", conf_get_int(conf, CONF_window_border));
    write_setting_i_forced(sesskey, "CurType", conf_get_int(conf, CONF_cursor_type));
    write_setting_b_forced(sesskey, "BlinkCur", conf_get_bool(conf, CONF_blink_cur));
    write_setting_i_forced(sesskey, "Beep", conf_get_int(conf, CONF_beep));
    write_setting_i_forced(sesskey, "BeepInd", conf_get_int(conf, CONF_beep_ind));
    write_setting_filename_forced(sesskey, "BellWaveFile", conf_get_filename(conf, CONF_bell_wavefile));
    write_setting_b_forced(sesskey, "BellOverload", conf_get_bool(conf, CONF_bellovl));
    write_setting_i_forced(sesskey, "BellOverloadN", conf_get_int(conf, CONF_bellovl_n));
    write_setting_i_forced(sesskey, "BellOverloadT", conf_get_int(conf, CONF_bellovl_t));
    write_setting_i_forced(sesskey, "BellOverloadS", conf_get_int(conf, CONF_bellovl_s));
    write_setting_i_forced(sesskey, "ScrollbackLines", conf_get_int(conf, CONF_savelines));
    write_setting_b_forced(sesskey, "DECOriginMode", conf_get_bool(conf, CONF_dec_om));
    write_setting_b_forced(sesskey, "AutoWrapMode", conf_get_bool(conf, CONF_wrap_mode));
    write_setting_b_forced(sesskey, "LFImpliesCR", conf_get_bool(conf, CONF_lfhascr));
    write_setting_b_forced(sesskey, "CRImpliesLF", conf_get_bool(conf, CONF_crhaslf));
    write_setting_b_forced(sesskey, "DisableArabicShaping", conf_get_bool(conf, CONF_no_arabicshaping));
    write_setting_b_forced(sesskey, "DisableBidi", conf_get_bool(conf, CONF_no_bidi));
    write_setting_b_forced(sesskey, "WinNameAlways", conf_get_bool(conf, CONF_win_name_always));
    write_setting_s_forced(sesskey, "WinTitle", conf_get_str(conf, CONF_wintitle));
    write_setting_i_forced(sesskey, "TermWidth", conf_get_int(conf, CONF_width));
    write_setting_i_forced(sesskey, "TermHeight", conf_get_int(conf, CONF_height));
    write_setting_fontspec_forced(sesskey, "Font", conf_get_fontspec(conf, CONF_font));
    write_setting_i_forced(sesskey, "FontQuality", conf_get_int(conf, CONF_font_quality));
    write_setting_i_forced(sesskey, "FontVTMode", conf_get_int(conf, CONF_vtmode));
    write_setting_b_forced(sesskey, "UseSystemColours", conf_get_bool(conf, CONF_system_colour));
    write_setting_b_forced(sesskey, "TryPalette", conf_get_bool(conf, CONF_try_palette));
    write_setting_b_forced(sesskey, "ANSIColour", conf_get_bool(conf, CONF_ansi_colour));
    write_setting_b_forced(sesskey, "Xterm256Colour", conf_get_bool(conf, CONF_xterm_256_colour));
    write_setting_b_forced(sesskey, "TrueColour", conf_get_bool(conf, CONF_true_colour));
    write_setting_i_forced(sesskey, "BoldAsColour", conf_get_int(conf, CONF_bold_style)-1);

    for (i = 0; i < 22; i++) {
        char buf[20], buf2[30];
        sprintf(buf, "Colour%d", i);
        sprintf(buf2, "%d,%d,%d",
                conf_get_int_int(conf, CONF_colours, i*3+0),
                conf_get_int_int(conf, CONF_colours, i*3+1),
                conf_get_int_int(conf, CONF_colours, i*3+2));
        write_setting_s_forced(sesskey, buf, buf2);
    }
    write_setting_b_forced(sesskey, "RawCNP", conf_get_bool(conf, CONF_rawcnp));
    write_setting_b_forced(sesskey, "UTF8linedraw", conf_get_bool(conf, CONF_utf8linedraw));
    write_setting_b_forced(sesskey, "PasteRTF", conf_get_bool(conf, CONF_rtf_paste));
    write_setting_i_forced(sesskey, "MouseIsXterm", conf_get_int(conf, CONF_mouse_is_xterm));
    write_setting_b_forced(sesskey, "RectSelect", conf_get_bool(conf, CONF_rect_select));
    write_setting_b_forced(sesskey, "PasteControls", conf_get_bool(conf, CONF_paste_controls));
    write_setting_b_forced(sesskey, "MouseOverride", conf_get_bool(conf, CONF_mouse_override));

    for (i = 0; i < 256; i += 32) {
        char buf[20], buf2[256];
        int j;
        sprintf(buf, "Wordness%d", i);
        *buf2 = '\0';
        for (j = i; j < i + 32; j++) {
            sprintf(buf2 + strlen(buf2), "%s%d",
                    (*buf2 ? "," : ""),
                    conf_get_int_int(conf, CONF_wordness, j));
        }
        write_setting_s_forced(sesskey, buf, buf2);
    }

    write_setting_b_forced(sesskey, "MouseAutocopy", conf_get_bool(conf, CONF_mouseautocopy));
    write_clip_setting_forced(sesskey, "MousePaste", conf, CONF_mousepaste, CONF_mousepaste_custom);
    write_clip_setting_forced(sesskey, "CtrlShiftIns", conf, CONF_ctrlshiftins, CONF_ctrlshiftins_custom);
    write_clip_setting_forced(sesskey, "CtrlShiftCV", conf, CONF_ctrlshiftcv, CONF_ctrlshiftcv_custom);
    write_setting_s_forced(sesskey, "LineCodePage", conf_get_str(conf, CONF_line_codepage));
    write_setting_b_forced(sesskey, "CJKAmbigWide", conf_get_bool(conf, CONF_cjk_ambig_wide));
    write_setting_b_forced(sesskey, "UTF8Override", conf_get_bool(conf, CONF_utf8_override));
    write_setting_s_forced(sesskey, "Printer", conf_get_str(conf, CONF_printer));
    write_setting_b_forced(sesskey, "CapsLockCyr", conf_get_bool(conf, CONF_xlat_capslockcyr));
    write_setting_b_forced(sesskey, "ScrollBar", conf_get_bool(conf, CONF_scrollbar));
    write_setting_b_forced(sesskey, "ScrollBarFullScreen", conf_get_bool(conf, CONF_scrollbar_in_fullscreen));
    write_setting_b_forced(sesskey, "ScrollOnKey", conf_get_bool(conf, CONF_scroll_on_key));
    write_setting_b_forced(sesskey, "ScrollOnDisp", conf_get_bool(conf, CONF_scroll_on_disp));
    write_setting_b_forced(sesskey, "EraseToScrollback", conf_get_bool(conf, CONF_erase_to_scrollback));
    write_setting_i_forced(sesskey, "LockSize", conf_get_int(conf, CONF_resize_action));
    write_setting_b_forced(sesskey, "BCE", conf_get_bool(conf, CONF_bce));
    write_setting_b_forced(sesskey, "BlinkText", conf_get_bool(conf, CONF_blinktext));
    write_setting_b_forced(sesskey, "X11Forward", conf_get_bool(conf, CONF_x11_forward));
    write_setting_s_forced(sesskey, "X11Display", conf_get_str(conf, CONF_x11_display));
    write_setting_i_forced(sesskey, "X11AuthType", conf_get_int(conf, CONF_x11_auth));
    write_setting_filename_forced(sesskey, "X11AuthFile", conf_get_filename(conf, CONF_xauthfile));
    write_setting_b_forced(sesskey, "LocalPortAcceptAll", conf_get_bool(conf, CONF_lport_acceptall));
    write_setting_b_forced(sesskey, "RemotePortAcceptAll", conf_get_bool(conf, CONF_rport_acceptall));
    wmap_forced(sesskey, "PortForwardings", conf, CONF_portfwd, true);
    write_setting_i_forced(sesskey, "BugIgnore1", 2-conf_get_int(conf, CONF_sshbug_ignore1));
    write_setting_i_forced(sesskey, "BugPlainPW1", 2-conf_get_int(conf, CONF_sshbug_plainpw1));
    write_setting_i_forced(sesskey, "BugRSA1", 2-conf_get_int(conf, CONF_sshbug_rsa1));
    write_setting_i_forced(sesskey, "BugIgnore2", 2-conf_get_int(conf, CONF_sshbug_ignore2));
    write_setting_i_forced(sesskey, "BugHMAC2", 2-conf_get_int(conf, CONF_sshbug_hmac2));
    write_setting_i_forced(sesskey, "BugDeriveKey2", 2-conf_get_int(conf, CONF_sshbug_derivekey2));
    write_setting_i_forced(sesskey, "BugRSAPad2", 2-conf_get_int(conf, CONF_sshbug_rsapad2));
    write_setting_i_forced(sesskey, "BugPKSessID2", 2-conf_get_int(conf, CONF_sshbug_pksessid2));
    write_setting_i_forced(sesskey, "BugRekey2", 2-conf_get_int(conf, CONF_sshbug_rekey2));
    write_setting_i_forced(sesskey, "BugMaxPkt2", 2-conf_get_int(conf, CONF_sshbug_maxpkt2));
    write_setting_i_forced(sesskey, "BugOldGex2", 2-conf_get_int(conf, CONF_sshbug_oldgex2));
    write_setting_i_forced(sesskey, "BugWinadj", 2-conf_get_int(conf, CONF_sshbug_winadj));
    write_setting_i_forced(sesskey, "BugChanReq", 2-conf_get_int(conf, CONF_sshbug_chanreq));
    write_setting_b_forced(sesskey, "StampUtmp", conf_get_bool(conf, CONF_stamp_utmp));
    write_setting_b_forced(sesskey, "LoginShell", conf_get_bool(conf, CONF_login_shell));
    write_setting_b_forced(sesskey, "ScrollbarOnLeft", conf_get_bool(conf, CONF_scrollbar_on_left));
    write_setting_fontspec_forced(sesskey, "BoldFont", conf_get_fontspec(conf, CONF_boldfont));
    write_setting_fontspec_forced(sesskey, "WideFont", conf_get_fontspec(conf, CONF_widefont));
    write_setting_fontspec_forced(sesskey, "WideBoldFont", conf_get_fontspec(conf, CONF_wideboldfont));
    write_setting_b_forced(sesskey, "ShadowBold", conf_get_bool(conf, CONF_shadowbold));
    write_setting_i_forced(sesskey, "ShadowBoldOffset", conf_get_int(conf, CONF_shadowboldoffset));
    write_setting_s_forced(sesskey, "SerialLine", conf_get_str(conf, CONF_serline));
    write_setting_i_forced(sesskey, "SerialSpeed", conf_get_int(conf, CONF_serspeed));
    write_setting_i_forced(sesskey, "SerialDataBits", conf_get_int(conf, CONF_serdatabits));
    write_setting_i_forced(sesskey, "SerialStopHalfbits", conf_get_int(conf, CONF_serstopbits));
    write_setting_i_forced(sesskey, "SerialParity", conf_get_int(conf, CONF_serparity));
    write_setting_i_forced(sesskey, "SerialFlowControl", conf_get_int(conf, CONF_serflow));
    write_setting_s_forced(sesskey, "WindowClass", conf_get_str(conf, CONF_winclass));
    write_setting_b_forced(sesskey, "ConnectionSharing", conf_get_bool(conf, CONF_ssh_connection_sharing));
    write_setting_b_forced(sesskey, "ConnectionSharingUpstream", conf_get_bool(conf, CONF_ssh_connection_sharing_upstream));
    write_setting_b_forced(sesskey, "ConnectionSharingDownstream", conf_get_bool(conf, CONF_ssh_connection_sharing_downstream));
    wmap_forced(sesskey, "SSHManualHostKeys", conf, CONF_ssh_manual_hostkeys, false);

#ifdef MOD_PROXY
    write_setting_s_forced(sesskey, "ProxySelection", conf_get_str(conf, CONF_proxyselection));
#endif
#ifdef MOD_PERSO
    if (conf_get_int(conf, CONF_transparencynumber)<-1) conf_set_int(conf, CONF_transparencynumber,-1);
    if (conf_get_int(conf, CONF_transparencynumber)>255) conf_set_int(conf, CONF_transparencynumber,255);
    write_setting_s_forced(sesskey, "HostAlt", conf_get_str(conf, CONF_host_alt));
    write_setting_i_forced(sesskey, "TransparencyValue", conf_get_int(conf, CONF_transparencynumber));
    write_setting_i_forced(sesskey, "SendToTray", conf_get_int(conf, CONF_sendtotray));
    write_setting_i_forced(sesskey, "Maximize", conf_get_int(conf, CONF_maximize));
    write_setting_i_forced(sesskey, "Fullscreen", conf_get_int(conf, CONF_fullscreen));
    write_setting_b_forced(sesskey, "SaveOnExit", conf_get_bool(conf, CONF_saveonexit));
    write_setting_i_forced(sesskey, "Icone", conf_get_int(conf, CONF_icone));
    write_setting_filename_forced(sesskey, "IconeFile", conf_get_filename(conf, CONF_iconefile));
    write_setting_i_forced(sesskey, "WinSCPProtocol", conf_get_int(conf, CONF_winscpprot));
    write_setting_s_forced(sesskey, "SFTPConnect", conf_get_str(conf, CONF_sftpconnect));
    write_setting_s_forced(sesskey, "PSCPOptions", conf_get_str(conf, CONF_pscpoptions));
    write_setting_s_forced(sesskey, "PSCPShell", conf_get_str(conf, CONF_pscpshell));
    write_setting_s_forced(sesskey, "PSCPRemoteDir", conf_get_str(conf, CONF_pscpremotedir));
    write_setting_s_forced(sesskey, "WinSCPOptions", conf_get_str(conf, CONF_winscpoptions));
    write_setting_s_forced(sesskey, "WinSCPRawSettings", conf_get_str(conf, CONF_winscprawsettings));
    write_setting_filename_forced(sesskey, "Scriptfile", conf_get_filename(conf, CONF_scriptfile));
    write_setting_s_forced(sesskey, "ScriptfileContent", conf_get_str(conf, CONF_scriptfilecontent));
    write_setting_s_forced(sesskey, "AntiIdle", conf_get_str(conf, CONF_antiidle));
    write_setting_s_forced(sesskey, "LogTimestamp", conf_get_str(conf, CONF_logtimestamp));
    write_setting_s_forced(sesskey, "Autocommand", conf_get_str(conf, CONF_autocommand));
    write_setting_s_forced(sesskey, "AutocommandOut", conf_get_str(conf, CONF_autocommandout));
    write_setting_s_forced(sesskey, "Folder", conf_get_str(conf, CONF_folder));
    write_setting_i_forced(sesskey, "LogTimeRotation", conf_get_int(conf, CONF_logtimerotation));
    write_setting_i_forced(sesskey, "TermXPos", conf_get_int(conf, CONF_xpos));
    write_setting_i_forced(sesskey, "TermYPos", conf_get_int(conf, CONF_ypos));
    write_setting_i_forced(sesskey, "WindowState", conf_get_int(conf, CONF_windowstate));
    write_setting_b_forced(sesskey, "SaveWindowPos", conf_get_bool(conf, CONF_save_windowpos));
    write_setting_b_forced(sesskey, "ForegroundOnBell", conf_get_bool(conf, CONF_foreground_on_bell));

#ifndef MOD_NOPASSWORD
    {
        char pst[4096];
        strcpy(pst, conf_get_str(conf, CONF_password));
        MASKPASS(GetCryptSaltFlag(), pst);
        cryptpassword(GetCryptSaltFlag(), pst, conf_get_str(conf, CONF_host), conf_get_str(conf, CONF_termtype));
        write_setting_s_forced(sesskey, "Password", pst);
        memset(pst, 0, strlen(pst));
    }
#endif
    write_setting_i_forced(sesskey, "CtrlTabSwitch", conf_get_int(conf, CONF_ctrl_tab_switch));
    write_setting_s_forced(sesskey, "Comment", conf_get_str(conf, CONF_comment));
    write_setting_b_forced(sesskey, "SCPAutoPwd", conf_get_bool(conf, CONF_scp_auto_pwd)); /* 0.84: BOOL (was conf_get_int -> assert) */
    write_setting_b_forced(sesskey, "NoFocusReporting", conf_get_bool(conf, CONF_no_focus_rep));
    write_setting_i_forced(sesskey, "LinesAtAScroll", conf_get_int(conf, CONF_scrolllines));
    write_setting_b_forced(sesskey, "SSHTunnelInTitle", conf_get_bool(conf, CONF_ssh_tunnel_print_in_title));
    write_setting_b_forced(sesskey, "OSC52WarnBeforeClipboardSync", conf_get_bool(conf, CONF_osc52_warn_before_cliboard_sync));
#endif
    fclose(sesskey);
}
