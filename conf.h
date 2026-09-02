/*
 * Master list of configuration options living in the Conf data
 * structure.
 *
 * Each CONF_OPTION directive defines a single CONF_foo primary key in
 * Conf, and can be equipped with the following properties:
 *
 *  - VALUE_TYPE: the type of data associated with that key
 *  - SUBKEY_TYPE: if the primary key goes with a subkey (that is, the
 *    primary key identifies some mapping from subkeys to values), the
 *    data type of the subkey
 *  - DEFAULT_INT, DEFAULT_STR, DEFAULT_BOOL: the default value for
 *    the key, if no save data is available. Must match VALUE_TYPE, if
 *    the key has no subkey. Otherwise, no default is permitted, and
 *    the default value of the mapping is assumed to be empty (and if
 *    not, then LOAD_CUSTOM code must override that).
 *  - SAVE_KEYWORD: the keyword used for the option in the Windows
 *    registry or ~/.putty/sessions save files.
 *  - STORAGE_ENUM: for int-typed settings with no subkeys, this
 *    identifies an enumeration in conf-enums.h which maps internal
 *    values of the setting in the Conf to values in the saved data.
 *  - LOAD_CUSTOM, SAVE_CUSTOM: suppress automated loading or saving
 *    (respectively) of this setting, in favour of manual code in
 *    settings.c load_open_settings() or save_open_settings()
 *    respectively.
 *  - NOT_SAVED: indicate that this setting is not part of saved
 *    session data at all.
 */

CONF_OPTION(host,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("HostName"),
)
CONF_OPTION(port,
    VALUE_TYPE(INT),
    SAVE_KEYWORD("PortNumber"),
    LOAD_CUSTOM, /* default value depends on the value of CONF_protocol */
)
CONF_OPTION(protocol,
    VALUE_TYPE(INT), /* PROT_SSH, PROT_TELNET etc */
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * Notionally SAVE_KEYWORD("Protocol"), but saving/loading is handled by
     * custom code because the stored value is a string representation
     * of the protocol name.
     */
)
CONF_OPTION(addressfamily,
    VALUE_TYPE(INT),
    DEFAULT_INT(ADDRTYPE_UNSPEC),
    SAVE_KEYWORD("AddressFamily"),
    STORAGE_ENUM(addressfamily),
)
CONF_OPTION(close_on_exit,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("CloseOnExit"),
    STORAGE_ENUM(off_auto_on),
)
CONF_OPTION(warn_on_close,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("WarnOnClose"),
)
CONF_OPTION(ping_interval,
    VALUE_TYPE(INT), /* in seconds */
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * Saving/loading is handled by custom code because for historical
     * reasons this value corresponds to two save keywords,
     * "PingInterval" (measured in minutes) and "PingIntervalSecs"
     * (measured in seconds), which are added together on loading.
     * Rationale: the value was once measured in minutes, and the
     * seconds field was added later.
     */
)
CONF_OPTION(tcp_nodelay,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("TCPNoDelay"),
)
CONF_OPTION(tcp_keepalives,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),   /* KiTTY: on by default - OS SO_KEEPALIVE refreshes NAT
                           * and detects dead peers; no keepalive-timeout->fatal
                           * path exists, so this never causes spurious drops. */
    SAVE_KEYWORD("TCPKeepalives"),
)
CONF_OPTION(loghost, /* logical host being contacted, for host key check */
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("LogHost"),
)
CONF_OPTION(pre_connect_command,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("PreConnectCommand"),
)

/* Proxy options */
CONF_OPTION(proxy_exclude_list,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("ProxyExcludeList"),
)
CONF_OPTION(proxy_dns,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("ProxyDNS"),
    STORAGE_ENUM(off_auto_on),
)
CONF_OPTION(even_proxy_localhost,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ProxyLocalhost"),
)
CONF_OPTION(proxy_type,
    VALUE_TYPE(INT), /* PROXY_NONE, PROXY_SOCKS4, ... */
    STORAGE_ENUM(proxy_type),
    SAVE_KEYWORD("ProxyMethod"),
    LOAD_CUSTOM,
    /*
     * Custom load code: there was an earlier keyword "ProxyType"
     * using a different enumeration, in which SOCKS4 and SOCKS5
     * shared a value, and a second keyword "ProxySOCKSVersion"
     * disambiguated.
     */
)
CONF_OPTION(proxy_host,
    VALUE_TYPE(STR),
    DEFAULT_STR("proxy"),
    SAVE_KEYWORD("ProxyHost"),
)
CONF_OPTION(proxy_port,
    VALUE_TYPE(INT),
    DEFAULT_INT(80),
    SAVE_KEYWORD("ProxyPort"),
)
CONF_OPTION(proxy_username,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("ProxyUsername"),
)
CONF_OPTION(proxy_password,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("ProxyPassword"),
)
CONF_OPTION(proxy_telnet_command,
    VALUE_TYPE(STR),
    DEFAULT_STR("connect %host %port\\n"),
    SAVE_KEYWORD("ProxyTelnetCommand"),
)
CONF_OPTION(proxy_log_to_term,
    VALUE_TYPE(INT),
    /* KiTTY: default AUTO ("only until session starts") instead of PuTTY's
     * FORCE_OFF, so proxy connections surface their handshake/diagnostics in the
     * terminal during setup (then go quiet once the session is up). */
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("ProxyLogToTerm"),
    STORAGE_ENUM(on_off_auto),
)
/* KiTTY MOD_PROXY: name of a saved proxy definition (Proxies\ subtree / ini
 * Proxies dir) applied over the session's own proxy at connect.
 * "- Session defined proxy -" = no-op (matches LoadProxyInfo's short-circuit). */
CONF_OPTION(proxyselection,
    VALUE_TYPE(STR),
    DEFAULT_STR(KITTY_PROXY_SESSION),
    SAVE_KEYWORD("ProxySelection"),
)

/* KiTTY: does THIS session accept broadcast text (/command, -sendcmd) from
 * other KiTTY windows of the same install?
 *
 * Saved with the session, so the sessions you drive in bulk can be armed while
 * the production one you keep open beside them is not - and the window menu
 * shows which is which (Tools > Accept broadcast, which overrides this at
 * runtime until the next Apply).
 *
 * The effective starting state is THIS setting OR the install-wide
 * [KiTTY] sendcmdmode: the ini switch is "I use this feature here", the session
 * flag is "and this session in particular". Default off, like the ini.
 */
CONF_OPTION(kitty_accept_broadcast,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("AcceptBroadcast"),
)

/* KiTTY: the key THIS session listens for. Empty = the install's key
 * ([KiTTY] sendcmdgroup, generated when unset). Setting it here aims the
 * feature: give three sessions a shared key and only they answer, even where
 * the install key would have matched. Shown - and editable behind an Edit
 * button - in Session > Scripting. */
CONF_OPTION(kitty_broadcast_key,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("BroadcastKey"),
)

/*
 * KiTTY: bound on chained SSH proxies, and the depth reached so far.
 *
 * An SSH jump host is configured from a Conf that may itself name a proxy, so a
 * configuration leading back into its own chain recurses without bound - seen live
 * as a wall of "Making proxy^N SSH connection to ..." and an unresponsive window.
 * There is deliberately no identity or cycle rule: a jump host reached through
 * itself is legitimate (a service bound to its own localhost, an internal
 * interface that routes differently), and we cannot know how anyone's hosts are
 * named or what they serve. A depth bound is the only honest limit.
 *
 * Both are NOT_SAVED. The depth is per-connection state that travels DOWN the
 * chain inside the child's Conf; a global counter would be wrong because
 * connections overlap. The max is filled in at startup from kitty.ini [KiTTY]
 * proxychainmax and carried in the Conf so that proxy/sshproxy.c - which compiles
 * into the shared crypto library, WITHOUT MOD_PERSO - can read it without linking
 * against a KiTTY symbol. 0 means "not configured"; the code substitutes its
 * default.
 */
CONF_OPTION(proxy_chain_max,
    VALUE_TYPE(INT),
    DEFAULT_INT(0),
    NOT_SAVED,
)
CONF_OPTION(proxy_chain_depth,
    VALUE_TYPE(INT),
    DEFAULT_INT(0),
    NOT_SAVED,
)
/*
 * KiTTY: how proxy/sshproxy.c must read the proxy HOST for this connection.
 *
 * 0 (the default, and what stock PuTTY does): try the host as the title of a
 * saved session first, and fall back to treating it as a hostname. 1: it is a
 * hostname, full stop.
 *
 * Decided per connection by kitty_proxy_select() from the named proxy's own
 * setting, or failing that from kitty.ini [KiTTY] namedproxy, and carried here
 * because sshproxy.c compiles into the shared crypto library WITHOUT MOD_PERSO
 * and cannot call a KiTTY accessor. Defaulting to 0 keeps the behaviour every
 * existing configuration already has.
 */
