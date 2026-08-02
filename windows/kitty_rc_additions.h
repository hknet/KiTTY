/* KiTTY resource/dialog/icon IDs ported from KiTTY windows/putty-rc.h */
#ifndef KITTY_RC_ADDITIONS_H
#define KITTY_RC_ADDITIONS_H
#ifndef NB_ICONES
#define NB_ICONES 50
#endif
#ifndef IDM_RESTARTSESSION
#define IDM_RESTARTSESSION 0xB110
#endif
#ifndef IDA_DON
#define IDA_DON 1007
#endif
#ifndef IDA_TEXT2
#define IDA_TEXT2 1005
#endif
#ifndef IDA_VERSION
#define IDA_VERSION 1006
#endif
#ifndef IDA_WEB2
#define IDA_WEB2 1007
#endif
#ifndef IDC_BAN
#define IDC_BAN 403
#endif
#ifndef IDC_EMAIL
#define IDC_EMAIL 402
#endif
#ifndef IDC_HOVER
#define IDC_HOVER 360
#endif
#ifndef IDC_RESULT
#define IDC_RESULT 1008
#endif
#ifndef IDC_WEBPAGE
#define IDC_WEBPAGE 401
#endif
#ifndef IDD_HK_ABSENT
#define IDD_HK_ABSENT 114
#endif
#ifndef IDD_HK_WRONG
#define IDD_HK_WRONG 115
#endif
#ifndef IDD_KITTYABOUT
#define IDD_KITTYABOUT 121
#endif
/* Master-password (DPAPI Phase 2) prompt dialog + its controls. */
#ifndef IDD_MASTERPW
#define IDD_MASTERPW 131
#endif
#ifndef IDC_MPW_PROMPT
#define IDC_MPW_PROMPT 1100
#endif
#ifndef IDC_MPW_EDIT
#define IDC_MPW_EDIT 1101
#endif
#ifndef IDC_MPW_CONFIRM_LBL
#define IDC_MPW_CONFIRM_LBL 1102
#endif
#ifndef IDC_MPW_CONFIRM
#define IDC_MPW_CONFIRM 1103
#endif
#ifndef IDC_MPW_EDIT_LBL
#define IDC_MPW_EDIT_LBL 1105
#endif
/* Legacy->protected password migration consent dialog (portable saves). */
#ifndef IDD_MIGRATEWARN
#define IDD_MIGRATEWARN 132
#endif
#ifndef IDC_MIG_NOASK
#define IDC_MIG_NOASK 1104
#endif
/* Named-proxy editor dialog (cyd01/KiTTY#11) + its controls. */
#ifndef IDD_PROXYEDIT
#define IDD_PROXYEDIT 133
#endif
#ifndef IDC_PXE_NAME
#define IDC_PXE_NAME 1200
#endif
#ifndef IDC_PXE_TYPE
#define IDC_PXE_TYPE 1201
#endif
#ifndef IDC_PXE_HOST
#define IDC_PXE_HOST 1202
#endif
#ifndef IDC_PXE_PORT
#define IDC_PXE_PORT 1203
#endif
#ifndef IDC_PXE_USER
#define IDC_PXE_USER 1204
#endif
#ifndef IDC_PXE_PASS
#define IDC_PXE_PASS 1205
#endif
#ifndef IDC_PXE_SHOWPW
#define IDC_PXE_SHOWPW 1206
#endif
#ifndef IDC_PXE_EXCLUDE
#define IDC_PXE_EXCLUDE 1207
#endif
#ifndef IDC_PXE_SAVE
#define IDC_PXE_SAVE 1208
#endif
#ifndef IDC_PXE_DELETE
#define IDC_PXE_DELETE 1209
#endif
#ifndef IDC_PXE_BANNER
#define IDC_PXE_BANNER 1210
#endif
#ifndef IDC_PXE_COMMAND
#define IDC_PXE_COMMAND 1211
#endif
#ifndef IDC_PXE_LOCALHOST
#define IDC_PXE_LOCALHOST 1212
#endif
#ifndef IDC_PXE_DNS
#define IDC_PXE_DNS 1213
#endif
#ifndef IDC_PXE_LOGTOTERM
#define IDC_PXE_LOGTOTERM 1214
#endif
/* "Update available / up to date" popup (kitty/kitty_win.c). A real dialog so
 * the dialog manager gives it the shell font at the right DPI, like every other
 * KiTTY dialog. */
