# KiTTY++ in the Windows Event Log

KiTTY++ can write what its binaries do - sessions (SSH, telnet, raw, rlogin,
SUPDUP, serial lines), host keys, logins, the proxies and jump hosts a
connection went through, every transferred file, key generation, and every
action of the SSH agent kageant - to the Windows Event Log. Each kind of
record has its own event ID, and each field is its own insertion string in
the event data, so Event Viewer filters, scripts and forwarding can select on
single fields.

The Terminal Window's Event Log (system menu > Event Log) stays in the
window and is gone when the window closes. No setting copies it to the
Windows Event Log. The setting sessioneventlog writes separate records
there (200-241).

The records carry host names, user names, file paths and key fingerprints,
and the Windows account (SID) of the program that wrote them - the Event
Viewer's *User* column.

## 1. What is written where

| Program | Source | Event IDs | Log (system MSI installed) | Default |
|---|---|---|---|---|
| kageant (agent log) | `KiTTY++_agent` | 100-199 | `KiTTY++_Agent` | on when the log `KiTTY++_Agent` exists (file and Windows Event Log), otherwise file only |
| kitty.exe, kitty_tel sessions | `KiTTY++_kitty` | 200-299 | `KiTTY++_Session` | off |
| klink, kscp, ksftp | `KiTTY++_cli` | 300-399 | `KiTTY++_Session` | off |
| kittygen, kittygen-cli | `KiTTY++_kittygen` | 400-499 | `KiTTY++_Session` | off |
| the suite | `KiTTY++` | 900-999 | `Application` | always written |

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

The suite's own records (source `KiTTY++`) are no exception: 900-907 keep
their event IDs and fields without the system MSI too. There the Event Viewer
shows "The description for Event ID ... cannot be found" with the field values
in the order of [EVENT-IDS.md](EVENT-IDS.md). Event ID 0 (one line of text) is
no longer written. Records of earlier versions still show their text where the
system MSI is installed.

kageant decides its default once, when it starts: with the log `KiTTY++_Agent`
present it writes the agent log to the file **and** the Windows Event Log,
otherwise to the file only. An explicit setting (section 2) always wins.
`Windows Event Log` or `file and Windows Event Log` without the system MSI
writes to the Application log.

With the agent log on, the anchors of its chain (event ID 171) go to the
Windows Event Log with every destination, `file` included: to
`KiTTY++_Agent` where it exists, else to the Application log. kageant's
settings show this under the Log page (section 7).

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
- **Destination:** - `file`, `Windows Event Log` or `file and Windows Event Log`.
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

The IDs:

| Range | Source | Records |
|---|---|---|
| 0 | `KiTTY++` | text only, no longer written: listed for the records of earlier versions |
| 100-199 | `KiTTY++_agent` | sign (100), confirm (101), session-bind (102), refused request (103), refused by a key's destination rules (104), add (110), load at startup (111), forget a path (112), retry (113), remove (114), remove all (115), rules changed in the rule editor (116), lock (120), unlock (121), Windows Hello enrol/protect/unlock (130-132), key file warning (140), a certificate expiring, expired or not yet valid (141), re-encrypt all (150), re-encrypt one key (151), decrypt a key (152), agent start/stop (160), log rotation (170), log chain anchor (171), a record repeated (172) |
| 200-299 | `KiTTY++_kitty` | session opened (200) and closed (201), host key known (210), new (211), mismatch (212), host key update (213), login succeeded (220) and failed (221), proxy used (225), far-end request refused (230), file transferred (240) and failed (241) - ZModem and kitten transfers |
| 300-399 | `KiTTY++_cli` | 300-325 as 200-225; file transferred (330) and failed (331) |
| 400-499 | `KiTTY++_kittygen` | key generated (400), private key saved (401), public key saved (402), passphrase changed (403) |
| 900-999 | `KiTTY++` | program file refused (900), log retention (901), missing Windows features (902), session not saved (903), store access rights not set (904), program file marked red on the Applications panel (905), helper program refused, allowed or declined (906), session log not written without a desktop (907) |

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
- **Destination (kageant):** 100, 101, 103 and 104 carry `dest`, the server of
  the client's verified session-bind (`SHA256:...`, then a host name when one
  is found), and `fwd`, the agent forwarding hops. Both are empty without a
  session-bind. The session-bind itself is event ID 102, kind `session-bind`.
- **Restricted keys (kageant):** a request refused because of a key's
  destination rules is event ID 104, kind `restricted`, with the reason as a
  token and the key's rules in `rule`. 110 carries the rules of the key added
  in `restrict`.
- **Serial lines:** `proto` `serial`, `host` the line (`COM3`), `port` the
  speed.
- **The agent log's chain (kageant):** every agent record 100-172 ends with
  `seq` (its line in the chain of the agent log file), `sig` (the signature
  of that line), `ts` and `prev` (the line's time and the hash of the line
  before it), all four empty when the file is not written. The event alone
  gives back the signed line (section 7).
- **Repeated records (kageant):** a record with the same kind and fields as
  the last one written in full, the time aside, is counted and not written.
  Event ID 172 (`repeated`) stands for the repeats: `count`, the time of the
  `first` and the `last` repeat, and `of`, the `seq` of the record repeated.

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

