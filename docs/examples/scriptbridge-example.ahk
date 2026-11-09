; scriptbridge-example.ahk - play a script in a KiTTY++ terminal over the
; script bridge and wait for the host's prompt. AutoHotkey v2.
; See docs/SCRIPTING.md.
;
; The terminal needs both switches on:
;   KiTTY++ Settings > Automation > Script bridge: Inbound + prompt event out
;               (Inbound is enough without a pattern)
;   session:    Session > Scripting > "Accept script bridge messages for this session"
;
; Usage:
;   AutoHotkey64.exe scriptbridge-example.ahk <pid> <key> <script file> [pattern]
;   AutoHotkey64.exe scriptbridge-example.ahk -session <name> <key> <script file> [pattern]
;
;   pid          the kitty.exe process of the terminal, for example the one
;                your script started (Run's OutputVarPID)
;   -session     or: the saved session of a terminal that is already open.
;                The terminal is found by its command line "-load <session>"
;                (as the launcher starts it). Refused when no terminal or
;                more than one matches. Windows 8.1 or later.
;   key          the script bridge key: Session > Scripting > "Script bridge
;                key" (Copy)
;   script file  the script, one line per command, as the window menu >
;                Tools > Run script file plays it
;   pattern      optional: wait until the host's output ends with this text
;                (the prompt). Several alternatives: "$ " "# " in one argument
;
; Writes "script accepted" and "prompt: <pattern>" to standard output.
; Exit code: 0 done, 1 refused or not delivered, 2 no prompt within 30 s.
; Every refusal has its reason in the terminal's Event Log.

#Requires AutoHotkey v2.0
#SingleInstance Off

WM_COPYDATA := 0x004A
ID_SEND := 5, ID_ENABLE := 6, ID_SET := 7, ID_PROMPT := 8

args := A_Args.Clone()
pid := 0
if (args.Length >= 2 && args[1] = "-session") {
    session := args[2]
    args.RemoveAt(1, 2)
    hits := FindBySession(session)
    if (hits.Length != 1) {
        Out(hits.Length " terminals with the session '" session "' - want exactly one")
        ExitApp(1)
    }
    pid := hits[1]
} else if (args.Length >= 1) {
    pid := args.RemoveAt(1)
}
if (!pid || args.Length < 2) {
    Out("usage: scriptbridge-example.ahk <pid> | -session <name>  <key> <script file> [pattern]")
    ExitApp(1)
}
key := args[1], scriptFile := args[2]
pattern := args.Length >= 3 ? args[3] : ""

; the terminal: the visible window of the process - never found by title
term := WinExist("ahk_pid " pid)
if (!term) {
    Out("no terminal window found")
    ExitApp(1)
}

; the window the terminal sends _prompt (8) to: hidden, never shown
global matched := ""
rx := Gui()
OnMessage(WM_COPYDATA, OnCopyData)

if (pattern != "") {
    ; _set (7): the prompt pattern, then _enable (6) "1": bind our window
    if (Bridge(term, ID_SET, key, pattern, 0) != 1) {
        Out("_set refused - see the Event Log")
        ExitApp(1)
    }
    if (Bridge(term, ID_ENABLE, key, "1", rx.Hwnd) != 1) {
        Out("_enable refused - see the Event Log")
        ExitApp(1)
    }
}

; _send (5): the script, played by the terminal's script engine
answer := Bridge(term, ID_SEND, key, FileRead(scriptFile, "UTF-8"), 0)
if (answer != 1) {
    Out("_send refused (" answer ") - see the Event Log")
    ExitApp(1)
}
Out("script accepted")
if (pattern = "")
    ExitApp(0)

; wait for _prompt (8); Sleep lets the message arrive
deadline := A_TickCount + 30000
while (matched = "" && A_TickCount < deadline)
    Sleep(50)
; _enable (6) "0": no more prompt messages
Bridge(term, ID_ENABLE, key, "0", rx.Hwnd)
if (matched = "") {
    Out("no prompt within 30 s")
    ExitApp(2)
}
Out("prompt: " matched)
ExitApp(0)

; One message "<key>\0<rest>" (rest as UTF-8, no NUL of its own).
; The answer: 1 accepted, 0 refused, -1 not delivered.
Bridge(hwnd, id, key, rest, wParam) {
    klen := StrPut(key, "UTF-8")              ; with its NUL: the separator
    rlen := StrPut(rest, "UTF-8") - 1
    data := Buffer(klen + rlen)
    StrPut(key, data, "UTF-8")
    if (rlen > 0) {
        tmp := Buffer(rlen + 1)
        StrPut(rest, tmp, "UTF-8")
        DllCall("RtlMoveMemory", "Ptr", data.Ptr + klen, "Ptr", tmp.Ptr, "UPtr", rlen)
    }
    cds := Buffer(A_PtrSize * 3, 0)           ; COPYDATASTRUCT
    NumPut("UPtr", id, cds, 0)
    NumPut("UInt", data.Size, cds, A_PtrSize)
    NumPut("UPtr", data.Ptr, cds, A_PtrSize * 2)
    try
        return SendMessage(0x004A, wParam, cds.Ptr, , "ahk_id " hwnd, , , , 10000)
    catch
        return -1
}

OnCopyData(wParam, lParam, msg, hwnd) {
    global matched
    if (NumGet(lParam, 0, "UPtr") != 8)       ; only _prompt
        return
    cb := NumGet(lParam, A_PtrSize, "UInt")
    p := NumGet(lParam, A_PtrSize * 2, "UPtr")
    matched := cb ? StrGet(p, cb, "UTF-8") : ""
    return 1
}

Out(text) {
    try FileAppend(text "`n", "*")
}

; The kitty.exe and kitty_portable.exe processes started with
; "-load <name>", the name alone or in double quotes. One entry per process.
FindBySession(name) {
    hits := []
    for exe in ["kitty.exe", "kitty_portable.exe"] {
        for hwnd in WinGetList("ahk_exe " exe) {
            if (WinGetClass(hwnd) = "#32770")     ; a dialog box, not a terminal
                continue
            p := WinGetPID(hwnd)
            cl := CommandLineOf(p)
            if (cl = "" || !RegExMatch(cl, "i)(^|\s)-load\s+(`"\Q" name "\E`"|\Q" name "\E)(\s|$)"))
                continue
            seen := false
            for h in hits
                if (h = p)
                    seen := true
            if (!seen)
                hits.Push(p)
        }
    }
    return hits
}

; The command line of a process, read from Windows itself
; (ProcessCommandLineInformation, Windows 8.1 or later). "" when unreadable.
CommandLineOf(pid) {
    h := DllCall("OpenProcess", "UInt", 0x1000, "Int", 0, "UInt", pid, "Ptr")   ; QUERY_LIMITED
    if (!h)
        return ""
    cl := "", len := 0
    DllCall("ntdll\NtQueryInformationProcess", "Ptr", h, "Int", 60, "Ptr", 0, "UInt", 0, "UInt*", &len)
    if (len > 0) {
        buf := Buffer(len, 0)
        if (DllCall("ntdll\NtQueryInformationProcess", "Ptr", h, "Int", 60, "Ptr", buf, "UInt", len, "UInt*", &len) = 0) {
            bytes := NumGet(buf, 0, "UShort")              ; a UNICODE_STRING
            cl := StrGet(NumGet(buf, A_PtrSize, "Ptr"), bytes // 2, "UTF-16")
        }
    }
    DllCall("CloseHandle", "Ptr", h)
    return cl
}