CONF_OPTION(proxy_named_hostname,
    VALUE_TYPE(INT),
    DEFAULT_INT(0),
    NOT_SAVED,
)
/*
 * KiTTY: what a NAMED PROXY says about its own Host field, carried with the rest
 * of the definition (stored as ProxyHostIs).
 *
 *  -1  say nothing - follow kitty.ini [KiTTY] namedproxy (the default)
 *   0  it may be the title of a saved session, as PuTTY has always allowed
 *   1  it is a hostname
 *
 * NOT_SAVED because it belongs to a proxy definition, not to a session: it is
 * only ever in a Conf while a definition is being loaded, edited or saved.
 */
CONF_OPTION(proxy_host_kind,
    VALUE_TYPE(INT),
    DEFAULT_INT(-1),
    NOT_SAVED,
)
/*
 * KiTTY: line spacing as a PERCENTAGE of the font's own line height. 100 is the
 * font's metrics untouched, and is the default; 120 makes each cell a fifth
 * taller with the glyph centred in it.
 *
 * A percentage rather than pixels, so it survives a font change and a move to a
 * monitor at a different DPI - the same reason other terminals express it as a
 * multiplier. It is also what makes the cell height INVERTIBLE, which matters
 * because init_fonts() is called again on resize with the current cell height:
 * glyph = cell * 100 / percent recovers the font's own height instead of
 * inflating an already-inflated one.
 */
CONF_OPTION(line_spacing,
    VALUE_TYPE(INT),
    DEFAULT_INT(100),
    SAVE_KEYWORD("LineSpacing"),
)

/* SSH options */
CONF_OPTION(remote_cmd,
    VALUE_TYPE(STR_AMBI),
    DEFAULT_STR(""),
    SAVE_KEYWORD("RemoteCommand"),
)
CONF_OPTION(remote_cmd2,
    /*
     * Fallback command to try to run if remote_cmd fails. Only set
     * internally by PSCP and PSFTP (so that they can try multiple
     * methods of running an SFTP server at the remote end); never set
     * by user configuration, or loaded or saved.
     */
    VALUE_TYPE(STR_AMBI),
    DEFAULT_STR(""),
    NOT_SAVED,
)
CONF_OPTION(nopty,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoPTY"),
)
CONF_OPTION(compression,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("Compression"),
)
CONF_OPTION(ssh_kexlist,
    SUBKEY_TYPE(INT), /* indices in preference order: 0,...,KEX_MAX-1
                       * (lower is more preferred) */
    VALUE_TYPE(INT),  /* KEX_* enum values */
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for preference lists */
)
CONF_OPTION(ssh_hklist,
    SUBKEY_TYPE(INT), /* indices in preference order: 0,...,HK_MAX-1
                       * (lower is more preferred) */
    VALUE_TYPE(INT),  /* HK_* enum values */
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for preference lists */
)
CONF_OPTION(ssh_prefer_known_hostkeys,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("PreferKnownHostKeys"),
)
CONF_OPTION(ssh_rekey_time,
    VALUE_TYPE(INT), /* in minutes */
    DEFAULT_INT(60),
    SAVE_KEYWORD("RekeyTime"),
)
CONF_OPTION(ssh_rekey_data,
    VALUE_TYPE(STR), /* string encoding e.g. "100K", "2M", "1G" */
    DEFAULT_STR("1G"),
    SAVE_KEYWORD("RekeyBytes"),
)
CONF_OPTION(tryagent,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("TryAgent"),
)
CONF_OPTION(agentfwd,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("AgentFwd"),
)
CONF_OPTION(change_username, /* allow username switching in SSH-2 */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ChangeUsername"),
)
CONF_OPTION(ssh_cipherlist,
    SUBKEY_TYPE(INT), /* indices in preference order: 0,...,CIPHER_MAX-1
                       * (lower is more preferred) */
    VALUE_TYPE(INT),  /* CIPHER_* enum values */
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for preference lists */
)
CONF_OPTION(keyfile,
    VALUE_TYPE(FILENAME),
    SAVE_KEYWORD("PublicKeyFile"),
)
CONF_OPTION(detached_cert,
    VALUE_TYPE(FILENAME),
    SAVE_KEYWORD("DetachedCertificate"),
)
CONF_OPTION(auth_plugin,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("AuthPlugin"),
)
CONF_OPTION(sshprot,
    /*
     * Which SSH protocol to use.
     *
     * For historical reasons, the current legal values for CONF_sshprot
     * are:
     *  0 = SSH-1 only
     *  3 = SSH-2 only
     *
     * We used to also support
     *  1 = SSH-1 with fallback to SSH-2
     *  2 = SSH-2 with fallback to SSH-1
     *
     * and we continue to use 0/3 in storage formats rather than the more
     * obvious 1/2 to avoid surprises if someone saves a session and later
     * downgrades PuTTY. So it's easier to use these numbers internally too.
     */
    VALUE_TYPE(INT),
    DEFAULT_INT(3),
    SAVE_KEYWORD("SshProt"),
    STORAGE_ENUM(ssh_protocol),
)
CONF_OPTION(ssh_simple,
    /*
     * This means that we promise never to open any channel other
     * than the main one, which means it can safely use a very large
     * window in SSH-2.
     *
     * Only ever set internally by file transfer tools; never set by
     * user configuration, or loaded or saved.
     */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    NOT_SAVED,
)
CONF_OPTION(ssh_connection_sharing,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ConnectionSharing"),
)
CONF_OPTION(ssh_connection_sharing_upstream,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("ConnectionSharingUpstream"),
)
CONF_OPTION(ssh_connection_sharing_downstream,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("ConnectionSharingDownstream"),
)
CONF_OPTION(ssh_manual_hostkeys,
    /*
     * Manually configured host keys to accept regardless of the state
     * of the host key cache.
     *
     * This is conceptually a set rather than a dictionary: every
     * value in this map is the empty string, and the set of subkeys
     * that exist is the important data.
     */
    SUBKEY_TYPE(STR),
    VALUE_TYPE(STR),
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for mappings */
)
CONF_OPTION(ssh2_des_cbc, /* "des-cbc" unrecommended SSH-2 cipher */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("SSH2DES"),
)
CONF_OPTION(ssh_no_userauth, /* bypass "ssh-userauth" (SSH-2 only) */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("SshNoAuth"),
)
CONF_OPTION(ssh_warn_pre_quantum, /* warn if kex is not post-quantum secure */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("SshWarnPreQuantum"),
)
CONF_OPTION(ssh_no_trivial_userauth, /* disable trivial types of auth */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("SshNoTrivialAuth"),
)
CONF_OPTION(ssh_show_banner, /* show USERAUTH_BANNERs (SSH-2 only) */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("SshBanner"),
)
CONF_OPTION(try_tis_auth,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("AuthTIS"),
)
CONF_OPTION(try_ki_auth,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("AuthKI"),
)
CONF_OPTION(try_gssapi_auth, /* attempt gssapi via ssh userauth */
    VALUE_TYPE(BOOL),
    LOAD_CUSTOM, SAVE_CUSTOM, /* under #ifndef NO_GSSAPI */
)
CONF_OPTION(try_gssapi_kex, /* attempt gssapi via ssh kex */
    VALUE_TYPE(BOOL),
    LOAD_CUSTOM, SAVE_CUSTOM, /* under #ifndef NO_GSSAPI */
)
CONF_OPTION(gssapifwd, /* forward tgt via gss */
    VALUE_TYPE(BOOL),
    LOAD_CUSTOM, SAVE_CUSTOM, /* under #ifndef NO_GSSAPI */
)
CONF_OPTION(gssapirekey, /* KEXGSS refresh interval (mins) */
    VALUE_TYPE(INT),
    LOAD_CUSTOM, SAVE_CUSTOM, /* under #ifndef NO_GSSAPI */
)
CONF_OPTION(ssh_gsslist,
    SUBKEY_TYPE(INT), /* indices in preference order: 0,...,ngsslibs
                       * (lower is more preferred; ngsslibs is a platform-
                       * dependent value) */
    VALUE_TYPE(INT),  /* indices of GSSAPI lib types (platform-dependent) */
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for preference lists, also this
                               * setting is under #ifndef NO_GSSAPI */
)
CONF_OPTION(ssh_gss_custom,
    VALUE_TYPE(FILENAME),
    LOAD_CUSTOM, SAVE_CUSTOM, /* under #ifndef NO_GSSAPI */
)
CONF_OPTION(ssh_subsys, /* run a subsystem rather than a command */
    /*
     * Only set internally by PSCP and PSFTP; never set by user
     * configuration, or loaded or saved.
     */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    NOT_SAVED,
)
CONF_OPTION(ssh_subsys2, /* fallback to go with remote_cmd2 */
    /*
     * Only set internally by PSCP and PSFTP; never set by user
     * configuration, or loaded or saved.
     */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    NOT_SAVED,
)
CONF_OPTION(ssh_no_shell, /* avoid running a shell */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("SshNoShell"),
)
CONF_OPTION(ssh_nc_host, /* host to connect to in `nc' mode */
    /*
     * Only set by the '-nc' command-line option and by the SSH proxy
     * code. There's no GUI config option for this, and therefore it's
     * also never loaded or saved.
     */
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    NOT_SAVED,
)
CONF_OPTION(ssh_nc_port, /* port to connect to in `nc' mode */
    /*
     * Only set by the '-nc' command-line option and by the SSH proxy
     * code. There's no GUI config option for this, and therefore it's
     * also never loaded or saved.
     */
    VALUE_TYPE(INT),
    DEFAULT_INT(0),
    NOT_SAVED,
)

