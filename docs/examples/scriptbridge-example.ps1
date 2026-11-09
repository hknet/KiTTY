<#
  scriptbridge-example.ps1 - play a script in a KiTTY++ terminal over the
  script bridge and wait for the host's prompt. See docs/SCRIPTING.md.

  The terminal needs both switches on:
    KiTTY++ Settings > Automation > Script bridge: Inbound + prompt event out
                (Inbound is enough without -Pattern)
    session:    Session > Scripting > "Accept script bridge messages for this session"

  Usage (Windows PowerShell 5.1 or PowerShell 7):
    powershell -File scriptbridge-example.ps1 -ProcessId 1234 -Key <script bridge key> `
        -ScriptFile commands.txt -Pattern '$ '
    powershell -File scriptbridge-example.ps1 -Session web01 -Key <script bridge key> `
        -ScriptFile commands.txt -Pattern '$ '

    -ProcessId    the kitty.exe process of the terminal, for example the one
                  your program started (Start-Process kitty.exe -PassThru)
    -Session      or: the saved session of a terminal that is already open.
                  The terminal is found by its command line "-load <session>"
                  (as the launcher starts it). Refused when no terminal or
                  more than one matches. Windows 8.1 or later.
    -Key          the script bridge key: Session > Scripting > "Script bridge
                  key" (Copy)
    -ScriptFile   the script, one line per command, as the window menu >
                  Tools > Run script file plays it
    -Pattern      optional: wait until the host's output ends with this text
                  (the prompt) and print it. Several alternatives: '"$ " "# "'

  Exit code: 0 done, 1 refused or not delivered, 2 no prompt within -TimeoutSec.
  Every refusal has its reason in the terminal's Event Log.
#>
[CmdletBinding(DefaultParameterSetName = 'ByProcess')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'ByProcess')][int]$ProcessId,
    [Parameter(Mandatory = $true, ParameterSetName = 'BySession')][string]$Session,
    [Parameter(Mandatory = $true)][string]$Key,
    [Parameter(Mandatory = $true)][string]$ScriptFile,
    [string]$Pattern    = '',
    [int]   $TimeoutSec = 30
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class KtBridge {
    public const int WM_COPYDATA = 0x004A;
    public const long ID_SEND = 5, ID_ENABLE = 6, ID_SET = 7, ID_PROMPT = 8;

    [StructLayout(LayoutKind.Sequential)]
    public struct COPYDATASTRUCT { public IntPtr dwData; public int cbData; public IntPtr lpData; }

    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")]
    static extern IntPtr SendMessageTimeoutW(IntPtr h, int msg, IntPtr w,
        ref COPYDATASTRUCT l, uint flags, uint ms, out IntPtr result);

    // The terminal window of a kitty.exe process: its visible top-level
    // window that is not a dialog box.
    public static IntPtr WindowOfProcess(uint pid) {
        IntPtr hit = IntPtr.Zero;
        EnumWindows(delegate (IntPtr h, IntPtr p) {
            uint wp;
            GetWindowThreadProcessId(h, out wp);
            if (wp != pid || !IsWindowVisible(h)) return true;
            StringBuilder c = new StringBuilder(64);
            GetClassName(h, c, 64);
            if (c.ToString() == "#32770") return true;
            hit = h;
            return false;
        }, IntPtr.Zero);
        return hit;
    }

    // The command line of a process, read from Windows itself
    // (ProcessCommandLineInformation, Windows 8.1 or later). null when it
    // cannot be read.
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("ntdll.dll")] static extern int NtQueryInformationProcess(IntPtr h, int cls, IntPtr buf, int len, out int retLen);
    public static string CommandLineOf(uint pid) {
        IntPtr h = OpenProcess(0x1000, false, pid);   // PROCESS_QUERY_LIMITED_INFORMATION
        if (h == IntPtr.Zero) return null;
        try {
            int len;
            NtQueryInformationProcess(h, 60, IntPtr.Zero, 0, out len);
            if (len <= 0) return null;
            IntPtr buf = Marshal.AllocHGlobal(len);
            try {
                if (NtQueryInformationProcess(h, 60, buf, len, out len) != 0) return null;
                // a UNICODE_STRING: Length in bytes, then the text's address
                int bytes = (ushort)Marshal.ReadInt16(buf);
                IntPtr text = Marshal.ReadIntPtr(buf, IntPtr.Size);
                return Marshal.PtrToStringUni(text, bytes / 2);
            } finally { Marshal.FreeHGlobal(buf); }
        } finally { CloseHandle(h); }
    }

    // One message: "<key>\0<rest>". The answer: 1 accepted, 0 refused
    // (the reason is in the terminal's Event Log), -1 not delivered.
    public static long SendCopyData(IntPtr term, long id, string key, byte[] rest, IntPtr wParam) {
        byte[] k = Encoding.ASCII.GetBytes(key);
        byte[] data = new byte[k.Length + 1 + rest.Length];
        Buffer.BlockCopy(k, 0, data, 0, k.Length);
        Buffer.BlockCopy(rest, 0, data, k.Length + 1, rest.Length);
        IntPtr mem = Marshal.AllocHGlobal(data.Length);
        try {
            Marshal.Copy(data, 0, mem, data.Length);
            COPYDATASTRUCT cds;
            cds.dwData = new IntPtr(id);
            cds.cbData = data.Length;
            cds.lpData = mem;
            IntPtr result;
            // SMTO_ABORTIFHUNG | SMTO_BLOCK, 10 s
            if (SendMessageTimeoutW(term, WM_COPYDATA, wParam, ref cds, 3, 10000, out result) == IntPtr.Zero)
                return -1;
            return result.ToInt64();
        } finally { Marshal.FreeHGlobal(mem); }
    }
}

