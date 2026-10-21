; KiTTY++ event log messages (kitty.exe resource). kitty_eventlog_line writes
; event ID 0 with the text as its one insertion string: message 0 shows it
; as written once the system MSI registers kitty.exe as the EventMessageFile
; of the "KiTTY++" source.
;
; Event ID blocks (extend this list when a block is taken):
;   0        plain text: the one insertion string is the whole message
;   100-199  kageant's agent log (source "KiTTY++_agent")
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
