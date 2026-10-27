; KiTTY++ event log messages (kitty.exe resource). kitty_eventlog_line writes
; event ID 0 with the text as its one insertion string: message 0 shows it
; as written once the system MSI registers kitty.exe as the EventMessageFile
; of the "KiTTY++" source.
;
; Event ID blocks (kitty/kitty_eventids.h is the table of every ID; extend
; both when a block is taken):
;   0        plain text: the one insertion string is the whole message
;   100-199  kageant's agent log (source "KiTTY++_agent")
;   200-299  kitty.exe's sessions (source "KiTTY++_kitty")
;   300-399  klink, kscp, ksftp (source "KiTTY++_cli")
;   400-499  kittygen, kittygen-cli (source "KiTTY++_kittygen")
;   900-999  the suite (source "KiTTY++"): refused program files, log
;            retention, missing Windows features, failed session saves,
;            store access rights not set - always written
;   other    reserved, not used yet
MessageIdTypedef=DWORD
LanguageNames=(English=0x409:MSG00409)

MessageId=0
SymbolicName=KITTY_EVENT_TEXT
Language=English
%1%0
.

; kageant's agent log, source "KiTTY++_agent" (kitty/kitty_auditlog.c,
; audit_kinds): one event ID per kind of record. %1 is the kind (the ev
; value of the file log), %2 onwards the fields in the fixed order of that
; table, an absent field as an empty string. The lists below name them in
; that order; keep the two in step. No Severity= line: the IDs are the
; plain numbers ReportEvent is given. A kind the table does not list is
; reported as message 0 with the whole record as its text.

MessageId=100
SymbolicName=KITTY_AGENT_SIGN
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=101
SymbolicName=KITTY_AGENT_CONFIRM
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=110
SymbolicName=KITTY_AGENT_ADD
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%npath: %9%0
.

MessageId=111
SymbolicName=KITTY_AGENT_LOADKEY
Language=English
%1%npath: %2%nresult: %3%nenc: %4%0
.

MessageId=112
SymbolicName=KITTY_AGENT_FORGET_PATH
Language=English
%1%npath: %2%nresult: %3%0
.

MessageId=113
SymbolicName=KITTY_AGENT_RETRY
Language=English
%1%ntrigger: %2%nloaded: %3%nrefused: %4%nabsent: %5%nbroken: %6%0
.

MessageId=114
SymbolicName=KITTY_AGENT_REMOVE
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=115
SymbolicName=KITTY_AGENT_REMOVE_ALL
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=120
SymbolicName=KITTY_AGENT_LOCK
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=121
SymbolicName=KITTY_AGENT_UNLOCK
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=130
SymbolicName=KITTY_AGENT_HELLO_ENROL
Language=English
%1%npath: %2%nresult: %3%ndetail: %4%0
.

MessageId=131
SymbolicName=KITTY_AGENT_HELLO_PROTECT
Language=English
%1%npath: %2%ndest: %3%nresult: %4%ndetail: %5%nerr: %6%0
.

MessageId=132
SymbolicName=KITTY_AGENT_HELLO_UNLOCK
Language=English
%1%npath: %2%nresult: %3%ndetail: %4%0
.

MessageId=140
SymbolicName=KITTY_AGENT_KEYFILE_WARNING
Language=English
%1%npath: %2%nwarning: %3%0
.

MessageId=150
SymbolicName=KITTY_AGENT_REENCRYPT_ALL
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=151
SymbolicName=KITTY_AGENT_REENCRYPT
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%nreason: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=152
SymbolicName=KITTY_AGENT_DECRYPT
Language=English
%1%nfp: %2%ncomment: %3%nresult: %4%ntrigger: %5%nreq: %6%nreqpath: %7%npid: %8%0
.

MessageId=160
SymbolicName=KITTY_AGENT_START_STOP
Language=English
%1%nresult: %2%0
.

MessageId=170
SymbolicName=KITTY_AGENT_LOGROTATE
Language=English
%1%nresult: %2%0
.

; kitty.exe's sessions, source "KiTTY++_kitty" (kitty/kitty_winevent.c, the
; table in kitty/kitty_eventids.h), written with [KiTTY] sessioneventlog=yes.
; %1 is the kind, %2 onwards the fields in the table's order, an absent
; field as an empty string; keep the two in step (tools/check-kitty-events.py).