/* Telnet options */
CONF_OPTION(termtype,
    VALUE_TYPE(STR),
    DEFAULT_STR("xterm"),
    SAVE_KEYWORD("TerminalType"),
)
CONF_OPTION(termspeed,
    VALUE_TYPE(STR),
    DEFAULT_STR("38400,38400"),
    SAVE_KEYWORD("TerminalSpeed"),
)
CONF_OPTION(ttymodes,
    /*
     * The full set of permitted subkeys is listed in
     * ssh/ttymode-list.h, as the first parameter of each TTYMODE_CHAR
     * or TTYMODE_FLAG macro.
     *
     * The permitted value strings are:
     *
     *  - "N" means do not include a record for this mode at all in
     *    the terminal mode data in the "pty-req" channel request.
     *    Corresponds to setting the mode to 'Nothing' in the GUI.
     *  - "A" means use PuTTY's automatic default, matching the
     *    settings for GUI PuTTY's terminal window or Unix Plink's
     *    controlling tty. Corresponds to setting 'Auto' in the GUI.
     *  - "V" followed by further string data means send a custom
     *    value to the SSH server. Values are as documented in the
     *    manual.
     */
    SUBKEY_TYPE(STR),
    VALUE_TYPE(STR),
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for mappings */
)
CONF_OPTION(environmt,
    SUBKEY_TYPE(STR), /* environment variable name */
    VALUE_TYPE(STR),  /* environment variable value */
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for mappings */
)
CONF_OPTION(username,
    VALUE_TYPE(STR_AMBI),
    DEFAULT_STR(""),
    SAVE_KEYWORD("UserName"),
)
CONF_OPTION(username_from_env,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("UserNameFromEnvironment"),
)
CONF_OPTION(localusername,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("LocalUserName"),
)
CONF_OPTION(rfc_environ,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("RFCEnviron"),
)
CONF_OPTION(passive_telnet,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("PassiveTelnet"),
)

/* Serial port options */
CONF_OPTION(serline,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("SerialLine"),
)
CONF_OPTION(serspeed,
    VALUE_TYPE(INT),
    DEFAULT_INT(9600),
    SAVE_KEYWORD("SerialSpeed"),
)
CONF_OPTION(serdatabits,
    VALUE_TYPE(INT),
    DEFAULT_INT(8),
    SAVE_KEYWORD("SerialDataBits"),
)
CONF_OPTION(serstopbits,
    VALUE_TYPE(INT),
    DEFAULT_INT(2),
    SAVE_KEYWORD("SerialStopHalfbits"),
)
CONF_OPTION(serparity,
    VALUE_TYPE(INT),
    DEFAULT_INT(SER_PAR_NONE),
    SAVE_KEYWORD("SerialParity"),
    STORAGE_ENUM(serparity),
)
CONF_OPTION(serflow,
    VALUE_TYPE(INT),
    DEFAULT_INT(SER_FLOW_XONXOFF),
    SAVE_KEYWORD("SerialFlowControl"),
    STORAGE_ENUM(serflow),
)

/* SUPDUP options */
CONF_OPTION(supdup_location,
    VALUE_TYPE(STR),
    DEFAULT_STR("The Internet"),
    SAVE_KEYWORD("SUPDUPLocation"),
)
CONF_OPTION(supdup_ascii_set,
    VALUE_TYPE(INT),
    DEFAULT_INT(SUPDUP_CHARSET_ASCII),
    SAVE_KEYWORD("SUPDUPCharset"),
    STORAGE_ENUM(supdup_charset),
)
CONF_OPTION(supdup_more,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("SUPDUPMoreProcessing"),
)
CONF_OPTION(supdup_scroll,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("SUPDUPScrolling"),
)

