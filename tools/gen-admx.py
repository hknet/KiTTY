#!/usr/bin/env python3
r"""Generate the Group Policy template windows/policy/kittypp.admx and its
en-US language file windows/policy/en-US/kittypp.adml.

Every policy is one add() line below: the value name the programs read under
Software\Policies\kapper.net\KiTTY++, its kind, its category, the text gpedit
shows. A policy marked recommended appears a second time under "KiTTY++ -
Default Settings (users can override)", written to the Recommended subkey.

It also writes the table of every policy into docs/GROUP-POLICY.md, between
its two "gen-admx" marker lines, so the guide cannot list other settings than
the template holds.

Re-run after editing this file:  python3 tools/gen-admx.py
With --check it writes nothing and fails when a file differs (the build runs
it so).
"""
import pathlib
import sys
from xml.sax.saxutils import escape

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "windows" / "policy"
DOC = ROOT / "docs" / "GROUP-POLICY.md"
DOC_BEGIN = "<!-- gen-admx: the table below is written by tools/gen-admx.py -->"
DOC_END = "<!-- gen-admx: end of the table -->"
KEY = r"Software\Policies\kapper.net\KiTTY++"
REC = KEY + r"\Recommended"

# id, valueName, category, kind, params, display, help, recommended
P = []
def add(*a): P.append(a)

# --- the settings store (no Recommended: they decide where settings live)
add("SaveMode", "savemode", "Store", "enum_str",
    [("registry", "registry"), ("dir", "dir"), ("file", "file")],
    "Settings store type",
    "Specifies where KiTTY++ stores its sessions and settings.\n\n"
    "registry: the Windows registry (the default for kitty.exe).\n"
    "dir: one file per session in the program folder, the portable store (the default for kitty_portable.exe).\n"
    "file: the registry, with a .sav file imported at startup if the registry key is missing. Legacy mode.\n\n"
    "All KiTTY++ programs apply this policy setting, including kitty_portable.exe. The /savemode command and the store buttons are rejected.\n\n"
    "Registry value: savemode (REG_SZ).", False)
add("ReadOnly", "readonly", "Store", "boolstr", None,
    "Make the settings store read-only",
    "If you enable this policy setting, no KiTTY++ program writes to the settings store. Changes apply to the running process only. Saving a session and the store commands are rejected with \"Set by your administrator\".\n\n"
    "If you disable this policy setting, the store is writable. A readonly=yes entry in kitty.ini is ignored.\n\n"
    "Registry value: readonly (REG_SZ yes or no).", False)
add("KiClassName", "KiClassName", "Store", "text", 64,
    "Window class name and registry key",
    "PuTTY: KiTTY++ registers the PuTTY window class and stores its sessions under the PuTTY registry key (Software\\SimonTatham\\PuTTY), shared with an installed PuTTY.\n\n"
    "If you do not configure this policy setting, KiTTY++ uses its own window class and registry key (Software\\kapper.net\\KiTTY).\n\n"
    "Registry value: KiClassName (REG_SZ).", False)

# --- hardening
add("RestrictAcl", "restrictacl", "Hardening", "boolstr", None,
    "Restrict access to KiTTY++ processes",
    "If you enable this policy setting, kitty.exe and kageant restrict the access rights that other processes of the same user have to them (the process ACL), like the -restrict-acl command-line option.\n\n"
    "This policy setting takes precedence over the machine-wide lock an administrator sets in the program (Application > Security > Hardening).\n\n"
    "Registry value: restrictacl (REG_SZ yes or no).", True)
add("HideApplicationTab", "applicationsettings", "Hardening", "boolstr_inv", None,
    "Hide the Application tab",
    "If you enable this policy setting, the configuration window has no Application tab. None of its panels is created. -cfgpanel Application/... and the buttons that link to it open the Session tab.\n\n"
    "If you disable this policy setting, the tab is shown.\n\n"
    "Registry value: applicationsettings (REG_SZ no to hide, yes to show).", True)