#ifndef IDD_UPDATEBOX
#define IDD_UPDATEBOX 134
#endif
#ifndef IDC_UPD_TEXT
#define IDC_UPD_TEXT 1220
#endif
#ifndef IDC_UPD_UPDATE
#define IDC_UPD_UPDATE 1221
#endif
#ifndef IDI_BLACKBALL
#define IDI_BLACKBALL 9902
#endif
#ifndef IDI_EDITICON
#define IDI_EDITICON 9903
#endif
#ifndef IDI_FILEASSOC
#define IDI_FILEASSOC 9904
#endif
#ifndef IDI_MAINICON_0
#define IDI_MAINICON_0 1
#endif
#ifndef IDI_MAINICON_1
#define IDI_MAINICON_1 2
#endif
#ifndef IDI_MAINICON_10
#define IDI_MAINICON_10 11
#endif
#ifndef IDI_MAINICON_11
#define IDI_MAINICON_11 12
#endif
#ifndef IDI_MAINICON_12
#define IDI_MAINICON_12 13
#endif
#ifndef IDI_MAINICON_13
#define IDI_MAINICON_13 14
#endif
#ifndef IDI_MAINICON_14
#define IDI_MAINICON_14 15
#endif
#ifndef IDI_MAINICON_15
#define IDI_MAINICON_15 16
#endif
#ifndef IDI_MAINICON_16
#define IDI_MAINICON_16 17
#endif
#ifndef IDI_MAINICON_17
#define IDI_MAINICON_17 18
#endif
#ifndef IDI_MAINICON_18
#define IDI_MAINICON_18 19
#endif
#ifndef IDI_MAINICON_19
#define IDI_MAINICON_19 20
#endif
#ifndef IDI_MAINICON_2
#define IDI_MAINICON_2 3
#endif
#ifndef IDI_MAINICON_20
#define IDI_MAINICON_20 21
#endif
#ifndef IDI_MAINICON_21
#define IDI_MAINICON_21 22
#endif
#ifndef IDI_MAINICON_22
#define IDI_MAINICON_22 23
#endif
#ifndef IDI_MAINICON_23
#define IDI_MAINICON_23 24
#endif
#ifndef IDI_MAINICON_24
#define IDI_MAINICON_24 25
#endif
#ifndef IDI_MAINICON_25
#define IDI_MAINICON_25 26
#endif
#ifndef IDI_MAINICON_26
#define IDI_MAINICON_26 27
#endif
#ifndef IDI_MAINICON_27
#define IDI_MAINICON_27 28
#endif
#ifndef IDI_MAINICON_28
#define IDI_MAINICON_28 29
#endif
#ifndef IDI_MAINICON_29
#define IDI_MAINICON_29 30
#endif
#ifndef IDI_MAINICON_3
#define IDI_MAINICON_3 4
#endif
#ifndef IDI_MAINICON_30
#define IDI_MAINICON_30 31
#endif
#ifndef IDI_MAINICON_31
#define IDI_MAINICON_31 32
#endif
#ifndef IDI_MAINICON_32
#define IDI_MAINICON_32 33
#endif
#ifndef IDI_MAINICON_33
#define IDI_MAINICON_33 34
#endif
#ifndef IDI_MAINICON_34
#define IDI_MAINICON_34 35
#endif
#ifndef IDI_MAINICON_35
#define IDI_MAINICON_35 36
#endif
#ifndef IDI_MAINICON_36
#define IDI_MAINICON_36 37
#endif
#ifndef IDI_MAINICON_37
#define IDI_MAINICON_37 38
#endif
#ifndef IDI_MAINICON_38
#define IDI_MAINICON_38 39
#endif
#ifndef IDI_MAINICON_39
#define IDI_MAINICON_39 40
#endif
#ifndef IDI_MAINICON_4
#define IDI_MAINICON_4 5
#endif
#ifndef IDI_MAINICON_40
#define IDI_MAINICON_40 41
#endif
#ifndef IDI_MAINICON_41
#define IDI_MAINICON_41 42
#endif
#ifndef IDI_MAINICON_42
#define IDI_MAINICON_42 43
#endif
#ifndef IDI_MAINICON_43
#define IDI_MAINICON_43 44
#endif
#ifndef IDI_MAINICON_44
#define IDI_MAINICON_44 45
#endif
#ifndef IDI_MAINICON_45
#define IDI_MAINICON_45 46
#endif
#ifndef IDI_MAINICON_46
#define IDI_MAINICON_46 47
#endif
#ifndef IDI_MAINICON_47
#define IDI_MAINICON_47 48
#endif
#ifndef IDI_MAINICON_48
#define IDI_MAINICON_48 49
#endif
#ifndef IDI_MAINICON_49
#define IDI_MAINICON_49 50
#endif
#ifndef IDI_MAINICON_5
#define IDI_MAINICON_5 6
#endif
#ifndef IDI_MAINICON_6
#define IDI_MAINICON_6 7
#endif
#ifndef IDI_MAINICON_7
#define IDI_MAINICON_7 8
#endif
#ifndef IDI_MAINICON_8
#define IDI_MAINICON_8 9
#endif
#ifndef IDI_MAINICON_9
#define IDI_MAINICON_9 10
#endif
#ifndef IDI_NOCON
#define IDI_NOCON 9905
#endif
#ifndef IDI_NUCLEAR
#define IDI_NUCLEAR 9906
#endif
#ifndef IDI_PUTTY_LAUNCH
#define IDI_PUTTY_LAUNCH 9901
#endif
#endif /* KITTY_RC_ADDITIONS_H */
#ifndef IDM_VISIBLE
#define IDM_VISIBLE 0xA850
#endif
#ifndef IDM_TRANSPARUP
#define IDM_TRANSPARUP 0xA880
#endif
#ifndef IDM_TRANSPARDOWN
#define IDM_TRANSPARDOWN 0xA890
#endif
#ifndef IDM_TOTRAY
#define IDM_TOTRAY 0xA930
#endif
#ifndef IDM_WINROL
#define IDM_WINROL 0xA900
#endif
#ifndef IDM_FONTUP
#define IDM_FONTUP 0xB050
#endif
#ifndef IDM_FONTDOWN
#define IDM_FONTDOWN 0xB060
#endif
#ifndef IDM_PROTECT
#define IDM_PROTECT 0xA860
#endif
#ifndef IDM_PRINT
#define IDM_PRINT 0xA870
#endif
#ifndef IDM_FONTBLACKANDWHITE
#define IDM_FONTBLACKANDWHITE 0xB070
#endif
#ifndef IDM_FONTNEGATIVE
#define IDM_FONTNEGATIVE 0xB080
#endif
#ifndef IDM_CLEARLOGFILE
#define IDM_CLEARLOGFILE 0xB100
#endif
#ifndef IDM_RESIZE
#define IDM_RESIZE 0xB020
#endif
#ifndef IDM_REPOS
#define IDM_REPOS 0xB030
#endif
#ifndef IDM_SHOWPORTFWD
#define IDM_SHOWPORTFWD 0xA950
#endif
#ifndef IDM_SHORTCUTSTOGGLE
#define IDM_SHORTCUTSTOGGLE 0xB120
#endif
#ifndef IDM_WINSCP
#define IDM_WINSCP 0xA920
#endif
#ifndef IDM_PSCP
#define IDM_PSCP 0xA910
#endif
#ifndef IDM_QUIT
#define IDM_QUIT 0xA840
#endif
#ifndef IDM_EXPORTSETTINGS
#define IDM_EXPORTSETTINGS 0xB040
#endif
#ifndef IDM_DUPKITTY
#define IDM_DUPKITTY 0xB130
#endif
#ifndef IDM_HYPERLINKTOGGLE
#define IDM_HYPERLINKTOGGLE 0xB140
#endif
/* ZModem menu items (multiples of 0x10 - WM_SYSCOMMAND masks wParam & ~0xF). */
#ifndef IDM_XYZSTART
#define IDM_XYZSTART  0xB150
#endif
#ifndef IDM_XYZUPLOAD
#define IDM_XYZUPLOAD 0xB160
#endif
#ifndef IDM_XYZABORT
#define IDM_XYZABORT  0xB170
#endif
/* Bulk session export/import (multiples of 0x10 - WM_SYSCOMMAND masks wParam;
 * 0xB180..0xB1D0 are taken by window.c-local script/mNotepad items). */