/* Keyboard options */
CONF_OPTION(bksp_is_delete,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("BackspaceIsDelete"),
)
CONF_OPTION(rxvt_homeend,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("RXVTHomeEnd"),
)
CONF_OPTION(funky_type,
    VALUE_TYPE(INT),
    DEFAULT_INT(FUNKY_TILDE),
    SAVE_KEYWORD("LinuxFunctionKeys"),
    STORAGE_ENUM(funky_type),
)
CONF_OPTION(sharrow_type,
    VALUE_TYPE(INT),
    /* KiTTY default: SHARROW_BITMAP (Ctrl+arrow word navigation), matching KiTTY
     * 0.76 - not PuTTY's SHARROW_APPLICATION. Restores Ctrl+Left/Right for
     * sessions that relied on the old default (e.g. migrated from the 9bis hive).
     * NB: deliberate KiTTY-vs-PuTTY default divergence; test_conf is taught
     * this default (KiTTY-aware, like TCPKeepalives) - re-check on rebase. */
    DEFAULT_INT(SHARROW_BITMAP),
    SAVE_KEYWORD("ShiftedArrowKeys"),
    STORAGE_ENUM(sharrow_type),
)
CONF_OPTION(word_nav_modifier,
    VALUE_TYPE(INT),
    DEFAULT_INT(WORDNAV_ALT),
    SAVE_KEYWORD("WordNavModifier"),
    STORAGE_ENUM(word_nav_modifier),
)
CONF_OPTION(check_update_startup,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("CheckUpdateStartup"),
)
CONF_OPTION(remember_winpos,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("RememberWindowPos"),
)
CONF_OPTION(no_applic_c, /* totally disable app cursor keys */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoApplicationCursors"),
)
CONF_OPTION(no_applic_k, /* totally disable app keypad */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoApplicationKeys"),
)
CONF_OPTION(no_mouse_rep, /* totally disable mouse reporting */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoMouseReporting"),
)
CONF_OPTION(no_remote_resize, /* disable remote resizing */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoRemoteResize"),
)
CONF_OPTION(no_alt_screen, /* disable alternate screen */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoAltScreen"),
)
CONF_OPTION(no_remote_wintitle, /* disable remote retitling */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoRemoteWinTitle"),
)
CONF_OPTION(no_remote_clearscroll, /* disable ESC[3J */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoRemoteClearScroll"),
)
CONF_OPTION(no_dbackspace, /* disable destructive backspace */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoDBackspace"),
)
CONF_OPTION(no_remote_charset, /* disable remote charset config */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NoRemoteCharset"),
)
CONF_OPTION(remote_qtitle_action, /* handling of remote window title queries */
    VALUE_TYPE(INT),
    STORAGE_ENUM(remote_qtitle_action),
    SAVE_KEYWORD("RemoteQTitleAction"),
    LOAD_CUSTOM, /* older versions had a boolean "NoRemoteQTitle"
                  * before we ended up with three options */
)
CONF_OPTION(app_cursor,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ApplicationCursorKeys"),
)
CONF_OPTION(app_keypad,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ApplicationKeypad"),
)
CONF_OPTION(nethack_keypad,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("NetHackKeypad"),
)
CONF_OPTION(telnet_keyboard,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("TelnetKey"),
)
CONF_OPTION(telnet_newline,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("TelnetRet"),
)
CONF_OPTION(alt_f4, /* is it special? */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("AltF4"),
)
CONF_OPTION(alt_space, /* is it special? */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("AltSpace"),
)
CONF_OPTION(alt_only, /* is it special? */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("AltOnly"),
)
CONF_OPTION(localecho,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("LocalEcho"),
    STORAGE_ENUM(on_off_auto),
)
CONF_OPTION(localedit,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("LocalEdit"),
    STORAGE_ENUM(on_off_auto),
)
CONF_OPTION(alwaysontop,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("AlwaysOnTop"),
)
CONF_OPTION(fullscreenonaltenter,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),    /* KiTTY: Alt+Enter toggles full screen by default */
    SAVE_KEYWORD("FullScreenOnAltEnter"),
)
CONF_OPTION(scroll_on_key,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ScrollOnKey"),
)
CONF_OPTION(scroll_on_disp,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("ScrollOnDisp"),
)
CONF_OPTION(erase_to_scrollback,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("EraseToScrollback"),
)
CONF_OPTION(compose_key,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ComposeKey"),
)
CONF_OPTION(ctrlaltkeys,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("CtrlAltKeys"),
)
CONF_OPTION(osx_option_meta,
    VALUE_TYPE(BOOL),
    LOAD_CUSTOM, SAVE_CUSTOM, /* under #ifdef OSX_META_KEY_CONFIG */
)
CONF_OPTION(osx_command_meta,
    VALUE_TYPE(BOOL),
    LOAD_CUSTOM, SAVE_CUSTOM, /* under #ifdef OSX_META_KEY_CONFIG */
)
CONF_OPTION(wintitle, /* initial window title */
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("WinTitle"),
)
/* Terminal options */
CONF_OPTION(savelines,
    VALUE_TYPE(INT),
    DEFAULT_INT(2000),
    SAVE_KEYWORD("ScrollbackLines"),
)
CONF_OPTION(dec_om,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("DECOriginMode"),
)
CONF_OPTION(wrap_mode,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("AutoWrapMode"),
)
CONF_OPTION(lfhascr,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("LFImpliesCR"),
)
CONF_OPTION(cursor_type,
    VALUE_TYPE(INT),
    DEFAULT_INT(0),
    SAVE_KEYWORD("CurType"),
    STORAGE_ENUM(cursor_type),
)
CONF_OPTION(blink_cur,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("BlinkCur"),
)
CONF_OPTION(beep,
    VALUE_TYPE(INT),
    DEFAULT_INT(BELL_DEFAULT),
    SAVE_KEYWORD("Beep"),
    STORAGE_ENUM(beep),
)
CONF_OPTION(beep_ind,
    VALUE_TYPE(INT),
    DEFAULT_INT(B_IND_DISABLED),
    SAVE_KEYWORD("BeepInd"),
    STORAGE_ENUM(beep_indication),
)
CONF_OPTION(bellovl, /* bell overload protection active? */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("BellOverload"),
)
CONF_OPTION(bellovl_n, /* number of bells to cause overload */
    VALUE_TYPE(INT),
    DEFAULT_INT(5),
    SAVE_KEYWORD("BellOverloadN"),
)
CONF_OPTION(bellovl_t, /* time interval for overload (ticks) */
    VALUE_TYPE(INT),
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * Loading and saving is done in custom code because the format is
     * platform-dependent for historical reasons: on Unix, the stored
     * value is multiplied by 1000. (And since TICKSPERSEC=1000 on
     * that platform, it means the stored value is interpreted in
     * microseconds.)
     */
)
CONF_OPTION(bellovl_s, /* period of silence to re-enable bell (s) */
    VALUE_TYPE(INT),
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * Loading and saving is done in custom code because the format is
     * platform-dependent for historical reasons: on Unix, the stored
     * value is multiplied by 1000. (And since TICKSPERSEC=1000 on
     * that platform, it means the stored value is interpreted in
     * microseconds.)
     */
)
CONF_OPTION(bell_wavefile,
    VALUE_TYPE(FILENAME),
    SAVE_KEYWORD("BellWaveFile"),
)
CONF_OPTION(scrollbar,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("ScrollBar"),
)
CONF_OPTION(scrollbar_in_fullscreen,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ScrollBarFullScreen"),
)
CONF_OPTION(resize_action,
    VALUE_TYPE(INT),
    DEFAULT_INT(RESIZE_TERM),
    SAVE_KEYWORD("LockSize"),
    STORAGE_ENUM(resize_effect),
)
CONF_OPTION(bce,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("BCE"),
)
CONF_OPTION(blinktext,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("BlinkText"),
)
CONF_OPTION(win_name_always,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("WinNameAlways"),
)
CONF_OPTION(launcher_global_hotkey_enabled,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("LauncherGlobalHotkeyEnabled"),
)
CONF_OPTION(launcher_global_hotkey,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("LauncherGlobalHotkey"),
)
/*
 * KiTTY: opt-in pin of the configured private key FILE. When non-empty, the
 * bare "SHA256:..." fingerprint the key file's public half must match at
 * connect time; a mismatching file is refused before any passphrase prompt.
 * Recorded from the config box (Connection/SSH/Auth/Credentials); empty =
 * feature off. Always the KEY's own blob, never a detached certificate's -
 * certificates rotate by design.
 */
CONF_OPTION(publickey_fingerprint,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("PublicKeyFingerprint"),
)
CONF_OPTION(width,
    VALUE_TYPE(INT),
    DEFAULT_INT(80),
    SAVE_KEYWORD("TermWidth"),
)
CONF_OPTION(height,
    VALUE_TYPE(INT),
    DEFAULT_INT(24),
    SAVE_KEYWORD("TermHeight"),
)
CONF_OPTION(font,
    VALUE_TYPE(FONT),
    SAVE_KEYWORD("Font"),
)
CONF_OPTION(font_quality,
    VALUE_TYPE(INT),
    DEFAULT_INT(FQ_DEFAULT),
    SAVE_KEYWORD("FontQuality"),
    STORAGE_ENUM(font_quality),
)
CONF_OPTION(logfilename,
    VALUE_TYPE(FILENAME),
    SAVE_KEYWORD("LogFileName"),
)
CONF_OPTION(logtype,
    VALUE_TYPE(INT),
    DEFAULT_INT(LGTYP_NONE),
    SAVE_KEYWORD("LogType"),
    STORAGE_ENUM(log_type),
)
CONF_OPTION(logxfovr,
    VALUE_TYPE(INT),
    DEFAULT_INT(LGXF_ASK),
    SAVE_KEYWORD("LogFileClash"),
    STORAGE_ENUM(log_to_existing_file),
)
CONF_OPTION(logflush,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("LogFlush"),
)
CONF_OPTION(logheader,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("LogHeader"),
)
CONF_OPTION(logomitpass,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("SSHLogOmitPasswords"),
)
CONF_OPTION(logomitdata,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("SSHLogOmitData"),
)
CONF_OPTION(hide_mouseptr,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("HideMousePtr"),
)
CONF_OPTION(sunken_edge,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("SunkenEdge"),
)
CONF_OPTION(window_border,
    VALUE_TYPE(INT), /* in pixels */
    DEFAULT_INT(1),
    SAVE_KEYWORD("WindowBorder"),
)
CONF_OPTION(answerback,
    VALUE_TYPE(STR),
    DEFAULT_STR("PuTTY"),
    SAVE_KEYWORD("Answerback"),
)
CONF_OPTION(printer,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("Printer"),
)
CONF_OPTION(no_arabicshaping,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("DisableArabicShaping"),
)
CONF_OPTION(no_bidi,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("DisableBidi"),
)
CONF_OPTION(no_bracketed_paste,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("DisableBracketedPaste"),
)

/* Colour options */
CONF_OPTION(ansi_colour,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("ANSIColour"),
)
CONF_OPTION(xterm_256_colour,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("Xterm256Colour"),
)
CONF_OPTION(true_colour,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("TrueColour"),
)
CONF_OPTION(system_colour,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("UseSystemColours"),
)
CONF_OPTION(try_palette,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("TryPalette"),
)
CONF_OPTION(bold_style,
    VALUE_TYPE(INT),
    DEFAULT_INT(2),
    SAVE_KEYWORD("BoldAsColour"),
    STORAGE_ENUM(bold_style),
)
/* KiTTY (TuTTY): enable flags for the extra colour slots. bold_colour mirrors
 * stock bold-as-colour; under_colour colours underlined text with CONF_COLOUR_under_fg;
 * sel_colour (rendering deferred) would colour the selection. */