// The window the terminal sends _prompt (8) to: hidden, never shown. Plain
// Win32, so it works the same in Windows PowerShell 5.1 and PowerShell 7.
public class KtReceiver : IDisposable {
    public string Matched;
    public IntPtr Handle;

    delegate IntPtr WndProcFn(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASS {
        public uint style; public WndProcFn lpfnWndProc; public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground;
        public string lpszMenuName, lpszClassName;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public int x, y; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassW(ref WNDCLASS wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y,
        int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool PeekMessageW(out MSG m, IntPtr h, uint f, uint l, uint remove);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG m);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref MSG m);
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandleW(IntPtr name);

    readonly WndProcFn proc;              // held here: the window calls it

    public KtReceiver() {
        proc = WndProc;
        WNDCLASS wc = new WNDCLASS();
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(IntPtr.Zero);
        wc.lpszClassName = "KtBridgeReceiver" + Guid.NewGuid().ToString("N");
        RegisterClassW(ref wc);
        Handle = CreateWindowExW(0, wc.lpszClassName, "", 0, 0, 0, 0, 0,
                                 IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
    }

    IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l) {
        if (m == KtBridge.WM_COPYDATA) {
            KtBridge.COPYDATASTRUCT cds = (KtBridge.COPYDATASTRUCT)
                Marshal.PtrToStructure(l, typeof(KtBridge.COPYDATASTRUCT));
            if (cds.dwData.ToInt64() == KtBridge.ID_PROMPT) {
                byte[] b = new byte[cds.cbData];
                if (cds.cbData > 0) Marshal.Copy(cds.lpData, b, 0, cds.cbData);
                Matched = Encoding.UTF8.GetString(b);
                return new IntPtr(1);
            }
        }
        return DefWindowProcW(h, m, w, l);
    }

    // Lets the terminal's message in: a sent message arrives while the
    // thread that made the window looks at its messages.
    public void Pump() {
        MSG msg;
        while (PeekMessageW(out msg, IntPtr.Zero, 0, 0, 1)) {
            TranslateMessage(ref msg);
            DispatchMessageW(ref msg);
        }
    }

    public void Dispose() {
        if (Handle != IntPtr.Zero) DestroyWindow(Handle);
        Handle = IntPtr.Zero;
    }
}
'@

# By session: the kitty.exe (or kitty_portable.exe) started with
# "-load <session>", the name alone or in double quotes
if ($PSCmdlet.ParameterSetName -eq 'BySession') {
    $esc = [regex]::Escape($Session)
    $load = '(^|\s)-load\s+("' + $esc + '"|' + $esc + ')(\s|$)'
    $found = @(Get-Process -Name kitty, kitty_portable -ErrorAction SilentlyContinue | Where-Object {
        $cl = [KtBridge]::CommandLineOf([uint32]$_.Id)
        $cl -and $cl -match $load })
    if ($found.Count -ne 1) {
        Write-Host "$($found.Count) terminals with the session '$Session' - want exactly one"
        exit 1
    }
    $ProcessId = $found[0].Id
}

# the terminal: the window of its process - never found by title
$term = [KtBridge]::WindowOfProcess([uint32]$ProcessId)
if ($term -eq [IntPtr]::Zero) { Write-Host 'no terminal window found'; exit 1 }

# bytes as they go to the host: UTF-8, right for a UTF-8 session
$utf8 = New-Object System.Text.UTF8Encoding $false
$text = [IO.File]::ReadAllText((Resolve-Path $ScriptFile), $utf8)

$rx = $null
if ($Pattern) {
    $rx = New-Object KtReceiver
    # _set (7): the prompt pattern, then _enable (6) "1": bind our window
    if ([KtBridge]::SendCopyData($term, [KtBridge]::ID_SET, $Key, $utf8.GetBytes($Pattern), [IntPtr]::Zero) -ne 1) {
        Write-Host '_set refused - see the Event Log'; exit 1
    }
    if ([KtBridge]::SendCopyData($term, [KtBridge]::ID_ENABLE, $Key, $utf8.GetBytes('1'), $rx.Handle) -ne 1) {
        Write-Host '_enable refused - see the Event Log'; exit 1
    }
}

# _send (5): the script, played by the terminal's script engine
$answer = [KtBridge]::SendCopyData($term, [KtBridge]::ID_SEND, $Key, $utf8.GetBytes($text), [IntPtr]::Zero)
if ($answer -ne 1) { Write-Host "_send refused ($answer) - see the Event Log"; exit 1 }
Write-Host 'script accepted'
if (-not $Pattern) { exit 0 }

# wait for _prompt (8), letting the message in
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not $rx.Matched -and (Get-Date) -lt $deadline) {
    $rx.Pump()
    Start-Sleep -Milliseconds 50
}
# _enable (6) "0": no more prompt messages
[void][KtBridge]::SendCopyData($term, [KtBridge]::ID_ENABLE, $Key, $utf8.GetBytes('0'), $rx.Handle)
$rx.Dispose()
if (-not $rx.Matched) { Write-Host "no prompt within $TimeoutSec s"; exit 2 }
Write-Host "prompt: $($rx.Matched)"
exit 0
