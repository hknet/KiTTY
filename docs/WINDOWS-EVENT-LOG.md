# KiTTY++ in the Windows Event Log

KiTTY++ can write what its binaries do - sessions (SSH, telnet, raw, rlogin,
SUPDUP, serial lines), host keys, logins, the proxies and jump hosts a
connection went through, every transferred file, key generation, and every
action of the SSH agent kageant - to the Windows Event Log. Each kind of
record has its own event ID, and each field is its own insertion string in
the event data, so Event Viewer filters, scripts and forwarding can select on
single fields.

The records carry host names, user names, file paths and key fingerprints,
and the Windows account (SID) of the program that wrote them - the Event
Viewer's *User* column.

## 1. What is written where

| Program | Source | Event IDs | Log (system MSI installed) | Default |
|---|---|---|---|---|
| kageant (agent log) | `KiTTY++_agent` | 100-199 | `KiTTY++_Agent` | on when the log `KiTTY++_Agent` exists (file and Event Log); otherwise file only |
| kitty.exe, kitty_tel sessions | `KiTTY++_kitty` | 200-299 | `KiTTY++_Session` | off |
| klink, kscp, ksftp | `KiTTY++_cli` | 300-399 | `KiTTY++_Session` | off |
| kittygen, kittygen-cli | `KiTTY++_kittygen` | 400-499 | `KiTTY++_Session` | off |
| the suite | `KiTTY++` | 0, 900-999 | `Application` | always written |

Both logs are in *Applications and Services Logs* in Event Viewer.

**Without the system-wide MSI** (per-user MSI, portable copy) neither log
exists and none of the sources is registered. The programs write the same
records with the same event IDs and insertion strings regardless; Windows puts
the events of an unregistered source into the **Application** log, with the
same event data.

**After uninstalling the system MSI, restart Windows.** Until then the Event
Log service keeps sending the KiTTY++ sources to the logs that are no longer
registered (`KiTTY++_Session.evtx`, `KiTTY++_Agent.evtx` stay on disk), not to
the Application log. Restarting only the Event Log service is no substitute:
on Windows 11 it left the service taking no records at all until a restart of
Windows.

The suite's own records (source `KiTTY++`) differ: where kitty.exe is not
registered as the message file of `KiTTY++`, events 900-902 are written as
event ID 0 with one readable line of text instead of fields.

kageant decides its default once, when it starts: with the log `KiTTY++_Agent`
present it writes the agent log to the file **and** the Event Log, otherwise to
the file only. An explicit setting (section 2) always wins; `Event Log` or
`file and Event Log` without the system MSI writes to the Application log.

## 2. Turning it on

### kitty.exe, kitty_tel, klink/kscp/ksftp, kittygen

Configuration box: *Application > Security*, group **Windows Event Log**:

| Checkbox | `[KiTTY]` key in `kitty.ini` | Covers |
|---|---|---|
| **kitty.exe sessions** | `sessioneventlog` | IDs 200-241, kitty.exe and kitty_tel |
| **klink, kscp, ksftp** | `clieventlog` | IDs 300-331 |
| **kittygen** | `kittygeneventlog` | IDs 400-403, kittygen and kittygen-cli |

kittygen-cli (the console kittygen) writes the same records 400-403 as the
kittygen window, with `tool` `kittygen-cli`.

Values `yes` / `no`, default `no`:

```ini
[KiTTY]
sessioneventlog=yes
clieventlog=yes
kittygeneventlog=yes
```

