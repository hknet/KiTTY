/*
 * Constant definitions for the Pageant resource file.
 */

#define IDI_MAINICON 200
#define IDI_TRAYICON 201

#define IDD_KEYLIST 211
#define IDD_LOAD_PASSPHRASE 210
#define IDD_ONDEMAND_PASSPHRASE 212
#define IDD_ABOUT 213
#define IDD_LICENCE 214
#define IDD_KEYDETAILS 215     /* KiTTY: key details (copyable fields) */
#define IDD_KEYSETTINGS 216    /* KiTTY: the [Agent] settings dialog */
#define IDD_AUDITVIEW 217      /* KiTTY: the audit-log viewer */
#define IDD_AUDITDETAIL 218    /* KiTTY: one audit record, field per line */
#define IDD_HELLOPROTECT 219   /* KiTTY: protect a key with Windows Hello */

/* KiTTY: the audit-log viewer's controls */
#define IDC_AUDIT_FILTER 400
#define IDC_AUDIT_ENABLE 401
#define IDC_AUDIT_REFRESH 402
#define IDC_AUDIT_LIST 403
#define IDC_AUDIT_PATH 404
#define IDC_AUDIT_DETAIL 405   /* the selected row's FULL raw line */
#define IDC_AUDIT_REQFILTER 406  /* slice to one client app */
#define IDC_AUDIT_FILTER_LBL 407
#define IDC_AUDIT_APP_LBL 408
#define IDC_AUDITDETAIL_TEXT 410

/* KiTTY: the settings dialog */
#define IDC_SET_OPENSSH 300
#define IDC_SET_STARTUP 301
#define IDC_SET_NOTIFY 302
#define IDC_SET_RETRY 303
#define IDC_SET_UNLOAD 304
#define IDC_SET_QUIET 305
#define IDC_SET_TTL 306
#define IDC_SET_NOTICESECS 307   /* notice display seconds */
#define IDC_SET_LOCKDOWN 308      /* IPC: refuse all add/remove */
#define IDC_SET_BLOCKADD 309      /* IPC: refuse adds */
#define IDC_SET_BLOCKREMOVE 310   /* IPC: refuse removes */
#define IDC_SET_LOADKEYS 311      /* re-add remembered keys at startup */
#define IDC_SET_HELLO 312         /* confirmations require Windows Hello */
/* KiTTY: the agent-log group */
#define IDC_SET_AGENTLOG 313      /* write the agent log */
#define IDC_SET_AGENTLOGPATH 314  /* path override, blank = default */
#define IDC_SET_AGENTLOGKB 315    /* rotate at (KB) */
#define IDC_SET_AGENTLOGKEEP 316  /* rotated generations */
#define IDC_SET_AGENTLOGDAYS 317  /* expunge after (days) */
#define IDC_SET_HELLOTTL 318      /* Hello KEK cache seconds */
#define IDC_SET_TABS 319          /* the settings dialog's tab strip */
#define IDC_SET_THEME 320         /* colour theme: system / light / dark */
/*
 * Every static on this dialog needs an ID of its own now. The pages share one
 * flat template and are shown and hidden a page at a time, and a control can
 * only be hidden if it can be named - the shared IDC_STATIC (-1) cannot.
 */
#define IDC_SET_L_NOTICE 330
#define IDC_SET_L_NOTICEHINT 331
#define IDC_SET_L_THEME 332
#define IDC_SET_L_THEMEHINT 333
#define IDC_SET_L_TTL 334
#define IDC_SET_L_TTLHINT 335
#define IDC_SET_L_HELLOTTL 336
#define IDC_SET_L_HELLOTTLHINT 337
#define IDC_SET_AUTOENCMODE 345   /* re-encrypt after idle: Off / Default / Enforced */
#define IDC_SET_AUTOENC 346       /* ... and the time */
#define IDC_SET_L_AUTOENC 347
#define IDC_SET_L_AUTOENCHINT 348
#define IDC_SET_L_RETRY 338
#define IDC_SET_L_LOGPATH 339
#define IDC_SET_L_LOGKB 340
#define IDC_SET_L_LOGKEEP 341
#define IDC_SET_L_LOGDAYS 342
#define IDC_SET_L_LOGNOTE 343
#define IDC_SET_L_LOGDEFAULT 344  /* the resolved path a blank File box means */