MessageId=200
SymbolicName=KITTY_SESSION_SESSION_OPEN
Language=English
%1%nsession: %2%nhost: %3%nport: %4%nproto: %5%naddr: %6%nfrom: %7%npid: %8%0
.

MessageId=201
SymbolicName=KITTY_SESSION_SESSION_CLOSE
Language=English
%1%nsession: %2%nhost: %3%nport: %4%nresult: %5%nreason: %6%nexitcode: %7%npid: %8%0
.

MessageId=210
SymbolicName=KITTY_SESSION_HOSTKEY_KNOWN
Language=English
%1%nhost: %2%nport: %3%nkeytype: %4%nfp: %5%nbasis: %6%npid: %7%nproxy: %8%0
.

MessageId=211
SymbolicName=KITTY_SESSION_HOSTKEY_NEW
Language=English
%1%nhost: %2%nport: %3%nkeytype: %4%nfp: %5%nresult: %6%npid: %7%nproxy: %8%0
.

MessageId=212
SymbolicName=KITTY_SESSION_HOSTKEY_MISMATCH
Language=English
%1%nhost: %2%nport: %3%nkeytype: %4%nfp: %5%nresult: %6%npid: %7%nproxy: %8%0
.

MessageId=213
SymbolicName=KITTY_SESSION_HOSTKEY_UPDATE
Language=English
%1%nhost: %2%nport: %3%nkeytype: %4%nfp: %5%nresult: %6%noldfp: %7%npid: %8%nproxy: %9%0
.

MessageId=220
SymbolicName=KITTY_SESSION_AUTH_OK
Language=English
%1%nhost: %2%nport: %3%nuser: %4%nmethod: %5%nfp: %6%npid: %7%nproxy: %8%nfor: %9%0
.

MessageId=221
SymbolicName=KITTY_SESSION_AUTH_FAILED
Language=English
%1%nhost: %2%nport: %3%nuser: %4%nmethod: %5%nreason: %6%nmethods: %7%npid: %8%nproxy: %9%nfor: %10%0
.

MessageId=225
SymbolicName=KITTY_SESSION_PROXY_USED
Language=English
%1%nproxy: %2%nhost: %3%nport: %4%nuser: %5%ncredentials: %6%npid: %7%nfor: %8%0
.

MessageId=230
SymbolicName=KITTY_SESSION_FAREND_REFUSED
Language=English
%1%nsession: %2%nhost: %3%nrequest: %4%nreason: %5%ndetail: %6%npid: %7%0
.

MessageId=240
SymbolicName=KITTY_KITTY_TRANSFER_DONE
Language=English
%1%nsession: %2%nhost: %3%nport: %4%nuser: %5%nmethod: %6%ndirection: %7%nlocal: %8%nremote: %9%nbytes: %10%npid: %11%0
.

MessageId=241
SymbolicName=KITTY_KITTY_TRANSFER_FAILED
Language=English
%1%nsession: %2%nhost: %3%nport: %4%nuser: %5%nmethod: %6%ndirection: %7%nlocal: %8%nremote: %9%nbytes: %10%npid: %11%nreason: %12%0
.

; klink, kscp and ksftp, source "KiTTY++_cli", with [KiTTY] clieventlog=yes:
; 300-321 are 200-221 (the SSH code reporting them is shared), 330/331 one
; per transfer job.

MessageId=300
SymbolicName=KITTY_CLI_SESSION_OPEN
Language=English
%1%nsession: %2%nhost: %3%nport: %4%nproto: %5%naddr: %6%nfrom: %7%npid: %8%0
.

MessageId=301
SymbolicName=KITTY_CLI_SESSION_CLOSE
Language=English
%1%nsession: %2%nhost: %3%nport: %4%nresult: %5%nreason: %6%nexitcode: %7%npid: %8%0
.

MessageId=310
SymbolicName=KITTY_CLI_HOSTKEY_KNOWN
Language=English
%1%nhost: %2%nport: %3%nkeytype: %4%nfp: %5%nbasis: %6%npid: %7%nproxy: %8%0
.