# --- kageant: keys and notifications
B = "bool"
add("ConfirmKeyUse", "ConfirmKeyUse", "AgentKeys", "enum_dword",
    [("ConfirmAuto", 0), ("ConfirmEvery", 1), ("ConfirmNever", 2)],
    "Require confirmation for key use",
    "Specifies which key uses require confirmation before the key signs.\n\n"
    "Keys that request it: only keys whose comment contains \"confirmation\" (the default).\n"
    "Every key use: confirmation for each signature.\n"
    "Never: no confirmation. Requests in key comments are ignored as well. A key set to confirm with Windows Hello still requires confirmation.\n\n"
    "Registry value: ConfirmKeyUse (REG_DWORD 0, 1 or 2).", True)
add("HelloConfirm", "HelloConfirm", "AgentKeys", B, None,
    "Confirm key use with Windows Hello",
    "If you enable this policy setting, every key-use confirmation requires Windows Hello verification (biometrics or the device PIN) instead of a Yes button. If Windows Hello is not available, the request is rejected.\n\n"
    "Registry value: HelloConfirm (REG_DWORD).", True)
add("NotifyOnKeyUse", "NotifyOnKeyUse", "AgentKeys", B, None,
    "Notify on key use",
    "If you enable this policy setting, a notification shows the key after each signature. A key that another program adds or removes triggers a notification that names that program.\n\n"
    "Registry value: NotifyOnKeyUse (REG_DWORD).", True)
add("NotifyOnKeyArrival", "NotifyOnKeyArrival", "AgentKeys", B, None,
    "Notify when startup keys become available",
    "If you enable this policy setting (the default), a notification appears when missing startup keys become available on a drive. The agent log records them in either case.\n\n"
    "Registry value: NotifyOnKeyArrival (REG_DWORD).", True)
add("NoticeTimeout", "NoticeTimeout", "AgentKeys", "dec", (2, 120, 8),
    "Notification display time",
    "The number of seconds kageant's notifications are displayed (2 to 120). If you do not configure this policy setting, each notification uses its own default.\n\n"
    "Registry value: NoticeTimeout (REG_DWORD).", True)
add("LoadKeysOnStartup", "LoadKeysOnStartup", "AgentKeys", B, None,
    "Load remembered keys at startup",
    "If you enable this policy setting, kageant loads the remembered key files at startup in encrypted form. The passphrase is required at first use. Only the file paths are stored.\n\n"
    "Registry value: LoadKeysOnStartup (REG_DWORD).", True)
add("RetryKeys", "RetryKeys", "AgentKeys", "enum_dword",
    [("RetryNever", 0), ("RetryPath", 1), ("RetryAnyDrive", 2)],
    "Load startup keys from newly attached drives",
    "Specifies what kageant does with startup keys that were missing at startup when a drive is attached.\n\n"
    "Never: the keys remain missing.\n"
    "From the stored path: the keys are loaded from their stored drive and path (the default).\n"
    "From any drive letter: the keys are also searched for at their stored path on the newly attached drive. A key is loaded only if its fingerprint matches the stored one.\n\n"
    "Registry value: RetryKeys (REG_DWORD 0, 1 or 2).", True)
add("UnloadOnRemove", "UnloadOnRemove", "AgentKeys", B, None,
    "Unload keys when their drive is removed",
    "If you enable this policy setting, a key whose drive is removed is unloaded from the agent until the drive is attached again. Disabled by default, so removing a USB drive does not interrupt a login in progress.\n\n"
    "Registry value: UnloadOnRemove (REG_DWORD).", True)
add("QuietMissingKeys", "QuietMissingKeys", "AgentKeys", B, None,
    "Suppress notifications for missing startup keys",
    "If you enable this policy setting, no notification appears at startup for startup keys whose drive is not present. A key file that exists but fails to load is still reported.\n\n"
    "Registry value: QuietMissingKeys (REG_DWORD).", True)