/* KiTTY: the key-details dialog */
#define IDC_KEYDETAIL_KEY 100
#define IDC_KEYDETAIL_STATE 101
#define IDC_KEYDETAIL_FPS 102
#define IDC_KEYDETAIL_COMMENT 103
#define IDC_KEYDETAIL_PATHS 104
#define IDC_KEYDETAIL_LOADNOW 105
#define IDC_KEYDETAIL_DEFER 106
#define IDC_KEYDETAIL_LIFETIME 107
#define IDC_KEYDETAIL_LIFETIME_LBL 108
#define IDC_KEYDETAIL_CONFIRM 109
#define IDC_KEYDETAIL_ACCEPT 110    /* adopt a changed key file (mismatch rows) */
#define IDC_KEYDETAIL_LOCATE 111    /* re-point an absent/unparseable entry */
#define IDC_KEYDETAIL_CONFIRM_LBL 112  /* label of the confirm-mode droplist */
#define IDC_KEYDETAIL_PROTECT 113   /* protect with Windows Hello (a COPY) */
#define IDC_KEYDETAIL_FORGET 114    /* forget one source path of this key */
#define IDC_KEYDETAIL_AUTOENC 115   /* re-encrypt after idle: this key's own value */
#define IDC_KEYDETAIL_AUTOENC_LBL 116
#define IDC_KEYDETAIL_AUTOENC_NOTE 117 /* agent default / enforced line */

/* KiTTY: the Hello-protect dialog */
#define IDC_HP_SRC 100
#define IDC_HP_DEST 101
#define IDC_HP_BROWSE 102
#define IDC_HP_OPENFOLDER 103
#define IDC_HP_SRCPASS_LBL 104
#define IDC_HP_SRCPASS 105
#define IDC_HP_USESRCPASS 106
#define IDC_HP_RECPASS_LBL 107
#define IDC_HP_RECPASS 108
#define IDC_HP_RECPASS2_LBL 109
#define IDC_HP_RECPASS2 110
#define IDC_HP_REPLACE 111
#define IDC_HP_WARN 112
#define IDC_HP_SIDEBOUND 114   /* printout = code bound to the sidecar */
#define IDC_HP_HELLOONLY 113

/* KiTTY: the printed-secret dialog */

#define IDC_PASSPHRASE_STATIC1 100
#define IDC_PASSPHRASE_FINGERPRINT 101
#define IDC_PASSPHRASE_STATIC2 102
#define IDC_PASSPHRASE_STATIC3 103
#define IDC_PASSPHRASE_EDITBOX 104

#define IDC_KEYLIST_LISTBOX 100
#define IDC_KEYLIST_ADDKEY 101
#define IDC_KEYLIST_ADDKEY_ENC 110
#define IDC_KEYLIST_REENCRYPT 106
#define IDC_KEYLIST_REMOVE 102
#define IDC_KEYLIST_HELP 103
#define IDC_KEYLIST_FPTYPE_STATIC 104
#define IDC_KEYLIST_FPTYPE 105
#define IDC_KEYLIST_MOVEUP 111
#define IDC_KEYLIST_MOVEDOWN 112
#define IDC_KEYLIST_INISTATUS 113
#define IDC_KEYLIST_CONFIRM_LABEL 114
#define IDC_KEYLIST_CONFIRM_YES 115
#define IDC_KEYLIST_CONFIRM_AUTO 116
#define IDC_KEYLIST_CONFIRM_NO 117
#define IDC_KEYLIST_SHOWUNAVAIL 118
#define IDC_KEYLIST_NEWKEY 119      /* launch kittygen */
#define IDC_KEYLIST_SETTINGS 120    /* the [Agent] settings dialog */
#define IDC_KEYLIST_STOPAGENT 121   /* quit kageant */
#define IDC_KEYLIST_ABOUT 122       /* open the About box */
#define IDC_KEYLIST_RESUMECONFIRM 123  /* lift confirm-suppress; shown only when blocked */
#define IDC_KEYLIST_RETRY 124       /* retry the keys whose file was missing */
#define IDC_KEYLIST_AUDITLOG 125    /* open the audit-log viewer */

#define IDC_ABOUT_LICENCE 101
#define IDC_ABOUT_WEBSITE 102
#define IDC_ABOUT_TEXTBOX 1000

#define IDC_LICENCE_TEXTBOX 1000