CONF_OPTION(bold_colour, VALUE_TYPE(INT), DEFAULT_INT(1), SAVE_KEYWORD("BoldAsColourTest"),)
CONF_OPTION(under_colour, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("UnderlinedAsColour"),)
CONF_OPTION(sel_colour, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("SelectedAsColour"),)
/* KiTTY (far2l): far2l terminal-extension clipboard sync. 0=disabled, 1=enabled,
 * 2=ask (SHARED_CLIPBOARD_* in putty.h). Default ask, so it works but prompts. */
CONF_OPTION(shared_clipboard, VALUE_TYPE(INT), DEFAULT_INT(2), SAVE_KEYWORD("SharedClipboard"),)
CONF_OPTION(colours,
    /*
     * Subkeys in this setting are indexed based on the CONF_COLOUR_*
     * enum values in putty.h. But each subkey identifies just one
     * component of the RGB value. Subkey 3*a+b identifies colour #a,
     * channel #b, where channels 0,1,2 mean R,G,B respectively.
     *
     * Values are 8-bit integers.
     */
    SUBKEY_TYPE(INT),
    VALUE_TYPE(INT),
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for mappings */
)

/* Selection options */
CONF_OPTION(mouse_is_xterm,
    VALUE_TYPE(INT),
    DEFAULT_INT(0),
    SAVE_KEYWORD("MouseIsXterm"),
    STORAGE_ENUM(mouse_buttons),
)
CONF_OPTION(rect_select,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("RectSelect"),
)
CONF_OPTION(paste_controls,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("PasteControls"),
)
CONF_OPTION(rawcnp,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("RawCNP"),
)
CONF_OPTION(utf8linedraw,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("UTF8linedraw"),
)
CONF_OPTION(rtf_paste,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("PasteRTF"),
)
CONF_OPTION(mouse_override,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("MouseOverride"),
)
CONF_OPTION(wordness,
    SUBKEY_TYPE(INT), /* ASCII character codes (literally, just 00-7F) */
    VALUE_TYPE(INT),  /* arbitrary equivalence-class value for that char */
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for mappings */
)
CONF_OPTION(mouseautocopy,
    /*
     * What clipboard (if any) to copy text to as soon as it's
     * selected with the mouse.
     */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(CLIPUI_DEFAULT_AUTOCOPY), /* platform-dependent bool-valued
                                            * macro */
    SAVE_KEYWORD("MouseAutocopy"),
)
CONF_OPTION(mousepaste, /* clipboard used by one-mouse-click paste actions */
    VALUE_TYPE(INT),
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * SAVE_KEYWORD("MousePaste"), but loading and saving is done by
     * custom code, because the saved value is a string, and also sets
     * CONF_mousepaste_custom
     */
)
CONF_OPTION(ctrlshiftins, /* clipboard used by Ctrl+Ins and Shift+Ins */
    VALUE_TYPE(INT),
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * SAVE_KEYWORD("CtrlShiftIns"), but loading and saving is done by
     * custom code, because the saved value is a string, and also sets
     * CONF_ctrlshiftins_custom
     */
)
CONF_OPTION(ctrlshiftcv, /* clipboard used by Ctrl+Shift+C and Ctrl+Shift+V */
    VALUE_TYPE(INT),
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * SAVE_KEYWORD("CtrlShiftCV"), but loading and saving is done by
     * custom code, because the saved value is a string, and also sets
     * CONF_ctrlshiftcv_custom
     */
)
CONF_OPTION(mousepaste_custom,
    /* Custom clipboard name if CONF_mousepaste is set to CLIPUI_CUSTOM */
    VALUE_TYPE(STR),
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * Loading and saving is handled by custom code in conjunction
     * with CONF_mousepaste
     */
)
CONF_OPTION(ctrlshiftins_custom,
    /* Custom clipboard name if CONF_ctrlshiftins is set to CLIPUI_CUSTOM */
    VALUE_TYPE(STR),
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * Loading and saving is handled by custom code in conjunction
     * with CONF_ctrlshiftins
     */
)
CONF_OPTION(ctrlshiftcv_custom,
    /* Custom clipboard name if CONF_ctrlshiftcv is set to CLIPUI_CUSTOM */
    VALUE_TYPE(STR),
    LOAD_CUSTOM, SAVE_CUSTOM,
    /*
     * Loading and saving is handled by custom code in conjunction
     * with CONF_ctrlshiftcv
     */
)

/* Character-set translation */
CONF_OPTION(vtmode,
    VALUE_TYPE(INT),
    DEFAULT_INT(VT_UNICODE),
    SAVE_KEYWORD("FontVTMode"),
    STORAGE_ENUM(line_drawing),
)
CONF_OPTION(line_codepage,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("LineCodePage"),
)
CONF_OPTION(cjk_ambig_wide,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("CJKAmbigWide"),
)
CONF_OPTION(utf8_override,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("UTF8Override"),
)
CONF_OPTION(xlat_capslockcyr,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("CapsLockCyr"),
)

/* X11 forwarding */
CONF_OPTION(x11_forward,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("X11Forward"),
)
CONF_OPTION(x11_display,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("X11Display"),
)
CONF_OPTION(x11_auth,
    VALUE_TYPE(INT),
    DEFAULT_INT(X11_MIT),
    SAVE_KEYWORD("X11AuthType"),
    STORAGE_ENUM(x11_auth),
)
CONF_OPTION(xauthfile,
    VALUE_TYPE(FILENAME),
    SAVE_KEYWORD("X11AuthFile"),
)

/* Port forwarding */
CONF_OPTION(lport_acceptall, /* accept conns from hosts other than localhost */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("LocalPortAcceptAll"),
)
CONF_OPTION(rport_acceptall, /* same for remote forwarded ports */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("RemotePortAcceptAll"),
)
CONF_OPTION(portfwd,
    /*
     * Subkeys for 'portfwd' can have the following forms:
     *
     *   [LR]localport
     *   [LR]localaddr:localport
     *
     * Dynamic forwardings are indicated by an 'L' key, and the
     * special value "D". For all other forwardings, the value should
     * be of the form 'host:port'.
     */
    SUBKEY_TYPE(STR),
    VALUE_TYPE(STR),
    LOAD_CUSTOM, SAVE_CUSTOM, /* necessary for mappings */
)

/* SSH bug compatibility modes. All FORCE_ON/FORCE_OFF/AUTO */
CONF_OPTION(sshbug_ignore1,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugIgnore1"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_plainpw1,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugPlainPW1"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_rsa1,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugRSA1"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_ignore2,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugIgnore2"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_derivekey2,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugDeriveKey2"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_rsapad2,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugRSAPad2"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_pksessid2,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugPKSessID2"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_rekey2,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugRekey2"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_maxpkt2,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugMaxPkt2"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_oldgex2,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugOldGex2"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_winadj,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugWinadj"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_chanreq,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugChanReq"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_dropstart,
    VALUE_TYPE(INT),
    DEFAULT_INT(FORCE_OFF),
    SAVE_KEYWORD("BugDropStart"),
    STORAGE_ENUM(off1_on2),
)
CONF_OPTION(sshbug_filter_kexinit,
    VALUE_TYPE(INT),
    DEFAULT_INT(FORCE_OFF),
    SAVE_KEYWORD("BugFilterKexinit"),
    STORAGE_ENUM(off1_on2),
)
CONF_OPTION(sshbug_rsa_sha2_cert_userauth,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugRSASHA2CertUserauth"),
    STORAGE_ENUM(auto_off_on),
)
CONF_OPTION(sshbug_hmac2,
    VALUE_TYPE(INT),
    DEFAULT_INT(AUTO),
    SAVE_KEYWORD("BugHMAC2"),
    STORAGE_ENUM(auto_off_on),
    LOAD_CUSTOM, /* there was an earlier keyword called "BuggyMAC" */
)