#ifndef IDM_EXPORTALLSETTINGS
#define IDM_EXPORTALLSETTINGS 0xB1E0
#endif
#ifndef IDM_IMPORTSETTINGS
#define IDM_IMPORTSETTINGS 0xB1F0
#endif

/* KiTTY send-text input boxes ([Shortcuts] input / inputm and the password
 * variant) and the InfoBox banner. The 0.76 originals were ordinals 117-120,
 * but 117 now collides with PuTTY 0.84's IDD_CA_CONFIG, so they live at
 * fresh ids; kitty.c loads them via these names. */
#ifndef IDB_OK
#define IDB_OK 1098
#endif
#ifndef IDD_INPUTBOX
#define IDD_INPUTBOX 135
#endif
#ifndef IDD_INPUTBOXMULTI
#define IDD_INPUTBOXMULTI 136
#endif
#ifndef IDD_INPUTBOXPW
#define IDD_INPUTBOXPW 137
#endif
#ifndef IDD_INFOBOX
#define IDD_INFOBOX 138
#endif
/* /help command list: modeless, resizable window (HelpBoxProc in
 * kitty/kitty_commands.c). */
#ifndef IDD_HELPBOX
#define IDD_HELPBOX 139
#endif
#ifndef IDC_HELPTEXT
#define IDC_HELPTEXT 1230
#endif
/* Session-export password dialogs (design/TASK_export_password.md). An export
 * bundle is a transport artifact: it gets its OWN password, and the store's
 * master password is left alone. IDD_EXPORTPW collects the choice up front;
 * IDD_EXPORTDONE is the summary that shows the password once, with Copy. */