add("PassphraseCacheSeconds", "PassphraseCacheSeconds", "AgentKeys", "dec", (0, 300, 60),
    "Passphrase cache time",
    "The number of seconds an entered passphrase is kept, encrypted in memory, to unlock further keys of the same batch (0 to 300, 0 = not cached). Default: 60.\n\n"
    "Registry value: PassphraseCacheSeconds (REG_DWORD).", True)
add("HelloCacheSeconds", "HelloCacheSeconds", "AgentKeys", "dec", (0, 300, 60),
    "Windows Hello cache time",
    "The number of seconds one Windows Hello verification also unlocks further protected keys (0 to 300, 0 = verification each time). Default: 60.\n\n"
    "Registry value: HelloCacheSeconds (REG_DWORD).", True)
add("AutoEncryptMode", "AutoEncryptMode", "AgentKeys", "enum_dword",
    [("AutoEncOff", 0), ("AutoEncDefault", 1), ("AutoEncEnforce", 2)],
    "Re-encrypt idle keys",
    "Passphrase-protected keys are re-encrypted after the timeout of \"Re-encryption timeout\".\n\n"
    "Off: no timeout (the default).\n"
    "Keys without their own timeout: the timeout applies to keys that set none.\n"
    "All keys: the timeout applies to every key.\n\n"
    "Registry value: AutoEncryptMode (REG_DWORD 0, 1 or 2).", True)
add("AutoEncryptSeconds", "AutoEncryptSeconds", "AgentKeys", "dec", (1, 604800, 600),
    "Re-encryption timeout",
    "The number of seconds after a key was decrypted or last used for a signature before it is encrypted again: 1 = immediately after each signature, otherwise 30 to 604800 (7 days). Values from 2 to 29 are treated as 30. Default: 600.\n\n"
    "Registry value: AutoEncryptSeconds (REG_DWORD).", True)
add("LockWithWindows", "LockWithWindows", "AgentKeys", "enum_dword",
    [("WinLockKeep", 0), ("WinLockLock", 1), ("WinLockReencrypt", 2)],
    "Action when Windows is locked",
    "Specifies what happens to the agent when Windows is locked.\n\n"
    "No action (the default).\n"
    "Lock the agent.\n"
    "Re-encrypt the keys.\n\n"
    "Registry value: LockWithWindows (REG_DWORD 0, 1 or 2).", True)
add("ReadKnownHosts", "ReadKnownHosts", "AgentKeys", B, None,
    "Read the OpenSSH known_hosts file",
    "If you enable this policy setting, %USERPROFILE%\\.ssh\\known_hosts is a source of host names for the agent log, the confirmation dialog and the rule editor. If you disable this policy setting (the default), kageant never opens the file. KiTTY++ never writes to it.\n\n"
    "Registry value: ReadKnownHosts (REG_DWORD).", True)

# --- kageant: requests from other programs
add("BlockIpcAdd", "BlockIpcAdd", "AgentAccess", B, None,
    "Block keys added by client programs",
    "If you enable this policy setting, a key that another program adds is rejected. Together with \"Block key removal by client programs\", no program can change the loaded keys. kageant's own buttons are not affected.\n\n"
    "Registry value: BlockIpcAdd (REG_DWORD).", True)
add("BlockIpcRemove", "BlockIpcRemove", "AgentAccess", B, None,
    "Block key removal by client programs",
    "If you enable this policy setting, a remove or remove-all request from another program is rejected. Signing and listing keys are not affected.\n\n"
    "Registry value: BlockIpcRemove (REG_DWORD).", True)
add("AllowIpcLock", "AllowIpcLock", "AgentAccess", B, None,
    "Allow client programs to lock the agent",
    "If you enable this policy setting, programs can lock and unlock the agent (ssh-add -x and -X). If you disable this policy setting (the default), only the tray menu locks and unlocks it.\n\n"
    "Registry value: AllowIpcLock (REG_DWORD).", True)