/* Options for Unix. Should split out into platform-dependent part. */
CONF_OPTION(stamp_utmp, /* used by Unix pterm */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("StampUtmp"),
)
CONF_OPTION(login_shell, /* used by Unix pterm */
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(true),
    SAVE_KEYWORD("LoginShell"),
)
CONF_OPTION(scrollbar_on_left,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ScrollbarOnLeft"),
)
CONF_OPTION(shadowbold,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("ShadowBold"),
)
CONF_OPTION(boldfont,
    VALUE_TYPE(FONT),
    SAVE_KEYWORD("BoldFont"),
)
CONF_OPTION(widefont,
    VALUE_TYPE(FONT),
    SAVE_KEYWORD("WideFont"),
)
CONF_OPTION(wideboldfont,
    VALUE_TYPE(FONT),
    SAVE_KEYWORD("WideBoldFont"),
)
CONF_OPTION(shadowboldoffset,
    VALUE_TYPE(INT), /* in pixels */
    DEFAULT_INT(1),
    SAVE_KEYWORD("ShadowBoldOffset"),
)
CONF_OPTION(crhaslf,
    VALUE_TYPE(BOOL),
    DEFAULT_BOOL(false),
    SAVE_KEYWORD("CRImpliesLF"),
)
CONF_OPTION(winclass,
    VALUE_TYPE(STR),
    DEFAULT_STR(""),
    SAVE_KEYWORD("WindowClass"),
)

/* ===== KiTTY (MOD_PERSO) configuration options — foundational set ported to 0.84 ===== */
CONF_OPTION(autocommand, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("Autocommand"),)
CONF_OPTION(bg_image_abs_fixed, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("BgImagePlacement"),)
CONF_OPTION(bg_image_abs_x, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("BgImageAbsoluteX"),)
CONF_OPTION(bg_image_abs_y, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("BgImageAbsoluteY"),)
CONF_OPTION(bg_image_filename, VALUE_TYPE(FILENAME), SAVE_KEYWORD("BgImageFile"),)
CONF_OPTION(bg_image_style, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("BgImageStyle"),)
CONF_OPTION(bg_opacity, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("BgOpacity"),)
CONF_OPTION(bg_slideshow, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("BgSlideshow"),)
CONF_OPTION(bg_type, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("BgType"),)
CONF_OPTION(folder, VALUE_TYPE(STR), DEFAULT_STR("Default"), SAVE_KEYWORD("Folder"),)
CONF_OPTION(icone, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("Icone"),)
CONF_OPTION(iconefile, VALUE_TYPE(FILENAME), SAVE_KEYWORD("IconeFile"),)
CONF_OPTION(password, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("Password"),)
CONF_OPTION(portknockingoptions, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("PortKnocking"),)
/* Flags handed to kscp. "-r" so that dropping a FOLDER on a terminal uploads it,
 * which is what the configuration panel has always told the user the default is
 * - and what the .ktx loader supplied, while every ordinary session got nothing
 * and the upload of a folder failed. */
CONF_OPTION(pscpoptions, VALUE_TYPE(STR), DEFAULT_STR("-r"), SAVE_KEYWORD("PSCPOptions"),)
CONF_OPTION(pscpremotedir, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("PSCPRemoteDir"),)
CONF_OPTION(pscpshell, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("PSCPShell"),)
CONF_OPTION(saveonexit, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("SaveOnExit"),)
CONF_OPTION(rzcommand, VALUE_TYPE(FILENAME), SAVE_KEYWORD("rzCommand"),)
CONF_OPTION(rzoptions, VALUE_TYPE(STR), DEFAULT_STR("-e -v"), SAVE_KEYWORD("rzOptions"),)
CONF_OPTION(szcommand, VALUE_TYPE(FILENAME), SAVE_KEYWORD("szCommand"),)
CONF_OPTION(szoptions, VALUE_TYPE(STR), DEFAULT_STR("-e -v"), SAVE_KEYWORD("szOptions"),)
CONF_OPTION(zdownloaddir, VALUE_TYPE(STR), DEFAULT_STR("C:\\"), SAVE_KEYWORD("zDownloadDir"),)
CONF_OPTION(scp_auto_pwd, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("SCPAutoPwd"),)
CONF_OPTION(osc7_cwd_tracking, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("OSC7CwdTracking"),)
CONF_OPTION(pscp_keep_window, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("PSCPKeepWindow"),)
CONF_OPTION(runcmdconfirm, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("RunCmdConfirm"),)
CONF_OPTION(runcmdnotify, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("RunCmdNotify"),)
CONF_OPTION(scriptfilecontent, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("ScriptfileContent"),)
CONF_OPTION(sessionname, VALUE_TYPE(STR), DEFAULT_STR(""), NOT_SAVED,)
CONF_OPTION(sftpconnect, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("SFTPConnect"),)
CONF_OPTION(transparencynumber, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("TransparencyValue"),)
CONF_OPTION(url_browser, VALUE_TYPE(FILENAME), SAVE_KEYWORD("HyperlinkBrowser"),)
CONF_OPTION(url_defbrowser, VALUE_TYPE(INT), DEFAULT_INT(1), SAVE_KEYWORD("HyperlinkBrowserUseDefault"),)
CONF_OPTION(url_defregex, VALUE_TYPE(INT), DEFAULT_INT(1), SAVE_KEYWORD("HyperlinkRegularExpressionUseDefault"),)
CONF_OPTION(url_regex, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("HyperlinkRegularExpression"),)
CONF_OPTION(url_underline, VALUE_TYPE(INT), DEFAULT_INT(1), SAVE_KEYWORD("HyperlinkUnderline"),)
CONF_OPTION(url_hover_cursor, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("HyperlinkHoverCursor"),)
CONF_OPTION(url_ctrl_click, VALUE_TYPE(INT), DEFAULT_INT(1), SAVE_KEYWORD("HyperlinkUseCtrlClick"),)
CONF_OPTION(windowstate, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("WindowState"),)
CONF_OPTION(winscpoptions, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("WinSCPOptions"),)
/* 0=scp 1=sftp 2=ftp 3=ftps 4=ftpes 5=http 6=https. Defaults to SFTP: OpenSSH
 * deprecated the legacy SCP protocol, and its own scp(1) has spoken SFTP
 * underneath since 9.0, because the old one had the remote SHELL expand paths -
 * which is where a decade of quoting and path-traversal bugs came from. Plenty
 * of hardened servers now offer only the sftp subsystem. SCP stays selectable
 * for embedded gear that has no sftp-server.
 * ⚠️ This one value drives BOTH kscp's -scp/-sftp flag and WinSCP's URL scheme
 * (kitty/kitty_xfer.c). Splitting it per tool is part of the file-transfer panel
 * rework. */
CONF_OPTION(winscpprot, VALUE_TYPE(INT), DEFAULT_INT(1), SAVE_KEYWORD("WinSCPProtocol"),)
CONF_OPTION(winscprawsettings, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("WinSCPRawSettings"),)
CONF_OPTION(xpos, VALUE_TYPE(INT), DEFAULT_INT(-1), SAVE_KEYWORD("TermXPos"),)
CONF_OPTION(ypos, VALUE_TYPE(INT), DEFAULT_INT(-1), SAVE_KEYWORD("TermYPos"),)
CONF_OPTION(maximize, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("Maximize"),)
CONF_OPTION(fullscreen, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("Fullscreen"),)
CONF_OPTION(sendtotray, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("SendToTray"),)
/* ===== KiTTY (MOD_PERSO) — previously NOTPORTED keys, now added ===== */
CONF_OPTION(enter_sends_crlf, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("EnterSendsCrLf"),)
CONF_OPTION(host_alt, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("HostAlt"),)
CONF_OPTION(scriptfile, VALUE_TYPE(FILENAME), SAVE_KEYWORD("Scriptfile"),)
CONF_OPTION(antiidle, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("AntiIdle"),)
CONF_OPTION(wakeup_reconnect, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("WakeupReconnect"),)
CONF_OPTION(failure_reconnect, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("FailureReconnect"),)
CONF_OPTION(logtimestamp, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("LogTimestamp"),)
CONF_OPTION(autocommandout, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("AutocommandOut"),)
CONF_OPTION(logtimerotation, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("LogTimeRotation"),)
/* KiTTY: pins the window to CONF_xpos/ypos - it sets a position, it saves
 * none. Stored as "SaveWindowPos" up to 0.84.1.67; a session that still has
 * the old name is read through it (windows/storage.c, kitty_retired_keys),
 * and the stale key is dropped the next time that session is saved. Carrying
 * the flag across cannot strand a window: a pin is applied only for x >= 0 and
 * y >= 0, and is clamped onto the nearest monitor (kitty_apply_window_pos). */