#ifndef IDD_EXPORTPW
#define IDD_EXPORTPW 140
#endif
#ifndef IDC_EXP_MODEPW
#define IDC_EXP_MODEPW 1240
#endif
#ifndef IDC_EXP_PASS_LBL
#define IDC_EXP_PASS_LBL 1241
#endif
#ifndef IDC_EXP_PASS
#define IDC_EXP_PASS 1242
#endif
#ifndef IDC_EXP_SHOWPW
#define IDC_EXP_SHOWPW 1243
#endif
#ifndef IDC_EXP_MODEDPAPI
#define IDC_EXP_MODEDPAPI 1244
#endif
#ifndef IDC_EXP_DPAPIWARN
#define IDC_EXP_DPAPIWARN 1245
#endif
#ifndef IDD_EXPORTDONE
#define IDD_EXPORTDONE 141
#endif
#ifndef IDC_EXPD_TEXT
#define IDC_EXPD_TEXT 1246
#endif
#ifndef IDC_EXPD_PWLBL
#define IDC_EXPD_PWLBL 1247
#endif
#ifndef IDC_EXPD_PW
#define IDC_EXPD_PW 1248
#endif
#ifndef IDC_EXPD_COPY
#define IDC_EXPD_COPY 1249
#endif
/* Import password prompt (design/TASK_export_password.md §5). Shown only when
 * the bundle actually carries password-protected values; a DPAPI bundle never
 * raises it. The prompt text is set at runtime so a wrong password can re-ask
 * in place with the number of tries left. */