add("BlockForwardedChanges", "BlockForwardedChanges", "AgentAccess", B, None,
    "Block changes through agent forwarding",
    "If you enable this policy setting (the default), add, remove, lock and unlock requests from a program on a server the agent is forwarded to are rejected. Listing and signing are not affected. A connection counts as forwarded only if its client sends session-bind marked for forwarding.\n\n"
    "Registry value: BlockForwardedChanges (REG_DWORD).", True)
add("AllowWmCopyData", "AllowWmCopyData", "AgentAccess", B, None,
    "Allow requests by window messages",
    "If you enable this policy setting, requests by window messages (WM_COPYDATA) are accepted. Programs built on PuTTY before 0.75 connect to the agent this way. kageant cannot identify the program that sends such a request. If you disable this policy setting (the default), they are rejected.\n\n"
    "Registry value: AllowWmCopyData (REG_DWORD).", True)
add("OpenSSHPipe", "OpenSSHPipe", "AgentAccess", B, None,
    "Serve the Windows OpenSSH agent pipe",
    "If you enable this policy setting, kageant also serves \\\\.\\pipe\\openssh-ssh-agent for ssh.exe, ssh-add and other OpenSSH clients. If another program already owns that pipe, kageant serves only its own pipe.\n\n"
    "Registry value: OpenSSHPipe (REG_DWORD).", True)
add("ExtraPipe", "ExtraPipe", "AgentAccess", "text", 200,
    "Additional named pipe",
    "An additional pipe name that kageant serves as \\\\.\\pipe\\<name>. The name persists across restarts. Allowed characters: letters, digits, '.', '_' and '-', up to 200.\n\n"
    "Registry value: ExtraPipe (REG_SZ).", True)
add("SetSshAuthSock", "SetSshAuthSock", "AgentAccess", B, None,
    "Set SSH_AUTH_SOCK",
    "If you enable this policy setting, the user's SSH_AUTH_SOCK environment variable points to the additional named pipe. Programs started afterwards use it. A value set by someone else is not overwritten.\n\n"
    "Registry value: SetSshAuthSock (REG_DWORD).", True)

# --- kageant: the agent log
add("AgentLog", "AgentLog", "AgentLog", B, None,
    "Enable the agent log",
    "If you enable this policy setting (the default), kageant records what it does with keys, as signed and chained entries, at the destination set by \"Agent log destination\".\n\n"
    "Registry value: AgentLog (REG_DWORD).", True)
add("AgentLogPath", "AgentLogPath", "AgentLog", "text", 260,
    "Agent log file path",
    "The full path of the log file. If you do not configure this policy setting: next to kitty.ini for a portable copy, otherwise %LOCALAPPDATA%\\kapper.net\\KiTTY\\kageant.log. Environment variables are not expanded.\n\n"
    "Registry value: AgentLogPath (REG_SZ).", True)
add("AgentLogSink", "AgentLogSink", "AgentLog", "enum_str",
    [("SinkFile", "file"), ("SinkEventLog", "eventlog"), ("SinkBoth", "both")],
    "Agent log destination",
    "File, Windows Event Log, or both. If you do not configure this policy setting: both if the KiTTY++_Agent event log exists (an all-users installation), otherwise the file.\n\n"
    "Registry value: AgentLogSink (REG_SZ).", True)
add("AgentLogMaxKB", "AgentLogMaxKB", "AgentLog", "dec", (16, 1048576, 5120),
    "Maximum log file size",
    "The size in KB at which the log file is rotated (16 to 1048576). Default: 5120.\n\n"
    "Registry value: AgentLogMaxKB (REG_DWORD).", True)
add("AgentLogKeep", "AgentLogKeep", "AgentLog", "dec", (1, 99, 3),
    "Number of rotated files to keep",
    "The number of rotated log files to keep (1 to 99). Default: 3.\n\n"
    "Registry value: AgentLogKeep (REG_DWORD).", True)