Each program reads its switch once, when it starts; a change reaches the
programs started after it. Outside portable mode the registry value of the same
name under `HKCU\Software\kapper.net\KiTTY` takes precedence over `kitty.ini`,
and the checkboxes write there - see
[KITTY-INI.md](KITTY-INI.md#when-an-edited-kittyini-appears-to-do-nothing).

The button **KiTTY++ Event Viewer** in the same group opens Event Viewer on a
view of the suite's records: the sources `KiTTY++`, `KiTTY++_kitty`,
`KiTTY++_cli` and `KiTTY++_kittygen` in the Application log, plus the log
`KiTTY++_Session` when it exists.

### kageant

Key list window: **Settings...** (or the tray menu's **Settings...**), page
**Log**:

- **Write the agent log** - the agent log as a whole (`agentlog`, default `yes`).
- **Destination:** - `file`, `Event Log` or `file and Event Log`.
- **Event Viewer** - Event Viewer on the source `KiTTY++_agent`, in the log
  `KiTTY++_Agent` when it exists and in the Application log. The agent log
  window (tray: **Agent log...**) has the same button.

In `kitty.ini`, section `[Agent]`:

```ini
[Agent]
agentlog=yes
; file | eventlog | both; absent = both with the system MSI, else file
agentlogsink=both
```

`agentlogsink` - where the agent log (sink) is sent (the Settings field *Destination*):
`file`, `eventlog` or `both`.

Every `[Agent]` key is listed in [`kitty.ini.example`](examples/kitty.ini.example).

## 3. The logs

The **system-wide MSI** creates both logs and registers the sources; the
per-user MSI and the portable copy create nothing.

| Log | Sources | MaxSize | Retention |
|---|---|---|---|
| `KiTTY++_Agent` | `KiTTY++_agent` | 20 MB (20971520 bytes) | 0: oldest records overwritten when full |
| `KiTTY++_Session` | `KiTTY++_kitty`, `KiTTY++_cli`, `KiTTY++_kittygen` | 20 MB (20971520 bytes) | 0: oldest records overwritten when full |

The source `KiTTY++` is registered in the Application log, with kitty.exe as
the message file of every source.

Current settings:

```
wevtutil gl KiTTY++_Agent
wevtutil gl KiTTY++_Session
```

To change the size, for example to 50 MB (elevated prompt required):
In order to get an elevated shell right-click the powershell icon
and select Run as `Administrator`.

```
wevtutil sl KiTTY++_Session /ms:52428800
```

or in Event Viewer: the log's *Properties*. Without the system MSI the records
are in the Application log, whose size and retention are Windows' own settings.

- **Major upgrade** of the system MSI: the size and retention found on the
  machine are kept and will not reset to the above defaults (20 MB, overwrite).
- **Uninstall** removes the registry keys of both logs under
  `HKLM\SYSTEM\CurrentControlSet\Services\EventLog`, with the sources under
  them. The `.evtx` files under `%SystemRoot%\System32\winevt\Logs` are
  Windows' own and are not removed.

## 4. Event IDs

48 IDs:

| Range | Source | Records |
|---|---|---|
| 0 | `KiTTY++` | text only: the one string is the message |
| 100-199 | `KiTTY++_agent` | sign (100), confirm (101), add (110), load at startup (111), forget a path (112), retry (113), remove (114), remove all (115), lock (120), unlock (121), Windows Hello enrol/protect/unlock (130-132), key file warning (140), re-encrypt all (150), re-encrypt one key (151), decrypt a key (152), agent start/stop (160), log rotation (170) |
| 200-299 | `KiTTY++_kitty` | session opened (200) and closed (201), host key known (210), new (211), mismatch (212), login succeeded (220) and failed (221), proxy used (225), far-end request refused (230), file transferred (240) and failed (241) - ZModem and kitten transfers |
| 300-399 | `KiTTY++_cli` | 300-325 as 200-225; file transferred (330) and failed (331) |
| 400-499 | `KiTTY++_kittygen` | key generated (400), private key saved (401), public key saved (402), passphrase changed (403) |
| 900-999 | `KiTTY++` | program file refused (900), log retention (901), missing Windows features (902) |

In every record `%1` is the kind (`session-open`, `auth-failed`, ...) and `%2`
onwards are the fields in a fixed order. Refusals, failures and mismatches are
Warning, 900 is Error, the rest are classified as Information.

- **Transfers:** one record per file (240/241, 330/331), with `direction`
  (`put`, `get`), `local`, `remote` and `bytes`.
- **Logins:** 221 and 321 are written for each attempt the server refuses.
- **Proxies and jump hosts:** 210-221 and 310-321 carry `proxy` - empty for
  the connection's own host key and login, else the proxy's type (`ssh` for a
  jump host, `http`, `socks5`, `socks4`). 225/325 record the proxy itself:
  type (`http`, `socks4`, `socks5`, `telnet`, `local-command`), host, port,
  user, and `credentials` (`yes` when a user name or password went to it).
  `for` in 220, 221, 225, 320, 321, 325: empty for the connection's own
  login; `session` for a jump host's login and for a proxy the session went
  through; `forward` for a port forwarding's connection, `x11` for an X11
  forwarding's.
- **SSH-1:** `keytype` `rsa1`; `fp` the MD5 hash (`aa:bb:...`).
- **Serial lines:** `proto` `serial`, `host` the line (`COM3`), `port` the
  speed.

The full list - every ID with its fields in order, and the meaning and values
of each field - is in [EVENT-IDS.md](EVENT-IDS.md), and the same list is in
the manual's appendix *Windows Event Log event IDs* (`kitty.chm`). Both are
generated from one table in the source and checked on every build, so a
change of an event ID or field shows in the history of EVENT-IDS.md.

Examples:

```powershell
Get-WinEvent -LogName 'KiTTY++_Session' -MaxEvents 20
Get-WinEvent -FilterHashtable @{ LogName='KiTTY++_Session'; Id=221,321 }
Get-WinEvent -FilterHashtable @{ LogName='KiTTY++_Agent'; Level=3 }
Get-WinEvent -LogName Application -FilterXPath "*[System[Provider[@Name='KiTTY++_kitty' or @Name='KiTTY++_cli' or @Name='KiTTY++_kittygen']]]"
```

`Level=3` selects Warning. The last one is for a machine without the system
MSI: there the sources are not registered, and `-FilterHashtable` with
`ProviderName` refuses an unregistered provider ("There is not an event
provider ... that matches"); the XPath form selects on the name in the
record.

A `Get-WinEvent` query that matches nothing ends in an error ("No events were
found that match the specified selection criteria."), not in empty output.

## 5. Read access

The installer sets no `CustomSD` on either log. With no `CustomSD`, members of
the local group **Event Log Readers** (`S-1-5-32-573`) can read both
`KiTTY++_Agent` and `KiTTY++_Session`. That membership is the access an account
collecting or forwarding the records needs.

```
reg query "HKLM\SYSTEM\CurrentControlSet\Services\EventLog\KiTTY++_Session" /v CustomSD
Add-LocalGroupMember -SID S-1-5-32-573 -Member <DOMAIN\account>
```

The first ends in an error when no `CustomSD` is set. The second (elevated
PowerShell) adds the collecting account to Event Log Readers.

## 6. Forwarding

The records are standard Windows Event Log records, so Windows Event Forwarding
or a log shipper that reads the Event Log can move them off the machine.
KiTTY++ sends no syslog itself.

Two machines take part: the **source** (where KiTTY++ runs and writes the
records) and the **collector** (where the records are gathered, in its log
*Forwarded Events*). Source and collector must be two different machines
(6e).

### 6a. Domain: source-initiated subscription [UNTESTED]

The usual setup in a domain: Group Policy points the sources at the collector,
and the sources connect to it over WinRM, HTTP port 5985, with Kerberos. Not
tested with KiTTY++. The query of the subscription in 6b applies unchanged.
Setup:
[Use Windows Event Forwarding to help with intrusion detection](https://learn.microsoft.com/en-us/windows/security/operating-system-security/device-management/use-windows-event-forwarding-to-assist-in-intrusion-detection).

### 6b. Workgroup: collector-initiated, Pull (tested)

Tested with Windows 11 as the source (system MSI installed, not in a domain)
and a second Windows 11 machine as the collector (a domain member, the source
not in its domain).

How it works: the collector connects to the source over WinRM (Windows
Remote Management, the Windows service for remote administration over HTTP),
logs on with a local account of the source, and pulls the records.

Placeholders:

- `<SourceIP>` - the source's IP address, as the collector reaches it
- `<SourceName>` - the source's computer name (`$env:COMPUTERNAME` on the source)
- `<ReaderUser>`, `<ReaderPassword>` - the local account on the SOURCE that
  the collector logs on with: created on the source in step 4, handed to the
  collector in step 9
- `<IfIndex>` - the InterfaceIndex of the source's network adapter that holds
  `<SourceIP>` (step 1)
- `<SubscriptionFile>` - the XML of step 8, saved as a file on the
  COLLECTOR, for example `C:\wef\kittypp-subscription.xml`

**Every command below runs in an elevated PowerShell** (*Run as
administrator*), on the machine named in the step.

#### Step 1 - source: network category Private

Why: Windows puts every network connection in a category - Public, Private or
Domain. The WinRM firewall exception that step 2 enables covers Domain and
Private only; the rule for Public stays off. A
source whose connection is Public cannot be reached on WinRM by the collector.
A network Windows does not recognise ("Unidentified network") is Public.

See the category and the InterfaceIndex:

```powershell
Get-NetConnectionProfile
(Get-NetIPAddress -IPAddress <SourceIP>).InterfaceIndex
```

`Get-NetConnectionProfile` lists each connection, for example:

```
Name                     : Unidentified network
InterfaceAlias           : Ethernet 2
InterfaceIndex           : 5
NetworkCategory          : Public
DomainAuthenticationKind : None
IPv4Connectivity         : NoTraffic
IPv6Connectivity         : NoTraffic
```

The second line prints `<IfIndex>` (here `5`). Set it Private (no output):

```powershell
Set-NetConnectionProfile -InterfaceIndex <IfIndex> -NetworkCategory Private
```

Check: `Get-NetConnectionProfile -InterfaceIndex <IfIndex>` shows the same
block with `NetworkCategory          : Private`.

Can fail: a connection still Public later shows on the collector as
LastError 0x80338126 (connection by IP address) or 0x80338169 (by name;
the message names the Public connection). Fix: this step.

#### Step 2 - source: WinRM on

Why: the collector reaches the source through WinRM. This starts the WinRM
service (delayed automatic start), creates its listener on port 5985 and
enables the firewall exception.

```
winrm quickconfig -q
```

On a machine where WinRM was off it prints what it changes, ending with:

```
WinRM service type changed successfully.
WinRM service started.
...
WinRM firewall exception enabled.
Configured LocalAccountTokenFilterPolicy to grant administrative rights remotely to local users.
```

`winrm quickconfig` also sets `LocalAccountTokenFilterPolicy`: remote logons
of local administrators get their full rights. The forwarding does not need
it (the account of step 4 is no administrator).

Check, on the source:

```powershell
Test-WSMan localhost
```

Expected:

```
wsmid           : http://schemas.dmtf.org/wbem/wsman/identity/1/wsmanidentity.xsd
ProtocolVersion : http://schemas.dmtf.org/wbem/wsman/1/wsman.xsd
ProductVendor   : Microsoft Corporation
ProductVersion  : OS: 0.0.0 SP: 0.0 Stack: 3.0
```

From the collector: `Test-NetConnection <SourceIP> -Port 5985` shows
`TcpTestSucceeded : True`.

Can fail: `winrm` (`winrm.cmd`) is a VBScript. On a Windows without the
VBScript feature it ends with `Input Error: There is no script engine for
file extension ".vbs".` Then use instead:

```powershell
Set-WSManQuickConfig -SkipNetworkProfileCheck -Force
```

It prints `WinRM has been updated for remote management.` and
`WinRM firewall exception enabled.`; then the same check. Step 3 then takes its `Set-Item` form (`winrm` is the
same VBScript).

#### Step 3 - source: WinRM's compatibility listener on port 80

Why: the collector pulls the records on port 80 of the source, not on 5985,
and the subscription cannot name a port (step 8). WinRM listens on port 80
only with its compatibility listener on.

```powershell
winrm set winrm/config/service '@{EnableCompatibilityHttpListener="true"}'
Enable-NetFirewallRule -Name 'WINRM-HTTP-Compat-In-TCP'
```

`winrm set` prints the whole service configuration, with the line
`EnableCompatibilityHttpListener = true`; `Enable-NetFirewallRule` prints
nothing. `WINRM-HTTP-Compat-In-TCP` is the firewall rule of the Private
profile (`WINRM-HTTP-Compat-In-TCP-NoScope` is the Domain profile's, not
needed here). Equivalent, without `winrm` (both print nothing):

```powershell
Set-Item WSMan:\localhost\Service\EnableCompatibilityHttpListener -Value true -Force
Enable-NetFirewallRule -Name 'WINRM-HTTP-Compat-In-TCP'
```

Check, on the source:

```powershell
Get-NetFirewallRule -Name 'WINRM-HTTP-Compat-In-TCP*'
Get-NetTCPConnection -LocalPort 80 -State Listen
```

Expected: for `Name : WINRM-HTTP-Compat-In-TCP` the lines
`Enabled : True` and `Profile : Private` (the `-NoScope` rule is listed too,
with `Profile : Domain`); and a socket listening on port 80 of process 4:

```
LocalAddress                        LocalPort RemoteAddress                       RemotePort State       AppliedSetting OwningProcess
------------                        --------- -------------                       ---------- -----       -------------- -------------
::                                  80        ::                                  0          Listen                     4
```

From the collector: `Test-NetConnection <SourceIP> -Port 80` shows
`TcpTestSucceeded : True`.

Can fail: without this step the source stays at `Trying` with LastError
0x80338126 on the collector (step 10), and no new records arrive.

#### Step 4 - source: the account the collector logs on with

Why: the collector reads the logs with a local account of the source. The
account needs read access to the logs and nothing more: membership in the
built-in group **Event Log Readers** (section 5). Windows translates the
group's name (German: *Ereignisprotokollleser*), so the command names it by
its SID, `S-1-5-32-573`, which is the same in every language.

```powershell
net user <ReaderUser> <ReaderPassword> /add
Add-LocalGroupMember -SID S-1-5-32-573 -Member <SourceName>\<ReaderUser>
```

`net user` prints `The command completed successfully.`;
`Add-LocalGroupMember` prints nothing. Check:

```powershell
Get-LocalGroupMember -SID S-1-5-32-573
```

Expected:

```
ObjectClass Name                      PrincipalSource
----------- ----                      ---------------
User        <SourceName>\<ReaderUser> Local
```

Can fail: with a password longer than 14 characters `net user` asks
`The password entered is longer than 14 characters. Computers with Windows
prior to Windows 2000 will not be able to use this account. Do you want to
continue this operation? (Y/N) [Y]:`. In a script nobody answers: it ends
with `No valid response was provided.`, and the account is not created.
Fix: 14 characters or less, or answer the question.

Event Log Readers alone is enough: membership in Administrators or Remote
Management Users is not needed.

#### Step 5 - source: KiTTY++'s event logging on

Why: KiTTY++ writes to the Event Log only where it is switched on (section 2).
Off, there is nothing to forward. On the source, in `kitty.ini` (or with the
checkboxes of section 2):

```ini
[KiTTY]
sessioneventlog=yes
clieventlog=yes
kittygeneventlog=yes

[Agent]
agentlog=yes
agentlogsink=both
```

`sessioneventlog`: kitty.exe and kitty_tel. `clieventlog`: klink, kscp,
ksftp. `kittygeneventlog`: kittygen, kittygen-cli. `agentlog` with
`agentlogsink` `eventlog` or `both`: kageant. Outside portable mode the
registry value of the same name takes precedence (section 2). Each program
reads its switch when it starts: restart it after the change.

Check, after a session or a key operation:

```powershell
Get-WinEvent -LogName 'KiTTY++_Session' -MaxEvents 5
Get-WinEvent -LogName 'KiTTY++_Agent' -MaxEvents 5
```

Expected: records with `ProviderName` `KiTTY++_kitty`, `KiTTY++_cli`,
`KiTTY++_kittygen` or `KiTTY++_agent`.

Can fail:

- `No events were found that match the specified selection criteria.` -
  the switch is off, or the program was started before it was switched on.
- `There is not an event log on the localhost computer that matches
  "KiTTY++_Session".` - the system MSI is not installed; the records are in
  the Application log (6c).

#### Step 6 - collector: Event Collector service on

Why: the Windows Event Collector service (Wecsvc) runs the subscription.

```
wecutil qc /q
```

It prints `Windows Event Collector service was configured successfully.`,
`$LASTEXITCODE` is `0`. Check:

```powershell
Get-Service Wecsvc
```

Expected:

```
Status   Name               DisplayName
------   ----               -----------
Running  Wecsvc             Windows Event Collector
```

#### Step 7 - collector: the source in TrustedHosts

Why: outside a domain there is no Kerberos, and the collector logs on with
the source's account by Negotiate. The WinRM client sends such a logon only
to hosts in its list **TrustedHosts**.

`WSMan:` is a PowerShell drive that holds WinRM's configuration, like a
folder tree; it is there while the WinRM service runs. `Set-Item` on
`WSMan:\localhost\Client\TrustedHosts` adds the source's IP address to the
WinRM client's list of hosts it may log on to without Kerberos;
`-Concatenate` keeps the entries already there.

```powershell
Start-Service WinRM
Set-Item WSMan:\localhost\Client\TrustedHosts -Value <SourceIP> -Concatenate -Force
```

Check:

```powershell
Get-Item WSMan:\localhost\Client\TrustedHosts
```

Both commands print nothing. Expected from the check (`Value` empty before
step 7):

```
   WSManConfig: Microsoft.WSMan.Management\WSMan::localhost\Client

Type            Name                           SourceOfValue   Value
----            ----                           -------------   -----
System.String   TrustedHosts                                   <SourceIP>
```

Can fail: without the entry, step 10 shows a Negotiate error
(0x8009030e, "A specified logon session does not exist") with a hint to
add the server to TrustedHosts; with `CredentialsType` `Default` instead of
`Negotiate`: 0x803380e4.

#### Step 8 - collector: the subscription file

Save as `<SubscriptionFile>`, UTF-8:

```xml
<Subscription xmlns="http://schemas.microsoft.com/2006/03/windows/events/subscription">
  <SubscriptionId>KiTTYpp</SubscriptionId>
  <SubscriptionType>CollectorInitiated</SubscriptionType>
  <Description>KiTTY++ agent and session records</Description>
  <Enabled>true</Enabled>
  <Uri>http://schemas.microsoft.com/wbem/wsman/1/windows/EventLog</Uri>
  <ConfigurationMode>Custom</ConfigurationMode>
  <Delivery Mode="Pull">
    <Batching>
      <MaxItems>1</MaxItems>
      <MaxLatencyTime>1000</MaxLatencyTime>
    </Batching>
    <PushSettings>
      <Heartbeat Interval="60000"/>
    </PushSettings>
  </Delivery>
  <Query><![CDATA[<QueryList><Query Id="0"><Select Path="KiTTY++_Agent">*</Select><Select Path="KiTTY++_Session">*</Select></Query></QueryList>]]></Query>
  <ReadExistingEvents>true</ReadExistingEvents>
  <TransportName>HTTP</TransportName>
  <ContentFormat>RenderedText</ContentFormat>
  <Locale Language="en-US"/>
  <LogFile>ForwardedEvents</LogFile>
  <CredentialsType>Negotiate</CredentialsType>
  <EventSources>
    <EventSource Enabled="true">
      <Address><SourceIP></Address>
    </EventSource>
  </EventSources>
</Subscription>
```

- `CollectorInitiated` and `Delivery Mode="Pull"`: the collector fetches.
  Push (`ConfigurationMode` `MinLatency`) needs a domain: outside one it
  fails with 0x8033813d.
- `<Address>`: the host alone - no `http://`, no port, no `/wsman`. With a
  port, in the tests `wecutil cs` (step 9) ended in exit 15080 and the
  source showed 0x803380d9 (connection string format). Not reproduced on the
  stand-in collector of the output check: there `wecutil cs` took
  `<SourceIP>:5985` with exit 0, and the source, not reachable from it,
  showed `LastError: 80338126`.
- `<Locale Language="en-US"/>`: KiTTY++'s message texts exist in English only
  (message table language 0x0409). A collector with another display language
  (de-AT, de-DE, ...) asks for the text in its own language, which does not
  exist: the records still arrive, with their fields, but without their text.
  Other English variants are not tested: use `en-US`.
- `<ReadExistingEvents>true`: the records already in the logs are fetched
  too.

#### Step 9 - collector: create the subscription, hand it the account

```
wecutil cs <SubscriptionFile>
wecutil ss KiTTYpp /cun:<SourceName>\<ReaderUser> /cup:<ReaderPassword>
```

The first creates the subscription from the file; the second gives it the
source's account of step 4.

Check: both print nothing, `$LASTEXITCODE` is `0`. `wecutil es` lists
`KiTTYpp`.

Can fail: exit 15080 (`Error = 0x3ae8`) - the subscription is saved but
cannot be activated; `wecutil gr KiTTYpp` (step 10) shows why. (Met in the
tests; not reproduced in the output check.)

A subscription made without `<Locale>` gets it afterwards (both print
nothing):

```
wecutil ss KiTTYpp /l:en-US
Restart-Service Wecsvc -Force
```

#### Step 10 - collector: check

```
wecutil gr KiTTYpp
Get-WinEvent -LogName ForwardedEvents -MaxEvents 20
```

`wecutil gr` shows the subscription and, per source, `RunTimeStatus` and
`LastError`. Expected, once the records flow (from the forwarding test):

```
Subscription: KiTTYpp
	RunTimeStatus: Active
	Events processed: 795
	FirstEventBatchTime: 2026-10-22T21:44:12.821
	LastError: 0
	EventSources:
		<SourceIP>
			RunTimeStatus: Active
			LastError: 0
```

`Get-WinEvent` then lists records with `ProviderName` `KiTTY++_agent`,
`KiTTY++_kitty`, `KiTTY++_cli` or `KiTTY++_kittygen`, each with its text.
Before the first record it prints `Get-WinEvent : No events were found that
match the specified selection criteria.`

Right after step 9 the source shows `RunTimeStatus: Trying` with
`LastError: 0`. A source that cannot be reached stays at `Trying`, with
`LastError` in hexadecimal without `0x`, the message and the times:

```
		<SourceIP>
			RunTimeStatus: Trying
			LastError: 80338126
			ErrorMessage: <f:WSManFault xmlns:f="http://schemas.microsoft.com/wbem/wsman/1/wsmanfault" Code="2150859046" Machine="<CollectorName>"><f:Message>WinRM cannot complete the operation. Verify that the specified computer name is valid, that the computer is accessible over the network, and that a firewall exception for the WinRM service is enabled and allows access from this computer. By default, the WinRM firewall exception for public profiles limits access to remote computers within the same local subnet. </f:Message></f:WSManFault>
			ErrorTime: 2026-10-23T14:33:45.319
			NextRetryTime: 2026-10-23T14:38:45.319
```

A source at `Trying` is retried at `NextRetryTime` (5 minutes on);
`Restart-Service Wecsvc -Force` retries at once.

The error codes met in the tests (`wecutil gr` prints `LastError` without
`0x`):

| LastError / exit | Meaning | Fix |
|---|---|---|
| 0x80338126 | cannot connect to the source | source not Public (step 1), port-80 listener and its rule (step 3), WinRM on (step 2) |
| 0x80338169 | the WinRM firewall exception does not work while a connection is Public | step 1 |
| 0x803380d9 | `<Address>` not in the expected format | host alone, no scheme or port (step 8) |
| 0x8033813d | push needs a domain | `Delivery Mode="Pull"`, `ConfigurationMode` `Custom` (step 8) |
| 0x803380e4 | not Kerberos and not in a domain: HTTPS or TrustedHosts needed | `CredentialsType` `Negotiate` and TrustedHosts (step 7) |
| 0x80338012 | cannot connect: source and collector on the same machine | two machines (6e) |
| 32 | "The request is not supported." - `localhost` as `<Address>` | two machines (6e) |
| 5 | "Access is denied." - `localhost` as `<Address>`, a non-administrator account in Event Log Readers (the output check's stand-in collector) | two machines (6e) |
| exit 15080 (0x3ae8) | saved but not activated | `wecutil gr KiTTYpp` names the cause |

Shown verbatim in the output check (an English Windows 11 as a stand-in
collector, the source not reachable): 80338126 and 5. The other rows are
from the tests; their `ErrorMessage` texts were not kept.

### 6c. KiTTY++ without the system MSI: the Application log

For KiTTY++ from the portable ZIP or installed per user - without the
system-wide MSI. There the logs `KiTTY++_Agent` and `KiTTY++_Session` do not
exist and the records go to the source's Application log.

Tested the same way as 6b (no MSI installed, after a restart of Windows):
steps 1-10 unchanged, only the `<Query>` selects the KiTTY++ sources in the
Application log:

```xml
<Query><![CDATA[<QueryList><Query Id="0"><Select Path="Application">*[System[Provider[@Name='KiTTY++' or @Name='KiTTY++_agent' or @Name='KiTTY++_kitty' or @Name='KiTTY++_cli' or @Name='KiTTY++_kittygen']]]</Select></Query></QueryList>]]></Query>
```

Every record arrives with its fields and the writer's account (SID), but
without its text: no source is registered, so the source cannot format the
message (no `RenderingInfo`). The step 5 check reads the Application log:
`Get-WinEvent -LogName Application -FilterXPath "*[System[Provider[@Name='KiTTY++_kitty' or @Name='KiTTY++_cli' or @Name='KiTTY++_kittygen']]]"`.
It lists the records under `ProviderName: KiTTY++_kitty` (and the other
sources) with `TimeCreated` and `Id`, the `Message` column empty - the same
missing text.

### 6d. What the forwarded records carry

The fields as insertion strings, in the same order as on the source, and the
Windows account (SID) of the program that wrote them.

### 6e. Source and collector on the same machine

Never worked. `localhost` as `<Address>` is refused (LastError 32, "The
request is not supported."; with a non-administrator account LastError 5,
"Access is denied."); by name or address the collector got 0x80338012, with
every credential type and group membership tried. Use two machines.