#ifndef IDD_IMPORTPW
#define IDD_IMPORTPW 142
#endif
#ifndef IDC_IMP_PROMPT
#define IDC_IMP_PROMPT 1250
#endif
#ifndef IDC_IMP_PASS
#define IDC_IMP_PASS 1251
#endif
#ifndef IDC_IMP_SHOWPW
#define IDC_IMP_SHOWPW 1252
#endif
/* "Your master password moved into this folder" notice, shown once after the
 * portable master-password migration. Names the folder and offers to copy the
 * path or open it, because the user has to carry that folder to their other
 * portable copies of KiTTY. */
#ifndef IDD_MPWMOVED
#define IDD_MPWMOVED 143
#endif
#ifndef IDC_MPWM_TEXT
#define IDC_MPWM_TEXT 1253
#endif
#ifndef IDC_MPWM_PATH
#define IDC_MPWM_PATH 1254
#endif
#ifndef IDC_MPWM_COPY
#define IDC_MPWM_COPY 1255
#endif
#ifndef IDC_MPWM_OPEN
#define IDC_MPWM_OPEN 1256
#endif
/* Window-title placeholder reference: a modeless list opened from the config
 * box's Window title field, so the codes stay on screen WHILE the title is
 * being typed. Select one and Copy (or double-click) puts it on the clipboard.
 * Classic KiTTY printed the same list as eight static lines in the panel. */
#ifndef IDD_TITLEVARS
#define IDD_TITLEVARS 144
#endif
#ifndef IDC_TITLEVARS_LIST
#define IDC_TITLEVARS_LIST 1257
#endif
#ifndef IDC_TITLEVARS_COPY
#define IDC_TITLEVARS_COPY 1258
#endif
/* OSC 52 clipboard-READ permission prompt (design/TASK_clipboard_read_permission.md).
 * A real dialog rather than a MessageBox because it has to show a MASKED summary
 * of what would be sent plus a View button - someone standing behind you, or a
 * screen-share, must not capture the clipboard just because a dialog appeared,
 * least of all when the answer is about to be no.
 *
 * Deny is the default button AND IDCANCEL, so Return on a dialog nobody read and
 * Escape both refuse. Allow is deliberately NOT IDOK: "OK on a dialog nobody
 * read" is precisely how this feature would go wrong. */
#ifndef IDD_OSC52READ
#define IDD_OSC52READ 145
#endif
#ifndef IDC_O52_WHAT
#define IDC_O52_WHAT 1260
#endif
#ifndef IDC_O52_WHERE
#define IDC_O52_WHERE 1261
#endif
#ifndef IDC_O52_CLAIM
#define IDC_O52_CLAIM 1262
#endif
#ifndef IDC_O52_SUMMARY
#define IDC_O52_SUMMARY 1263
#endif
#ifndef IDC_O52_PREVIEW
#define IDC_O52_PREVIEW 1264
#endif
#ifndef IDC_O52_VIEW
#define IDC_O52_VIEW 1265
#endif
#ifndef IDC_O52_ONCE
#define IDC_O52_ONCE 1266
#endif
#ifndef IDC_O52_MINUTES
#define IDC_O52_MINUTES 1267
#endif
#ifndef IDC_O52_REQUESTS
#define IDC_O52_REQUESTS 1268
#endif
#ifndef IDC_O52_SESSION
#define IDC_O52_SESSION 1269
#endif
#ifndef IDC_O52_ALWAYSDENY
#define IDC_O52_ALWAYSDENY 1270
#endif
#ifndef IDC_O52_ALLOW
#define IDC_O52_ALLOW 1271
#endif
#ifndef IDC_O52_COUNTDOWN
#define IDC_O52_COUNTDOWN 1272
#endif
#ifndef IDC_O52_APPLYLBL
#define IDC_O52_APPLYLBL 1273
#endif