MessageId=311
SymbolicName=KITTY_CLI_HOSTKEY_NEW
Language=English
%1%nhost: %2%nport: %3%nkeytype: %4%nfp: %5%nresult: %6%npid: %7%nproxy: %8%0
.

MessageId=312
SymbolicName=KITTY_CLI_HOSTKEY_MISMATCH
Language=English
%1%nhost: %2%nport: %3%nkeytype: %4%nfp: %5%nresult: %6%npid: %7%nproxy: %8%0
.

MessageId=313
SymbolicName=KITTY_CLI_HOSTKEY_UPDATE
Language=English
%1%nhost: %2%nport: %3%nkeytype: %4%nfp: %5%nresult: %6%noldfp: %7%npid: %8%nproxy: %9%0
.

MessageId=320
SymbolicName=KITTY_CLI_AUTH_OK
Language=English
%1%nhost: %2%nport: %3%nuser: %4%nmethod: %5%nfp: %6%npid: %7%nproxy: %8%nfor: %9%0
.

MessageId=321
SymbolicName=KITTY_CLI_AUTH_FAILED
Language=English
%1%nhost: %2%nport: %3%nuser: %4%nmethod: %5%nreason: %6%nmethods: %7%npid: %8%nproxy: %9%nfor: %10%0
.

MessageId=325
SymbolicName=KITTY_CLI_PROXY_USED
Language=English
%1%nproxy: %2%nhost: %3%nport: %4%nuser: %5%ncredentials: %6%npid: %7%nfor: %8%0
.

MessageId=330
SymbolicName=KITTY_CLI_TRANSFER_DONE
Language=English
%1%nsession: %2%nhost: %3%nuser: %4%ndirection: %5%nlocal: %6%nremote: %7%nbytes: %8%npid: %9%0
.

MessageId=331
SymbolicName=KITTY_CLI_TRANSFER_FAILED
Language=English
%1%nsession: %2%nhost: %3%nuser: %4%ndirection: %5%nlocal: %6%nremote: %7%nbytes: %8%npid: %9%nreason: %10%0
.

; kittygen and kittygen-cli, source "KiTTY++_kittygen", with [KiTTY]
; kittygeneventlog=yes.

MessageId=400
SymbolicName=KITTY_KGEN_KEY_GENERATED
Language=English
%1%ntool: %2%nkeytype: %3%nbits: %4%nfp: %5%ncomment: %6%npid: %7%0
.

MessageId=401
SymbolicName=KITTY_KGEN_PRIVATE_SAVED
Language=English
%1%ntool: %2%nfp: %3%ncomment: %4%npath: %5%nformat: %6%nprotection: %7%nresult: %8%nreason: %9%npid: %10%0
.

MessageId=402
SymbolicName=KITTY_KGEN_PUBLIC_SAVED
Language=English
%1%ntool: %2%nfp: %3%npath: %4%nformat: %5%nresult: %6%nreason: %7%npid: %8%0
.

MessageId=403
SymbolicName=KITTY_KGEN_PASSPHRASE_CHANGED
Language=English
%1%ntool: %2%nfp: %3%npath: %4%nprotection: %5%npid: %6%0
.

; The suite, source "KiTTY++", always written, in the Application log: a
; program file refused at start (rename, signature, integrity), log
; retention, missing Windows features, failed session saves, folder-store
; access rights not set.

MessageId=900
SymbolicName=KITTY_SUITE_PROGRAM_REFUSED
Language=English
%1%ncheck: %2%nfound: %3%nprogram: %4%0
.

MessageId=901
SymbolicName=KITTY_SUITE_LOG_RETENTION
Language=English
%1%nfiles: %2%nkb: %3%ndays: %4%nmb: %5%npattern: %6%0
.

MessageId=902
SymbolicName=KITTY_SUITE_MISSING_FEATURES
Language=English
%1%nfeatures: %2%nmissing: %3%0
.

MessageId=903
SymbolicName=KITTY_SUITE_SESSION_SAVE_FAILED
Language=English
%1%nsession: %2%npath: %3%nreason: %4%0
.

MessageId=904
SymbolicName=KITTY_SUITE_STORE_ACL_NOT_SET
Language=English
%1%npath: %2%nreason: %3%0
.