**Positions never change.** A field keeps its position (`%2`, `%3`, ...)
for as long as its ID exists: positions are never reordered or reused, a new
field only takes a new position at the end of its ID's list. Section 8 has
every position of every ID. Where no message table is registered (the
Application log, without the system MSI) that table is the mapping of the
insertion strings. The build holds the writers, the message texts and the
manual to the one table in the source. It holds every list to the one of
the last release (`docs/event-fields-baseline.txt`).

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
or a log shipper that reads the Windows Event Log can move them off the machine.
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

Why: KiTTY++ writes to the Windows Event Log only where it is switched on (section 2).
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

## 7. The agent log's chain and its anchors

kageant chains the lines of the agent log file: every line ends with `seq`
(a counter over the whole log, continued across runs and files), `prev`
(the SHA-256 of the line before it, exactly as on disk, base64) and `sig`.
`sig` is an Ed25519 signature over the line up to its `sig` field, `prev`
included, with a key kageant makes at its start and holds in its memory
only. At its start, at each rotation, at a stop, every 15 minutes while
records were written and before a change of `agentlog`, `agentlogpath` or
`agentlogsink` takes effect, kageant writes an anchor record
(`ev="logchain"`, event ID 171). The run's public key is the anchor's
`logkey`. A run that has ended can never be signed again.

A flood of identical records costs one signature: a record with the same
kind and fields as the last one written in full, the time aside, is
counted, not written. The summary record `ev="repeated"` (event ID 172)
stands for the repeats: `count`, `first` and `last` (the time of the first
and the last repeat) and `of` (the `seq` of the record repeated). It is
written before the next different record, a minute after the first repeat
or before an anchor (a rotation and the stop included). It is chained and
signed as every other record.

Every anchor also goes to the Windows Event Log (KiTTY++_Agent, source
KiTTY++_agent) where the system MSI registered that log, else to the Windows
Event Log (Application, source KiTTY++_agent) - with every `agentlogsink`
setting, `file` included. Any program can add events under the source, but
none can remove or reorder the ones written: a file cut at its end, or
replaced as a whole, meets the anchors there.

`kageant -verifylog <file>` checks the file, its rotated generations and the
anchors in this computer's logs (`-anchors <file.evtx>`: an exported log
instead). Exit code 0 intact, 1 broken, 2 cannot verify. The **Verify**
button of the agent log window runs the same check. The kageant manual
lists the report lines.

Limits:

- The records after the last anchor are signed, but no anchor pins them: a
  cut after the last anchor cannot be seen. The report lists their `seq`
  range.
- Without the anchors (the Event Log cannot be read, or they rolled out) a
  cut at the end or a replaced file cannot be seen. The check then ends
  with exit code 2, never with "intact".
- An event is checked from the Windows Event Log alone (below). Which run
  signed it shows in the anchors: the `logkey` of the run's `start` or
  `resumed` anchor (171) whose time span holds the event.
- An administrator can clear the log (System log, event 104 of
  Microsoft-Windows-Eventlog) or edit an exported file. Only events already
  forwarded off the computer (section 6) are out of reach.

```powershell
Get-WinEvent -LogName Application -FilterXPath "*[System[Provider[@Name='KiTTY++_agent'] and EventID=171]]"
kageant -verifylog "$env:LOCALAPPDATA\kapper.net\KiTTY\kageant.log"
```

A line's signature covers its `prev`, so one valid signature binds every
line of the unbroken chain before it under the same run key. `kageant
-verifylog` therefore checks the signature of the last line of each such
stretch (the line before a break of the chain or a change of run key, and a
file's last line). An event in the Windows Event Log that is not its line
in the file is checked by its own signature. A stretch whose last signature does
not fit is checked line by line, so the report names each line that does
not fit. For a full log (20 MB, 4 files) the checks take under a second.
Reading its events from the Windows Event Log comes on top.
The Verify button runs on a thread of its own, so kageant goes on
answering meanwhile.

`kageant -verifylog` also reads every other agent event of those logs
(each ID of 100-172 but 171). An event that is its line in the file passes. An event signed by
a run of another log file is that log's. An event signed by a run of this
file whose line no file holds is reported, and so is one that differs from
its line. An event signed by no run known, written while a run of this file
was writing, is reported as forged or edited.

### Checking one event from the Windows Event Log alone

Every agent event (100-172) carries `%1` = the kind, then the fields of its
ID in the order of section 8, ending with `seq`, `sig`, `ts` and `prev`. The
signed bytes are the record's line in the agent log file up to its `sig`
field. They are rebuilt from the event:

1. Take the event's strings in order: `%1` is the kind, `%2` onwards the
   fields with the labels of section 8.
2. Start with `ts=`, the value of `ts`, a space, `ev="`, the kind, `"`.
3. For each field in section 8's order, `seq`, `sig`, `ts` and `prev` left
   out: when its string is not empty, add a space, the label, `="`, the
   string with every `"` doubled, `"`. An empty string adds nothing.
4. Add ` seq="`, the value of `seq`, `" prev="`, the value of `prev`, `"`.
5. The bytes are the result in the computer's ANSI code page (the text
   kageant wrote), with no line end. The strings hold no control
   characters: kageant wrote each as `?`.