add("AgentLogExpireDays", "AgentLogExpireDays", "AgentLog", "dec", (0, 3650, 90),
    "Rotated file retention",
    "The number of days after which rotated log files are deleted at the next rotation (0 to 3650, 0 = never deleted). Default: 90.\n\n"
    "Registry value: AgentLogExpireDays (REG_DWORD).", True)

ENUM_TEXT = {
    "registry": "registry", "dir": "dir (portable)", "file": "file (legacy)",
    "ConfirmAuto": "Keys that request it", "ConfirmEvery": "Every key use", "ConfirmNever": "Never",
    "RetryNever": "Never", "RetryPath": "From the stored path", "RetryAnyDrive": "From any drive letter",
    "AutoEncOff": "Off", "AutoEncDefault": "Keys without their own timeout", "AutoEncEnforce": "All keys",
    "WinLockKeep": "No action", "WinLockLock": "Lock the agent", "WinLockReencrypt": "Re-encrypt the keys",
    "SinkFile": "File", "SinkEventLog": "Windows Event Log", "SinkBoth": "File and Windows Event Log",
}
CATS = [
    ("KiTTYpp", None, "KiTTY++"),
    ("Store", "KiTTYpp", "Settings store"),
    ("Hardening", "KiTTYpp", "Hardening"),
    ("Agent", "KiTTYpp", "kageant (SSH agent)"),
    ("AgentKeys", "Agent", "Keys and notifications"),
    ("AgentAccess", "Agent", "Requests from client programs"),
    ("AgentLog", "Agent", "Agent log"),
    ("KiTTYpp_Rec", None, "KiTTY++ - Default Settings (users can override)"),
    ("Hardening_Rec", "KiTTYpp_Rec", "Hardening"),
    ("Agent_Rec", "KiTTYpp_Rec", "kageant (SSH agent)"),
    ("AgentKeys_Rec", "Agent_Rec", "Keys and notifications"),
    ("AgentAccess_Rec", "Agent_Rec", "Requests from client programs"),
    ("AgentLog_Rec", "Agent_Rec", "Agent log"),
]
REC_NOTE = ("\n\nRecommended policy: applies while the user has no value of their own. "
            "The mandatory policy of the same name under \"KiTTY++\" overrides it.")

def esc(s): return escape(s, {'"': "&quot;"})

def policy_xml(pid, vname, cat, kind, prm, rec):
    name = pid + ("_Recommended" if rec else "")
    key = REC if rec else KEY
    sid = name
    head = (f'    <policy name="{name}" class="Both" displayName="$(string.{sid})" '
            f'explainText="$(string.{sid}_Help)" key="{esc(key)}"')
    parent = f'      <parentCategory ref="{cat}{"_Rec" if rec else ""}" />\n      <supportedOn ref="SUPPORTED_KiTTYpp_0_85_1_22" />\n'
    if kind in ("bool", "boolstr", "boolstr_inv"):
        if kind == "bool":
            on, off = '<decimal value="1" />', '<decimal value="0" />'
        elif kind == "boolstr":
            on, off = '<string>yes</string>', '<string>no</string>'
        else:
            on, off = '<string>no</string>', '<string>yes</string>'
        return (head + f' valueName="{vname}">\n' + parent +
                f'      <enabledValue>{on}</enabledValue>\n'
                f'      <disabledValue>{off}</disabledValue>\n    </policy>\n')
    head += f' presentation="$(presentation.{sid})">\n'
    el = "      <elements>\n"
    if kind == "dec":
        lo, hi, _ = prm
        el += f'        <decimal id="{pid}_Value" valueName="{vname}" minValue="{lo}" maxValue="{hi}" required="true" />\n'
    elif kind == "text":
        el += f'        <text id="{pid}_Value" valueName="{vname}" maxLength="{prm}" required="true" />\n'
    else:
        el += f'        <enum id="{pid}_Value" valueName="{vname}" required="true">\n'
        for iid, v in prm:
            val = f'<decimal value="{v}" />' if kind == "enum_dword" else f'<string>{v}</string>'
            el += f'          <item displayName="$(string.{pid}_{iid})"><value>{val}</value></item>\n'
        el += '        </enum>\n'
    el += "      </elements>\n"
    return head + parent + el + "    </policy>\n"

