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

#define IDC_ABOUT_LICENCE 101
#define IDC_ABOUT_WEBSITE 102
#define IDC_ABOUT_TEXTBOX 1000

#define IDC_LICENCE_TEXTBOX 1000