6. `sig` is base64 of a 64-byte Ed25519 signature of those bytes. The key
   is the `logkey` (base64, 32 bytes) of the run: the `start` or `resumed`
   anchor (171) of that log file before the event. An anchor checks the
   same way, by its own `logkey`.

Example, event 110 (`add`): `%2 fp` = `ssh-ed25519 255
SHA256:l9+TL7y3IbXVDDFm6grzb/8NwJglZ4ckgviz7wG+3yQ`, `%3 comment` = `my
"work" key`, `%4 result` = `done`, `%5 reason` = `ipc`, `%6 req` =
`ssh-add.exe`, `%7 reqpath` = `C:\Windows\System32\OpenSSH\ssh-add.exe`,
`%8 pid` = `4711`, `%9 path` and `%10 restrict` empty, `%11 seq` = `42`,
`%12 sig` = 88 base64 characters, `%13 ts` = `2026-11-01T10:00:00Z`, `%14
prev` = `Px01AuVcwD/2pxrLL2ldwMPBF8vBpnVq6Uy/r/OuEgM=`. The signed bytes
(one line):

```
ts=2026-11-01T10:00:00Z ev="add" fp="ssh-ed25519 255 SHA256:l9+TL7y3IbXVDDFm6grzb/8NwJglZ4ckgviz7wG+3yQ" comment="my ""work"" key" result="done" reason="ipc" req="ssh-add.exe" reqpath="C:\Windows\System32\OpenSSH\ssh-add.exe" pid="4711" seq="42" prev="Px01AuVcwD/2pxrLL2ldwMPBF8vBpnVq6Uy/r/OuEgM="
```

The line in the file is the same text, then ` sig="` and the value of `%12`.
`%12` decoded must be a valid Ed25519 signature of the line above under the
run's `logkey`. Else the event was edited, or written by a program without
the run's key.

## 8. The fields of each event ID

Generated from the table in `kitty/kitty_eventids.h`. `%1` is the kind of
every record, the fields follow from `%2` on. Positions are never reordered
or reused (section 4).

<!-- BEGIN field tables generated by tools/check-kitty-events.py --write-appendix -->
### 100 sign

Source `KiTTY++_agent`, `%1` = `sign`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; allowed, denied |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `dest` | the server the key is used for, from the client's verified session-bind: the hash of its host key (SHA256:...), then its host names - the one the client sent after the binding first, marked (client), then the ones found in the host-key store and known_hosts, five in all, +N for more; empty without a session-bind |
| `%10` | `fwd` | the agent forwarding hops of the client's verified session-binds, in the order bound, each as in dest, separated by a comma; empty when there is none |
| `%11` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%12` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%13` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%14` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 101 confirm

Source `KiTTY++_agent`, `%1` = `confirm`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; allowed, denied |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `dest` | the server the key is used for, from the client's verified session-bind: the hash of its host key (SHA256:...), then its host names - the one the client sent after the binding first, marked (client), then the ones found in the host-key store and known_hosts, five in all, +N for more; empty without a session-bind |
| `%10` | `fwd` | the agent forwarding hops of the client's verified session-binds, in the order bound, each as in dest, separated by a comma; empty when there is none |
| `%11` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%12` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%13` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%14` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 102 session-bind

Source `KiTTY++_agent`, `%1` = `session-bind`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `result` | the outcome; accepted |
| `%3` | `dest` | the server the client bound to, written the same way without the client's name |
| `%4` | `forwarding` | yes when the client bound the connection for agent forwarding, else no |
| `%5` | `req` | the file name of the program that asked the agent |
| `%6` | `reqpath` | that program's full path |
| `%7` | `pid` | the process that asked the agent, empty when none did |
| `%8` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%9` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%10` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%11` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 103 request

Source `KiTTY++_agent`, `%1` = `request`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `result` | the outcome; refused |
| `%3` | `reason` | the agent's reason as text (key not found, window-messages - a request by window messages refused by allowwmcopydata, unrecognised message, key usage not confirmed by user, ...) |
| `%4` | `req` | the file name of the program that asked the agent |
| `%5` | `reqpath` | that program's full path |
| `%6` | `pid` | the process that asked the agent, empty when none did |
| `%7` | `dest` | the server of the connection's verified session-bind, written the same way, empty without one |
| `%8` | `fwd` | the agent forwarding hops of the client's verified session-binds, in the order bound, each as in dest, separated by a comma; empty when there is none |
| `%9` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%10` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%11` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%12` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 104 restricted