CONF_OPTION(set_windowpos, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("SetWindowPos"),)
CONF_OPTION(foreground_on_bell, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("ForegroundOnBell"),)
CONF_OPTION(ctrl_tab_switch, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("CtrlTabSwitch"),)
CONF_OPTION(comment, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("Comment"),)
/* KiTTY: print the Comment into the terminal once the session is up. */
CONF_OPTION(comment_notify, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("CommentNotify"),)
CONF_OPTION(launcherhide, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("LauncherHide"),) /* KiTTY: exclude from kitty -launcher */
/* KiTTY (classic parity): which of the window's own buttons exist.
 *
 * All default TRUE, i.e. an ordinary window - turning any of them off is the
 * unusual case. This is a kiosk / embedding feature: KiTTY hosted inside another
 * application (mRemoteNG puts it in a tab) has no use for a Close button that
 * would strand the host, and a window nobody may minimise is the point of a kiosk.
 *
 * ⚠️ WindowHasSysMenu is not one of four equals - Windows will not draw ANY caption
 * button without WS_SYSMENU, so turning it off removes close, minimise and
 * maximise whatever those three say. Classic KiTTY greyed the other three boxes in
 * its dialog to show that; this port has no dlg_enable() to grey a control with, so
 * the labels say it instead. The behaviour is identical either way, because it is
 * Windows enforcing it and not us.
 *
 * Closing is disabled by greying SC_CLOSE on the system menu rather than by
 * dropping a style bit, which is what also greys the X and disables Alt+F4. */
CONF_OPTION(window_has_sysmenu, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("WindowHasSysMenu"),)
CONF_OPTION(window_closable, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("WindowClosable"),)
CONF_OPTION(window_minimizable, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("WindowMinimizable"),)
CONF_OPTION(window_maximizable, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("WindowMaximizable"),)
CONF_OPTION(no_focus_rep, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("NoFocusReporting"),)
CONF_OPTION(scrolllines, VALUE_TYPE(INT), DEFAULT_INT(-1), SAVE_KEYWORD("LinesAtAScroll"),)
CONF_OPTION(ssh_tunnel_print_in_title, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("SSHTunnelInTitle"),)
CONF_OPTION(disablealtgr, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("DisableAltGr"),)
CONF_OPTION(printclip, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("PrintToClipboard"),)
/* KiTTY: may a remote host put text on the local clipboard with OSC 52?
 * 0=deny, 1=allow, 2=ask-once-per-session (OSC52_CLIPBOARD_* in putty.h).
 *
 * Default ASK (changed on a review of the defaults, 2026-08-05). That is one step
 * stricter than the comparable terminals: Ghostty permits OSC 52 writes
 * unconditionally, Alacritty ships "OnlyCopy" (write yes, read no), kitty writes
 * by default. This shipped as ALLOW for exactly that reason.
 *
 * What ALLOW missed: "the write direction leaks nothing" is true, and is why the
 * read direction is treated far more harshly - but a write is not harmless
 * either. A host that silently replaces your clipboard chooses what you paste
 * NEXT, possibly into a root shell. Nothing leaves the machine, and something can
 * still arrive on it.
 *
 * ASK is affordable here precisely because it is NOT per-request: the answer
 * latches for the rest of the session, so tmux or neovim costs one dialog on the
 * first copy and nothing afterwards. That keeps it clear of the failure mode the
 * read direction has to design around - measured elsewhere, not assumed: kitty's
 * own users report its clipboard warnings are "so annoying that everyone will
 * look for a fix and disable" them, and Ghostty has bug reports of read prompts
 * firing repeatedly from nothing worse than Neovim polling the clipboard over
 * SSH. A once-per-session question is not that.
 *
 * Set it to Allow per session (or in Default Settings) for the old behaviour, or
 * to Deny if you would rather no host touched the clipboard at all.
 * Replaces the BOOL "OSC52WarnBeforeClipboardSync", which this port loaded and
 * saved but never read, because OSC 52 itself was never ported. That key is NOT
 * migrated on purpose: it meant "warn", so its default false meant "sync
 * silently", and honouring it would quietly switch remote clipboard writes ON
 * for every session imported from classic KiTTY. It is dropped when a session is
 * next saved (windows/storage.c, kitty_retired_keys). */
CONF_OPTION(osc52_clipboard, VALUE_TYPE(INT), DEFAULT_INT(2), SAVE_KEYWORD("OSC52Clipboard"),)
/* KiTTY: may a remote host ask for the CONTENTS of the local clipboard and have
 * them sent back? 0=deny, 1=ask (OSC52_READ_* in putty.h). Default DENY.
 *
 * This is the opposite direction from OSC52Clipboard above and it is not the same
 * kind of question. A write changes what you paste next; a read hands the host
 * whatever is on your clipboard, which is a password often enough to matter, and
 * the host picks the moment - typically just after you pasted something into it,
 * because then it knows there is something worth taking.
 *
 * There is no "allow" value, here or in kitty.ini, on purpose: see OSC52_READ_*.
 * Everything about how long a granted read lasts is in the OSC52Read* keys. */
CONF_OPTION(osc52_clipboard_read, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("OSC52ClipboardRead"),)
/* KiTTY: require the window to have keyboard focus before ANY remote clipboard
 * activity at all - reads AND writes, over OSC 52, OSC 5522 AND far2l. Default on.
 *
 * "KiTTY never touches your clipboard unless you are looking at that window" is
 * short enough to hold in your head, which is most of its value - and a rule with
 * an exception in it is not that sentence any more, which is why far2l is included
 * rather than left as the one protocol that ignores it. It also kills the failure
 * mode Ghostty hit: an editor polling the clipboard over SSH raising dialogs on a
 * window nobody is looking at. A grant is SUSPENDED while focus is elsewhere, not
 * cancelled - it resumes without asking again when you come back.
 *
 * It is a setting rather than a hard rule only because it changes behaviour that
 * shipped working: a background job that copies its own output stops working while
 * you are in another window, and someone who relies on that needs a way back. The
 * default enforces the rule.
 *
 * Named Clipboard* rather than OSC52* because it governs far2l too; a key called
 * OSC52RequireFocus that silently also gated far2l would be a lie. */
CONF_OPTION(clipboard_require_focus, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("ClipboardRequireFocus"),)
/* KiTTY: how long the dialog's "the next N minutes" grant lasts. Minutes. */
CONF_OPTION(osc52_read_minutes, VALUE_TYPE(INT), DEFAULT_INT(10), SAVE_KEYWORD("OSC52ReadMinutes"),)
/* KiTTY: how many requests the dialog's "the next N requests" grant covers. */
CONF_OPTION(osc52_read_requests, VALUE_TYPE(INT), DEFAULT_INT(25), SAVE_KEYWORD("OSC52ReadRequests"),)
/* KiTTY: shortest gap, in seconds, between two clipboard hand-overs. 0 = no
 * limit.
 *
 * This is the one that stops a grant being turned against the user, and it is
 * separate from the limit on dialogs. Allow a host for the rest of the session
 * and it does not receive one clipboard: asking every two seconds for an hour, it
 * receives EVERYTHING copied during that hour, including whatever a password
 * manager put there in between. Exceeding this drops the grant and goes back to
 * asking, rather than quietly continuing. No other terminal appears to implement
 * such a limit - kitty and Ghostty spend their effort on per-program permission
 * instead - so the number is ours and is a guess worth revisiting. */
CONF_OPTION(osc52_read_interval, VALUE_TYPE(INT), DEFAULT_INT(2), SAVE_KEYWORD("OSC52ReadInterval"),)
/* KiTTY: most clipboard hand-overs served in one window, ever. 0 = no ceiling.
 * The whole-session backstop for the same problem as OSC52ReadInterval. */
CONF_OPTION(osc52_read_max, VALUE_TYPE(INT), DEFAULT_INT(200), SAVE_KEYWORD("OSC52ReadMax"),)
/* KiTTY: seconds before an unanswered clipboard-read dialog gives up. 0 = wait
 * for ever. A timeout is NOT a decision: the request is refused and nothing is
 * remembered either way, so the next request asks again. Anything else would
 * record a choice the user never made. */
CONF_OPTION(osc52_read_timeout, VALUE_TYPE(INT), DEFAULT_INT(60), SAVE_KEYWORD("OSC52ReadTimeout"),)
/* KiTTY: most dialogs shown in any ten seconds, so a host cannot use the prompt
 * itself as the attack. Extras are refused without asking. */