def presentation_xml(pid, kind, prm, rec, display):
    sid = pid + ("_Recommended" if rec else "")
    if kind in ("bool", "boolstr", "boolstr_inv"):
        return ""
    s = f'      <presentation id="{sid}">\n'
    label = esc(display)
    if kind == "dec":
        s += f'        <decimalTextBox refId="{pid}_Value" defaultValue="{prm[2]}">{label}</decimalTextBox>\n'
    elif kind == "text":
        s += f'        <textBox refId="{pid}_Value"><label>{label}</label></textBox>\n'
    else:
        s += f'        <dropdownList refId="{pid}_Value" noSort="true" defaultItem="0">{label}</dropdownList>\n'
    return s + "      </presentation>\n"

admx = ['<?xml version="1.0" encoding="utf-8"?>\n',
        '<!-- KiTTY++ Group Policy template. Copy kittypp.admx to %SystemRoot%\\PolicyDefinitions\n'
        '     and en-US\\kittypp.adml to %SystemRoot%\\PolicyDefinitions\\en-US (or to the\n'
        '     domain\'s Central Store). Values are written to Software\\Policies\\kapper.net\\KiTTY++\n'
        '     under HKLM (Computer Configuration) or HKCU (User Configuration). HKLM takes precedence. -->\n',
        '<policyDefinitions xmlns:xsd="http://www.w3.org/2001/XMLSchema" '
        'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" revision="1.0" schemaVersion="1.0" '
        'xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions">\n',
        '  <policyNamespaces>\n    <target prefix="kittypp" namespace="kapper.net.Policies.KiTTYpp" />\n  </policyNamespaces>\n',
        '  <resources minRequiredRevision="1.0" />\n',
        '  <supportedOn>\n    <definitions>\n      <definition name="SUPPORTED_KiTTYpp_0_85_1_22" '
        'displayName="$(string.SUPPORTED_KiTTYpp_0_85_1_22)" />\n    </definitions>\n  </supportedOn>\n',
        '  <categories>\n']
for cid, parent, _ in CATS:
    admx.append(f'    <category name="{cid}" displayName="$(string.Cat_{cid})">\n')
    if parent:
        admx.append(f'      <parentCategory ref="{parent}" />\n')
    admx.append('    </category>\n')
admx.append('  </categories>\n  <policies>\n')
for p in P:
    admx.append(policy_xml(p[0], p[1], p[2], p[3], p[4], False))
for p in P:
    if p[7]:
        admx.append(policy_xml(p[0], p[1], p[2], p[3], p[4], True))
admx.append('  </policies>\n</policyDefinitions>\n')

strings = [("SUPPORTED_KiTTYpp_0_85_1_22", "KiTTY++ 0.85.1.22 and later")]
for cid, _, text in CATS:
    strings.append((f"Cat_{cid}", text))
pres = []
for p in P:
    pid, vname, cat, kind, prm, display, help_, rec = p
    strings.append((pid, display))
    strings.append((pid + "_Help", help_))
    if kind in ("enum_str", "enum_dword"):
        for iid, _ in prm:
            strings.append((f"{pid}_{iid}", ENUM_TEXT[iid]))
    pres.append(presentation_xml(pid, kind, prm, False, display))
    if rec:
        strings.append((pid + "_Recommended", display))
        strings.append((pid + "_Recommended_Help", help_ + REC_NOTE))
        pres.append(presentation_xml(pid, kind, prm, True, display))

adml = ['<?xml version="1.0" encoding="utf-8"?>\n',
        '<policyDefinitionResources xmlns:xsd="http://www.w3.org/2001/XMLSchema" '
        'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" revision="1.0" schemaVersion="1.0" '
        'xmlns="http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions">\n',
        '  <displayName>KiTTY++</displayName>\n',
        '  <description>Group Policy settings of KiTTY++ and its SSH agent kageant.</description>\n',
        '  <resources>\n    <stringTable>\n']