Source `KiTTY++_agent`, `%1` = `restricted`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; sign-refused, remove-refused, store-damaged (the rule file cannot be read: every key is refused over the pipe), keys-added (known_hosts turned on as a host-key source: host keys found there added to the key's 'fixed list' destinations), no-key-offered (the key list a program received was empty: the destination rules left out every key kageant holds, the record without fp, comment and rule) |
| `%5` | `reason` | a token - unbound (no session-bind), bind-failed, not-userauth (the data to sign is no login for the key), destination (no rule permits the server), forwarding-hop, session-id, hostkey, store-damaged, no-port (a CA destination's Valid hosts has a port condition, the client sent no port), known_hosts (result keys-added); for result no-key-offered one of destination, forwarding-hop (the last session-bind is for forwarding), bind-failed, store-damaged; for result store-damaged the file and why it cannot be read, as text |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `dest` | the server of the connection's verified session-bind, written the same way, empty without one |
| `%10` | `fwd` | the agent forwarding hops of the client's verified session-binds, in the order bound, each as in dest, separated by a comma; empty when there is none |
| `%11` | `rule` | the key's destination rules: each as host name, then the hash of its first host key (SHA256:..., CA SHA256:... for a CA key) and +N for more keys, user@ in front for a user, from-host > in front for use through a forwarded agent, separated by a comma; empty when the rule file cannot be read |
| `%12` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%13` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%14` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%15` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 110 add

Source `KiTTY++_agent`, `%1` = `add`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; done, blocked |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `path` | the key file |
| `%10` | `restrict` | the key's destination rules, written as in rule; empty for a key without rules (116: every rule of the key removed) |
| `%11` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%12` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%13` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%14` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 111 loadkey

Source `KiTTY++_agent`, `%1` = `loadkey`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the key file |
| `%3` | `result` | the outcome; loaded, failed, fp-refused, not-verified |
| `%4` | `enc` | whether the key file has a passphrase; values: password, blank |
| `%5` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%6` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%7` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%8` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 112 forget-path

Source `KiTTY++_agent`, `%1` = `forget-path`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the key file |
| `%3` | `result` | the outcome; forgotten |
| `%4` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%5` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%6` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%7` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 113 retry

Source `KiTTY++_agent`, `%1` = `retry`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `trigger` | what started the retry; values: button, device-arrival |
| `%3` | `loaded` | startup keys loaded |
| `%4` | `refused` | startup keys refused |
| `%5` | `absent` | startup keys whose file was not there |
| `%6` | `broken` | startup keys whose file could not be read |
| `%7` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%8` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%9` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%10` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 114 remove

Source `KiTTY++_agent`, `%1` = `remove`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; done, blocked |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%10` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%11` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%12` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 115 remove-all

Source `KiTTY++_agent`, `%1` = `remove-all`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; done, blocked |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%10` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%11` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%12` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 116 rules

Source `KiTTY++_agent`, `%1` = `rules`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; changed (the rule editor's OK changed the key's destination rules), removed, refused (kageant -R: every rule of the key removed after the confirmation, or kept - No, Windows Hello refused, no interactive desktop) |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `restrict` | the key's destination rules, written as in rule; empty for a key without rules (116: every rule of the key removed) |
| `%10` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%11` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%12` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%13` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 120 lock

Source `KiTTY++_agent`, `%1` = `lock`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; done, refused, waiting, unlock-shut, shut, blocked |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%10` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%11` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%12` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 121 unlock

Source `KiTTY++_agent`, `%1` = `unlock`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; done, refused, waiting, unlock-shut, shut, blocked |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%10` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%11` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%12` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 130 hello-enrol

Source `KiTTY++_agent`, `%1` = `hello-enrol`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the key file |
| `%3` | `result` | the outcome; added, denied, failed |
| `%4` | `detail` | the detail Windows Hello reported |
| `%5` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%6` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%7` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%8` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 131 hello-protect

Source `KiTTY++_agent`, `%1` = `hello-protect`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the key file |
| `%3` | `dest` | the protected copy written |
| `%4` | `result` | the outcome; protected, denied, failed, startup-entry-replaced |
| `%5` | `detail` | 130, 132: the detail Windows Hello reported; 230: the refusals the rate limit merged into this record (a number), open (a far2l clipboard open), the transfer id or the file name |
| `%6` | `err` | the error text when the protection failed |
| `%7` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%8` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%9` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%10` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 132 hello-unlock

Source `KiTTY++_agent`, `%1` = `hello-unlock`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the key file |
| `%3` | `result` | the outcome; hello, hello-cached, recovery-passphrase, printed-secret, denied, no-door-here, failed |
| `%4` | `detail` | the detail Windows Hello reported |
| `%5` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%6` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%7` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%8` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 140 keyfile-warning

Source `KiTTY++_agent`, `%1` = `keyfile-warning`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the key file |
| `%3` | `warning` | weak-protection or old-format |
| `%4` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%5` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%6` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%7` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 141 certificate

Source `KiTTY++_agent`, `%1` = `certificate`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; expiring (a certificate of a key expires within 14 days), expired, not-yet-valid |
| `%5` | `valid` | the certificate's validity in UTC, YYYY-MM-DD HH:MM:SS to YYYY-MM-DD HH:MM:SS; from ... or until ... for an open end, always for none |
| `%6` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%7` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%8` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%9` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 150 reencrypt-all

Source `KiTTY++_agent`, `%1` = `reencrypt-all`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; done, failed |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%10` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%11` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%12` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 151 reencrypt

Source `KiTTY++_agent`, `%1` = `reencrypt`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; done, failed |
| `%5` | `reason` | a token - gui, cmdline, ipc, ipc-policy, forwarded (a key change or a lock over a connection bound for agent forwarding, refused by blockforwardedchanges), tray, cli (116: kageant -R), lifetime, device-removal, idle, windows-lock, windows-unlock, click-allow, click-deny, click-cancel-latch, pileup, suppress-latch, hello-verified, hello-denied, hello-unavailable, hello-error, known_hosts (100: a destination rule permitted the signature only through a host name read from OpenSSH's known_hosts) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%10` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%11` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%12` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 152 decrypt

Source `KiTTY++_agent`, `%1` = `decrypt`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `fp` | the key fingerprint; as the agent shows it (ssh-ed25519 255 SHA256:...; an SSH-1 key's bits and MD5 hash, 2048 aa:bb:...) |
| `%3` | `comment` | the comment of the key concerned |
| `%4` | `result` | the outcome; done, failed (a wrong passphrase), cancelled |
| `%5` | `trigger` | what asked for the passphrase; values: first-use (a signature), gui (the key list's Decrypt) |
| `%6` | `req` | the file name of the program that asked the agent |
| `%7` | `reqpath` | that program's full path |
| `%8` | `pid` | the process that asked the agent, empty when none did |
| `%9` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%10` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%11` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%12` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 160 agent

Source `KiTTY++_agent`, `%1` = `agent`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `result` | the outcome; start, stop |
| `%3` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%4` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%5` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%6` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 170 logrotate

Source `KiTTY++_agent`, `%1` = `logrotate`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `result` | the outcome; failed |
| `%3` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%4` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%5` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%6` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 171 logchain

Source `KiTTY++_agent`, `%1` = `logchain`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `result` | the outcome; start, resumed (after a crash fragment), rotate, head (every 15 minutes with new records), stop, config (before a change of agentlog, agentlogpath or agentlogsink) |
| `%3` | `run` | the id of this kageant run, 32 hex digits |
| `%4` | `logkey` | the public Ed25519 key of this kageant run (base64), made at its start and held in its memory only. It signs every record of the run |
| `%5` | `file` | the agent log file the chain is written to, its full path |
| `%6` | `fragment` | resumed: the bytes of the incomplete record a crash left at the end of the file, closed with CRLF before this record |
| `%7` | `value` | config: the setting about to change and its new value: agentlog=no, agentlog=yes, agentlogpath= the new path (empty for the default) or agentlogsink= file, eventlog or both |
| `%8` | `rotation` | rotate: the number of this rotation of the log, counted from its first file |
| `%9` | `keep` | rotate: agentlogkeep at this rotation, the rotated generations kept on disk |
| `%10` | `expired` | rotate: the rotated generations the age rule (agentlogexpiredays) deleted at this rotation, empty for none |
| `%11` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%12` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%13` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%14` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written; the head the anchor pins |

### 172 repeated

Source `KiTTY++_agent`, `%1` = `repeated`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `count` | how often the record of seq of was repeated after it was written in full or after the last 172 for it: every field the same, the time aside |
| `%3` | `first` | the time of the first repeat counted (UTC, as ts in the file) |
| `%4` | `last` | the time of the last repeat counted (UTC, as ts in the file) |
| `%5` | `of` | the seq of the record repeated, empty when the file is not written |
| `%6` | `seq` | the record's line in the chain of the agent log file, counted over every file and every run, empty when the file is not written |
| `%7` | `sig` | the Ed25519 signature (base64) of the record's line up to its sig field, under the run's logkey, empty when the file is not written. The signed line is rebuilt from the event alone: ts=, the kind as ev=, the fields in the order of this table when not empty, then seq and prev |
| `%8` | `ts` | the time of the record as its file line carries it (UTC), empty when the file is not written. With prev it lets the signature be checked from the event alone |
| `%9` | `prev` | the SHA-256 of the line before this record in the file (base64), none for the first line of a file, empty when the file is not written |

### 200 session-open

Source `KiTTY++_kitty`, `%1` = `session-open`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%5` | `proto` | the protocol; values: ssh, telnet, raw, rlogin, supdup, serial |
| `%6` | `addr` | the address connected to (a proxy's when one is used); a serial line's name |
| `%7` | `from` | the local end of the connection, address and port |
| `%8` | `pid` | the process writing the record |

### 201 session-close

Source `KiTTY++_kitty`, `%1` = `session-close`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%5` | `result` | the outcome; closed, remote-closed, fatal, user |
| `%6` | `reason` | the fatal error's text |
| `%7` | `exitcode` | the remote command's exit code |
| `%8` | `pid` | the process writing the record |

### 210 hostkey-known

Source `KiTTY++_kitty`, `%1` = `hostkey-known`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `keytype` | the host key's type as its fingerprint names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key, empty in a 213, 313 refused |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `basis` | how the host key was known; values: cache, manual, ca |
| `%7` | `pid` | the process writing the record |
| `%8` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |

### 211 hostkey-new

Source `KiTTY++_kitty`, `%1` = `hostkey-new`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `keytype` | the host key's type as its fingerprint names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key, empty in a 213, 313 refused |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `result` | the outcome; accept-new, accepted, refused |
| `%7` | `pid` | the process writing the record |
| `%8` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |

### 212 hostkey-mismatch

Source `KiTTY++_kitty`, `%1` = `hostkey-mismatch`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `keytype` | the host key's type as its fingerprint names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key, empty in a 213, 313 refused |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `result` | the outcome; accept-new, accepted, refused |
| `%7` | `pid` | the process writing the record |
| `%8` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |

### 213 hostkey-update

Source `KiTTY++_kitty`, `%1` = `hostkey-update`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `keytype` | the host key's type as its fingerprint names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key, empty in a 213, 313 refused |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `result` | the outcome; stored, added (beside a stored key of its type), replaced, made-primary, removed, proof-failed, refused (the server refused the proof request), over-limit (more keys of one type than hostkeyspertype), declined (Change nothing, or no, with updatehostkeys=ask), unanswered (no answer with updatehostkeys=ask) |
| `%7` | `oldfp` | the hash of the stored key a replaced key took the place of, for added the hash of key 1 of its type (SHA256:...), empty for every other result |
| `%8` | `pid` | the process writing the record |
| `%9` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |

### 220 auth-ok

Source `KiTTY++_kitty`, `%1` = `auth-ok`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `user` | the user name of the login (a proxy's: the name sent to it) |
| `%5` | `method` | the authentication method; for 221 and 321 the one refused, or the last one tried when no method was left; values: none, publickey, password, keyboard-interactive, gssapi-with-mic, over SSH-1 also tis and cryptocard; for a proxy's login: password, chap, none, ident |
| `%6` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%7` | `pid` | the process writing the record |
| `%8` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |
| `%9` | `for` | what the connection served; empty for the connection's own login; session for a jump host's login and for a proxy the session went through; forward for a port forwarding's connection, x11 for an X11 forwarding's |

### 221 auth-failed

Source `KiTTY++_kitty`, `%1` = `auth-failed`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `user` | the user name of the login (a proxy's: the name sent to it) |
| `%5` | `method` | the authentication method; for 221 and 321 the one refused, or the last one tried when no method was left; values: none, publickey, password, keyboard-interactive, gssapi-with-mic, over SSH-1 also tis and cryptocard; for a proxy's login: password, chap, none, ident |
| `%6` | `reason` | access denied, public-key signature refused, no supported authentication methods available, the server's message when it ended the connection instead of answering, or a proxy's refusal as text |
| `%7` | `methods` | the methods the server still offered, comma-separated (over SSH-1 from its authentication mask: publickey, password, tis, cryptocard); empty for a proxy's login |
| `%8` | `pid` | the process writing the record |
| `%9` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |
| `%10` | `for` | what the connection served; empty for the connection's own login; session for a jump host's login and for a proxy the session went through; forward for a port forwarding's connection, x11 for an X11 forwarding's |

### 225 proxy-used

Source `KiTTY++_kitty`, `%1` = `proxy-used`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `proxy` | the proxy's type: http, socks4, socks5, telnet, local-command |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's; local-command: the program's file name |
| `%4` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's; local-command: empty |
| `%5` | `user` | the user name sent to the proxy, empty when none |
| `%6` | `credentials` | yes when a user name or password went to the proxy, else no |
| `%7` | `pid` | the process writing the record |
| `%8` | `for` | what the connection served; empty for the connection's own login; session for a jump host's login and for a proxy the session went through; forward for a port forwarding's connection, x11 for an X11 forwarding's |

### 230 farend-refused

Source `KiTTY++_kitty`, `%1` = `farend-refused`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `request` | what the far end asked for; values: osc52-read, osc5522-read, far2l-clipboard, file-read, file-write |
| `%5` | `reason` | the reason as text |
| `%6` | `detail` | the refusals the rate limit merged into this record (a number), open (a far2l clipboard open), the transfer id or the file name |
| `%7` | `pid` | the process writing the record |

### 240 transfer-done

Source `KiTTY++_kitty`, `%1` = `transfer-done`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%5` | `user` | the user name of the login (a proxy's: the name sent to it) |
| `%6` | `method` | zmodem or kitten-osc5113 |
| `%7` | `direction` | put (upload, to the server) or get (download, from it) |
| `%8` | `local` | the file's full local path; a 331 for a run that failed before any file: the local path as given |
| `%9` | `remote` | the file's remote path - in full where the protocol names it (sftp), else as the server sent it (scp protocol: the name; ZModem download: the name rz announced, or empty; kitten: the far end's name) |
| `%10` | `bytes` | the file's bytes moved; empty when not known |
| `%11` | `pid` | the process writing the record |

### 241 transfer-failed

Source `KiTTY++_kitty`, `%1` = `transfer-failed`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%5` | `user` | the user name of the login (a proxy's: the name sent to it) |
| `%6` | `method` | zmodem or kitten-osc5113 |
| `%7` | `direction` | put (upload, to the server) or get (download, from it) |
| `%8` | `local` | the file's full local path; a 331 for a run that failed before any file: the local path as given |
| `%9` | `remote` | the file's remote path - in full where the protocol names it (sftp), else as the server sent it (scp protocol: the name; ZModem download: the name rz announced, or empty; kitten: the far end's name) |
| `%10` | `bytes` | the file's bytes moved; empty when not known |
| `%11` | `pid` | the process writing the record |
| `%12` | `reason` | the reason as text |

### 300 session-open

Source `KiTTY++_cli`, `%1` = `session-open`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%5` | `proto` | the protocol; values: ssh, telnet, raw, rlogin, supdup, serial |
| `%6` | `addr` | the address connected to (a proxy's when one is used); a serial line's name |
| `%7` | `from` | the local end of the connection, address and port |
| `%8` | `pid` | the process writing the record |

### 301 session-close

Source `KiTTY++_cli`, `%1` = `session-close`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%5` | `result` | the outcome; closed, remote-closed, fatal, user |
| `%6` | `reason` | the fatal error's text |
| `%7` | `exitcode` | the tool's own exit code; empty when there is none |
| `%8` | `pid` | the process writing the record |

### 310 hostkey-known

Source `KiTTY++_cli`, `%1` = `hostkey-known`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `keytype` | the host key's type as its fingerprint names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key, empty in a 213, 313 refused |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `basis` | how the host key was known; values: cache, manual, ca |
| `%7` | `pid` | the process writing the record |
| `%8` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |

### 311 hostkey-new

Source `KiTTY++_cli`, `%1` = `hostkey-new`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `keytype` | the host key's type as its fingerprint names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key, empty in a 213, 313 refused |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `result` | the outcome; accept-new, accepted, refused |
| `%7` | `pid` | the process writing the record |
| `%8` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |

### 312 hostkey-mismatch

Source `KiTTY++_cli`, `%1` = `hostkey-mismatch`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `keytype` | the host key's type as its fingerprint names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key, empty in a 213, 313 refused |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `result` | the outcome; accept-new, accepted, refused |
| `%7` | `pid` | the process writing the record |
| `%8` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |

### 313 hostkey-update

Source `KiTTY++_cli`, `%1` = `hostkey-update`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `keytype` | the host key's type as its fingerprint names it (ssh-ed25519, ssh-rsa, ...), rsa1 for an SSH-1 key, empty in a 213, 313 refused |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `result` | the outcome; stored, added (beside a stored key of its type), replaced, made-primary, removed, proof-failed, refused (the server refused the proof request), over-limit (more keys of one type than hostkeyspertype), declined (Change nothing, or no, with updatehostkeys=ask), unanswered (no answer with updatehostkeys=ask) |
| `%7` | `oldfp` | the hash of the stored key a replaced key took the place of, for added the hash of key 1 of its type (SHA256:...), empty for every other result |
| `%8` | `pid` | the process writing the record |
| `%9` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |

### 320 auth-ok

Source `KiTTY++_cli`, `%1` = `auth-ok`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `user` | the user name of the login (a proxy's: the name sent to it) |
| `%5` | `method` | the authentication method; for 221 and 321 the one refused, or the last one tried when no method was left; values: none, publickey, password, keyboard-interactive, gssapi-with-mic, over SSH-1 also tis and cryptocard; for a proxy's login: password, chap, none, ident |
| `%6` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%7` | `pid` | the process writing the record |
| `%8` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |
| `%9` | `for` | what the connection served; empty for the connection's own login; session for a jump host's login and for a proxy the session went through; forward for a port forwarding's connection, x11 for an X11 forwarding's |

### 321 auth-failed

Source `KiTTY++_cli`, `%1` = `auth-failed`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%3` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's |
| `%4` | `user` | the user name of the login (a proxy's: the name sent to it) |
| `%5` | `method` | the authentication method; for 221 and 321 the one refused, or the last one tried when no method was left; values: none, publickey, password, keyboard-interactive, gssapi-with-mic, over SSH-1 also tis and cryptocard; for a proxy's login: password, chap, none, ident |
| `%6` | `reason` | access denied, public-key signature refused, no supported authentication methods available, the server's message when it ended the connection instead of answering, or a proxy's refusal as text |
| `%7` | `methods` | the methods the server still offered, comma-separated (over SSH-1 from its authentication mask: publickey, password, tis, cryptocard); empty for a proxy's login |
| `%8` | `pid` | the process writing the record |
| `%9` | `proxy` | empty for the connection's own host key and login; for a proxy's its type: ssh (a jump host), http, socks5, socks4 |
| `%10` | `for` | what the connection served; empty for the connection's own login; session for a jump host's login and for a proxy the session went through; forward for a port forwarding's connection, x11 for an X11 forwarding's |

### 325 proxy-used

Source `KiTTY++_cli`, `%1` = `proxy-used`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `proxy` | the proxy's type: http, socks4, socks5, telnet, local-command |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's; local-command: the program's file name |
| `%4` | `port` | the port connected to; a serial line's speed; with proxy set and in 225, 325: the proxy's; local-command: empty |
| `%5` | `user` | the user name sent to the proxy, empty when none |
| `%6` | `credentials` | yes when a user name or password went to the proxy, else no |
| `%7` | `pid` | the process writing the record |
| `%8` | `for` | what the connection served; empty for the connection's own login; session for a jump host's login and for a proxy the session went through; forward for a port forwarding's connection, x11 for an X11 forwarding's |

### 330 transfer-done

Source `KiTTY++_cli`, `%1` = `transfer-done`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `user` | the user name of the login (a proxy's: the name sent to it) |
| `%5` | `direction` | put (upload, to the server) or get (download, from it) |
| `%6` | `local` | the file's full local path; a 331 for a run that failed before any file: the local path as given |
| `%7` | `remote` | the file's remote path - in full where the protocol names it (sftp), else as the server sent it (scp protocol: the name; ZModem download: the name rz announced, or empty; kitten: the far end's name) |
| `%8` | `bytes` | the file's bytes moved; empty when not known |
| `%9` | `pid` | the process writing the record |

### 331 transfer-failed

Source `KiTTY++_cli`, `%1` = `transfer-failed`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `host` | the host name as given for the connection; a serial line's name (COM3); with proxy set and in 225, 325: the proxy's |
| `%4` | `user` | the user name of the login (a proxy's: the name sent to it) |
| `%5` | `direction` | put (upload, to the server) or get (download, from it) |
| `%6` | `local` | the file's full local path; a 331 for a run that failed before any file: the local path as given |
| `%7` | `remote` | the file's remote path - in full where the protocol names it (sftp), else as the server sent it (scp protocol: the name; ZModem download: the name rz announced, or empty; kitten: the far end's name) |
| `%8` | `bytes` | the file's bytes moved; empty when not known |
| `%9` | `pid` | the process writing the record |
| `%10` | `reason` | the reason as text |

### 400 key-generated

Source `KiTTY++_kittygen`, `%1` = `key-generated`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `tool` | kittygen or kittygen-cli |
| `%3` | `keytype` | the key's SSH type |
| `%4` | `bits` | the key size in bits (255 for Ed25519, the curve size for ECDSA) |
| `%5` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%6` | `comment` | of the key generated or saved |
| `%7` | `pid` | the process writing the record |

### 401 private-saved

Source `KiTTY++_kittygen`, `%1` = `private-saved`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `tool` | kittygen or kittygen-cli |
| `%3` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%4` | `comment` | of the key generated or saved |
| `%5` | `path` | the file written |
| `%6` | `format` | ppk3, ppk2, openssh, ssh.com or ssh1 |
| `%7` | `protection` | none, passphrase or hello |
| `%8` | `result` | the outcome; saved, failed |
| `%9` | `reason` | the reason as text |
| `%10` | `pid` | the process writing the record |

### 402 public-saved

Source `KiTTY++_kittygen`, `%1` = `public-saved`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `tool` | kittygen or kittygen-cli |
| `%3` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%4` | `path` | the file written |
| `%5` | `format` | rfc4716, openssh or ssh1 |
| `%6` | `result` | the outcome; saved, failed |
| `%7` | `reason` | the reason as text |
| `%8` | `pid` | the process writing the record |

### 403 passphrase-changed

Source `KiTTY++_kittygen`, `%1` = `passphrase-changed`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `tool` | kittygen or kittygen-cli |
| `%3` | `fp` | the key fingerprint; the hash alone (SHA256:...; an SSH-1 key's MD5 hash, aa:bb:...), empty for a proxy's login |
| `%4` | `path` | the file written |
| `%5` | `protection` | none, passphrase or hello |
| `%6` | `pid` | the process writing the record |

### 900 program-refused

Source `KiTTY++`, `%1` = `program-refused`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `check` | the check that refused the program file; values: rename, signature, integrity |
| `%3` | `found` | what the check found; rename: the program's file name; signature: bad digest, not signed, or signer: followed by the signer name; integrity: no stamp, truncated, bad stamp, modified or cannot check |
| `%4` | `program` | the refused program file's path |

### 901 log-retention

Source `KiTTY++`, `%1` = `log-retention`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `files` | the log files deleted |
| `%3` | `kb` | the kilobytes freed |
| `%4` | `days` | LogKeepDays as set, 0 = off |
| `%5` | `mb` | LogKeepMB as set, 0 = off |
| `%6` | `pattern` | the folder and the log file name pattern searched |

### 902 missing-features

Source `KiTTY++`, `%1` = `missing-features`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `features` | the features this Windows lacks, comma-separated |
| `%3` | `missing` | each missing entry point with its DLL and the feature it powers, separated by semicolons |

### 903 session-save-failed

Source `KiTTY++`, `%1` = `session-save-failed`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `session` | the saved session's name, empty when none was loaded |
| `%3` | `path` | the session file |
| `%4` | `reason` | the reason as text |

### 904 store-rights-not-set

Source `KiTTY++`, `%1` = `store-rights-not-set`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the folder or file created without the access rights |
| `%3` | `reason` | share (a network share without access lists), fat (a volume without access lists) or the reason setting them failed, as text |

### 905 app-file-warning

Source `KiTTY++`, `%1` = `app-file-warning`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the program file the Applications panel marked red |
| `%3` | `reason` | the reason for the red verdict, as the panel shows it |

### 906 helper-program

Source `KiTTY++`, `%1` = `helper-program`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `result` | the outcome; refused (not started), allowed (Yes in the question), declined (No, or the question closed) |
| `%3` | `path` | the helper program's file |
| `%4` | `reason` | why the file was not started (refused), or the signature line the question showed (allowed, declined) |

### 907 session-log-not-written

Source `KiTTY++`, `%1` = `session-log-not-written`.

| Position | Label | Meaning |
|---:|---|---|
| `%2` | `path` | the session log file |

<!-- END generated field tables -->