CONF_OPTION(osc52_read_dialogs, VALUE_TYPE(INT), DEFAULT_INT(3), SAVE_KEYWORD("OSC52ReadDialogs"),)
/* KiTTY: most remote clipboard WRITES applied in any one second. 0 = no limit.
 * Default 10. Covers OSC 52 and far2l.
 *
 * A RATE CAP rather than a minimum gap between writes, and the difference matters.
 * A minimum gap means the FIRST write in a burst wins and the rest are dropped, so
 * a script that copies three things in quick succession leaves you holding the
 * first - a stale value, silently. That is the wrong way round for a clipboard,
 * where the whole convention is that the last write wins. A cap high enough to
 * clear any realistic burst keeps last-wins intact for every legitimate case and
 * still bounds the abusive one.
 *
 * The write direction had no rate limit at all, and it is the one that is ON BY
 * DEFAULT - reads default to Deny, writes to Allow - so the unprotected path was
 * the one every user has. A host could call SetClipboardData as fast as it could
 * send sequences, which destroys whatever you copied, floods the Win+V history so
 * your real entries fall off it, hammers any clipboard manager watching, and in
 * the targeted version overwrites your clipboard at the moment you are about to
 * paste.
 *
 * Ten per second because no human workflow produces more, while a bomb produces
 * thousands - so the cap separates them without having to guess at intent.
 *
 * Exceeding it drops the write and says so - it does NOT withdraw permission, and
 * that is the deliberate difference from the read side. Tripping a read limit is
 * evidence of harvesting and withdrawing is proportionate; a write burst is far
 * more likely to be an ordinary script, and revoking would break the normal case
 * to punish it. There is no per-window total for writes either: a total bounds
 * cumulative disclosure, and writes disclose nothing.
 *
 * Honest limit: even at ten per second a hostile host can still stamp on your
 * clipboard for ever. No cap fixes that - the answer there is setting writes to
 * Deny, which is exactly what clicking the notification offers. What this turns
 * off is "thousands per second, invisibly". */
CONF_OPTION(clipboard_writes_per_sec, VALUE_TYPE(INT), DEFAULT_INT(10), SAVE_KEYWORD("ClipboardWritesPerSecond"),)
/* KiTTY: largest single remote-clipboard payload we will hold, in megabytes.
 * Applies to OSC 52 and to far2l alike; default 16 (lowered from 64 on a review
 * of the defaults, 2026-08-05).
 *
 * Sized for an image rather than a line of text, because far2l carries arbitrary
 * Windows clipboard formats. 64 came from the worst case anyone could name - an
 * uncompressed 4K CF_DIB is ~33 MB raw, ~44 MB once base64'd - and defaulting to
 * the worst case is the wrong way round: it is a ceiling a HOSTILE host reaches
 * every time and a real one reaches almost never. 16 MB still carries a 1080p DIB
 * (~8 MB raw, ~11 MB encoded) and every text payload by three orders of magnitude,
 * while cutting what an attacker can make a window hold to a quarter. Somebody who
 * really does copy 4K screenshots raises it and knows why they are doing it.
 *
 * One number for both protocols on purpose: the old split (16 MB for OSC 52, 64
 * for far2l) only ever meant "text does not need as much", which is not a security
 * argument, since the bound a hostile host can reach is the same either way.
 *
 * ⚠️ This is a memory-exhaustion backstop, not a feature limit. A host that opens
 * a sequence and never terminates it can make one window hold this much with no
 * user interaction, and the decode transiently costs about the same again. So it
 * is CLAMPED in code (see clip_ceiling_bytes in terminal.c): a user cannot turn a
 * bounded denial of service into an unbounded one by typing a large number here.
 * Lowering it is the more interesting direction - somebody who never copies images
 * can shrink what a hostile host can make them hold.
 *
 * It does NOT govern the 2 KB ceiling on ordinary escape sequences. That one stays
 * fixed and unsettable: it is what stops the clipboard feature being used to hand
 * us a multi-megabyte window title. */
CONF_OPTION(clipboard_max_mb, VALUE_TYPE(INT), DEFAULT_INT(16), SAVE_KEYWORD("ClipboardMaxMB"),)
/* KiTTY: show a tray balloon for remote-clipboard events - a permission granted or
 * expired, a request refused, or a payload dropped for being too large. Covers
 * OSC 52, OSC 5522 and far2l alike. Default on.
 *
 * It carries the meaning when the title bar cannot - full screen, or decorations
 * off. Two rules keep it from becoming something a host can pull at will:
 *  - it only speaks for a feature the user actually enabled. Someone who set a
 *    policy to Deny has already answered and does not need telling again, though
 *    the Event Log records it either way;
 *  - it is rate-limited (CLIP_NOTIFY_GAP in terminal.c), because every one of
 *    these events fires at a moment the REMOTE HOST chose.
 *
 * The dropped-payload case is the one that earns this its keep: a silent refusal
 * is indistinguishable from a broken feature, which is exactly how far2l's 2 KB
 * cap went unnoticed for so long. */
CONF_OPTION(clipboard_notify, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("ClipboardNotify"),)
/* KiTTY: append a "clip read"/"clip write" marker to the window title while a
 * clipboard permission is live. Default on. */
CONF_OPTION(osc52_title_mark, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("OSC52TitleMark"),)
/* KiTTY: also mark a STANDING permission - writes left on "Allow" - and not only
 * one granted in the moment. Default OFF.
 *
 * Off by default because writes ship as Allow, so this would put a marker in the
 * title of every session for ever, and a marker that is always on is one nobody
 * reads within a day.
 *
 * On for anyone who wants the title to be a complete statement. Without it, "no
 * marker" means "no permission granted in the moment", NOT "no remote clipboard
 * access is possible" - which is exactly true for reads, since they can never be
 * standing, and not true for writes. */
CONF_OPTION(clipboard_mark_always, VALUE_TYPE(BOOL), DEFAULT_BOOL(false), SAVE_KEYWORD("ClipboardMarkAlways"),)
/* KiTTY: show a marker when the host actually READS or WRITES the clipboard, as
 * against merely having permission to. Default on.
 *
 * This is a different question from the permission markers above, and the more
 * useful one day to day: permission says what COULD happen, activity says what
 * DID. A clipboard the host never touches and one it reads every thirty seconds
 * look identical without it.
 *
 * The marker goes at the FRONT of the title, unlike the permission markers, and
 * that is deliberate: it is transient and meant to catch the eye, whereas those
 * are standing state that must not push the connection name out of a truncated
 * taskbar entry. It clears itself after ClipboardActivitySeconds. */
CONF_OPTION(clipboard_activity_mark, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("ClipboardActivityMark"),)
/* KiTTY: how long that activity marker stays up, in seconds. Default 5.
 * Repeated activity extends it rather than repainting, so a host writing at the
 * permitted rate cannot make the title flicker. */
CONF_OPTION(clipboard_activity_secs, VALUE_TYPE(INT), DEFAULT_INT(5), SAVE_KEYWORD("ClipboardActivitySeconds"),)
/* KiTTY: tint the title bar and window border while a clipboard permission is
 * live. Default on, but Windows 11 build 22000+ only - on Windows 10 there is no
 * supported way for an application to colour either, so the title marker has to
 * carry the meaning by itself and this silently does nothing. */
CONF_OPTION(osc52_colour_frame, VALUE_TYPE(BOOL), DEFAULT_BOOL(true), SAVE_KEYWORD("OSC52ColourFrame"),)
/* ===== KiTTY rutty scripting (script.c) ===== */
CONF_OPTION(script_mode, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("ScriptMode"),)
CONF_OPTION(script_line_delay, VALUE_TYPE(INT), DEFAULT_INT(5), SAVE_KEYWORD("ScriptLineDelay"),)
CONF_OPTION(script_char_delay, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("ScriptCharDelay"),)
CONF_OPTION(script_cond_line, VALUE_TYPE(STR), DEFAULT_STR(":"), SAVE_KEYWORD("ScriptCondLine"),)
CONF_OPTION(script_cond_use, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("ScriptCondUse"),)
CONF_OPTION(script_crlf, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("ScriptCRLF"),)
CONF_OPTION(script_enable, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("ScriptEnable"),)
CONF_OPTION(script_except, VALUE_TYPE(INT), DEFAULT_INT(0), SAVE_KEYWORD("ScriptExcept"),)
CONF_OPTION(script_timeout, VALUE_TYPE(INT), DEFAULT_INT(15), SAVE_KEYWORD("ScriptTimeout"),)
CONF_OPTION(script_waitfor, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("ScriptWait"),)
CONF_OPTION(script_halton, VALUE_TYPE(STR), DEFAULT_STR(""), SAVE_KEYWORD("ScriptHalt"),)