for sid, text in strings:
    adml.append(f'      <string id="{sid}">{esc(text)}</string>\n')
adml.append('    </stringTable>\n    <presentationTable>\n')
adml.extend(x for x in pres if x)
adml.append('    </presentationTable>\n  </resources>\n</policyDefinitionResources>\n')

# --- the table of docs/GROUP-POLICY.md
def folder(cat):
    names = {cid: (parent, text) for cid, parent, text in CATS}
    chain = []
    while cat:
        parent, text = names[cat]
        chain.append(text)
        cat = parent
    return " > ".join(reversed(chain))

def values(kind, prm):
    if kind == "bool":
        return "REG_DWORD: 1 on, 0 off"
    if kind == "boolstr":
        return "REG_SZ: yes, no"
    if kind == "boolstr_inv":
        return "REG_SZ: no (Enabled), yes (Disabled)"
    if kind == "dec":
        return f"REG_DWORD: {prm[0]} to {prm[1]}, default {prm[2]}"
    if kind == "text":
        return f"REG_SZ: up to {prm} characters"
    if kind == "enum_dword":
        return "REG_DWORD: " + ", ".join(f"{v} {ENUM_TEXT[i]}" for i, v in prm)
    return "REG_SZ: " + ", ".join(f"{v}" for _, v in prm)

def cell(s): return s.replace("|", "\\|")

rows = ["", "| Policy | Folder | Value name | Type and values | Recommended |",
        "|---|---|---|---|---|"]
for pid, vname, cat, kind, prm, display, help_, rec in P:
    rows.append(f"| {cell(display)} | {cell(folder(cat))} | `{vname}` | {cell(values(kind, prm))} | {'yes' if rec else 'no'} |")
rows.append("")
doc = DOC.read_text(encoding="utf-8")
if doc.count(DOC_BEGIN) != 1 or doc.count(DOC_END) != 1 or doc.index(DOC_BEGIN) > doc.index(DOC_END):
    raise SystemExit(f"{DOC}: the two gen-admx marker lines are missing or out of order")
head, rest = doc.split(DOC_BEGIN, 1)
_, tail = rest.split(DOC_END, 1)
outputs = {
    DOC: (head + DOC_BEGIN + "\n".join(rows) + DOC_END + tail).encode("utf-8"),
    OUT / "kittypp.admx": "".join(admx).replace("\n", "\r\n").encode("utf-8"),
    OUT / "en-US" / "kittypp.adml": "".join(adml).replace("\n", "\r\n").encode("utf-8"),
}
if "--check" in sys.argv[1:]:
    # the build's check (CMakeLists.txt): the files as this script writes them
    stale = [str(f.relative_to(ROOT)) for f, b in outputs.items()
             if not f.exists() or f.read_bytes() != b]
    if stale:
        print("gen-admx.py: not as tools/gen-admx.py writes them:", ", ".join(stale))
        print("  -> run: python3 tools/gen-admx.py")
        sys.exit(1)
    # every value name still a name the programs read: a setting renamed in
    # the code leaves its policy writing a value nothing reads
    code = "".join(p.read_text(encoding="utf-8", errors="ignore")
                   for d in ("kitty", "windows") for p in (ROOT / d).glob("*.[ch]"))
    unread = [p[1] for p in P if f'"{p[1]}"' not in code]
    if unread:
        print("gen-admx.py: value names no source file in kitty/ or windows/ names:", ", ".join(unread))
        sys.exit(1)
    sys.exit(0)
(OUT / "en-US").mkdir(parents=True, exist_ok=True)
for f, b in outputs.items():
    f.write_bytes(b)
print(len(P), "settings,", sum(1 for p in P if p[7]), "recommended,", len(P) + sum(1 for p in P if p[7]), "policies")
