char default_init_file_content[] = "[ConfigBox]\n\
\n\
; dblclick: what a double-click on a saved session does\n\
;    - open: load it and open it in this window (like the Open button)\n\
;    - start: launch it in a new window and keep the configuration box open (like the Start button)\n\
;dblclick=open\n\
\n\
; defaultsettings: enable/disable the automatic creation of the 'Default Settings' session\n\
;defaultsettings=yes\n\
\n\
; filter: enable/disable filter in saved sessions selection box\n\
filter=yes\n\
\n\
; height: the number of visible lines into the saved sessions selection box (in main window)\n\
height=21\n\
\n\
; noexit: if yes, closing a window that ran a connected session reopens the configuration box\n\
;noexit=no\n\
\n\
; proxyselection: add a proxy selection feature in the main configuration box\n\
;proxyselection=no\n\
\n\
; windowheight: the size of the configuration box\n\
;windowheight=800\n\
\n\
\n\
\n\
[KiTTY]\n\
\n\
; antiidle: character string regularly sent to maintain the connection alive\n\
;antiidle== \\k08\\\n\
\n\
; antiidledelay: time delay between two sending\n\
;antiidledelay=60\n\
\n\
; autoreconnect: enable/disable the automatic reconnection feature\n\
autoreconnect=yes\n\
\n\
; backgroundimage: enable/disable the background image feature\n\
bgimage=no\n\
\n\
; bcdelay: delay in milliseconds between each character in automatic sends (autocommand, login script, user commands, send-text boxes); 0=off\n\
;bcdelay=0\n\
\n\
; commanddelay: delay in seconds between two lines of the automatic start command script (0.05 = 50 ms)\n\
;commanddelay=0.05\n\
\n\
; conf: enable/disable the automatic creation of the default configuration file\n\
conf=yes\n\
\n\
; configdir: set the configuration directory\n\
;    In portable mode it is the place where to find settings files directories: Sessions, SshHostKeys, ...\n\
;    Usually it is the same directory where kitty.exe is\n\
;configdir=\n\
\n\
; cryptsalt: set salt mode for password encryption \n\
;cryptsalt=1\n\
\n\
; CtHelperPath: the full path to the cthelper.exe binary (Wrapper for Cygwin feature)\n\
;CtHelperPath=\n\
\n\
; ctrltab: switch between PuTTY windows with CTRL+Tab\n\
ctrltab=yes\n\
\n\
; cygterm: enable/disable the cygterm feature\n\
cygterm=no\n\
\n\
; debug: for debug purpose only\n\
;debug=no\n\
\n\
; downloaddir: this is the local directory where files are downloaded with the pscp integration\n\
;downloaddir=\n\
\n\
; fileextension: the extension for session settings files in portable mode\n\
;fileextension=.ktx\n\
\n\
; hyperlink: enable/disable the hyperlink feature\n\
hyperlink=yes\n\
\n\
; icon: enable/disable the icon management feature (a particular icon for specific session)\n\
icon=yes\n\
\n\
; iconfile: point to a file where internal icons are located\n\
;iconfile=kitty.exe\n\
\n\
; initdelay: initial delay in seconds before automatic sending of password, or auto command\n\
;initdelay=2.0\n\
\n\
; KiClassName: make KiTTY impersonate PuTTY for external tools that only know\n\
;   PuTTY: the Win32 window class and the window/dialog titles become \"PuTTY\",\n\
;   and sessions/host keys are read from AND written to PuTTY's registry hive\n\
;   (Software\\SimonTatham\\PuTTY) instead of KiTTY's own. Use it so PuTTY-only\n\
;   window managers, automation scripts, or tools that provision sessions into\n\
;   PuTTY's registry work with KiTTY unmodified. Not needed just to SEE existing\n\
;   PuTTY sessions - KiTTY already lists those alongside its own.\n\
;KiClassName=PuTTY\n\
\n\
; mouseshortcuts: enable/disable mouse shortcuts\n\
mouseshortcuts=yes\n\
\n\
; numberoficons: total number of internal icons\n\
;numberoficons=45\n\
\n\
; pastesize: paste size limit (number of characters). Above the limit a confirmation is requested. (0 means unlimited)\n\
;pastesize=0\n\
\n\
; PSCPPath: the full path to the pscp.exe (or kscp.exe) binary\n\
;PSCPPath=\n\
\n\
; ReconnectDelay: the delay before trying to reconnect automatically (in seconds)\n\
ReconnectDelay=5\n\
\n\
; sav: path to the save file of the KiTTY registry. In most cases it is located here %APPDATA%/KiTTY/kitty.sav\n\
;\n\
\n\
; savemode: specify the save mode for sessions settings\n\
;    Allowed values are:\n\
;    - registry : to save sessions into windows registry\n\
;    - file : to save sessions into one unique file (currently not maintained)\n\
;    - dir : to save sessions into files in the directory structure\n\
;savemode=registry\n\
\n\
; scriptfilefilter: specify the local script files extension\n\
;    Default value is (*.ksh,*.sh)|*.ksh;*.sh|SQL files (*.sql)|*.sql|All files (*.*)|*.*|\n\
;scriptfilefilter=All files (*.*)|*.*|\n\
\n\
; scriptmode: master switch for the RuTTY script engine (send a script file line by line, wait-for-prompt mode)\n\
;scriptmode=yes\n\
\n\
; shortcuts: enable/disable keyboard shortcuts\n\
shortcuts=yes\n\
\n\
; size: append the live terminal size [rows x cols] to the window title (not while maximized; needs wintitle=yes)\n\
;size=no\n\
\n\
; slidedelay: delay in seconds before automatic image switching in slideshow mode\n\
;    0 means no automatic slide, waiting for keyboard event\n\
;slidedelay=0\n\
\n\
; sshversion: Force the SSH version title. \n\
;    Default value: PuTTY-KiTTY\n\
;    KiTTY claims it is PuTTY-KiTTY. But sometimes there are servers that do not allow non standard values.\n\
;    So it is possible to redefine it\n\
;sshversion=OpenSSH_5.5\n\
\n\
; transparency: enable/disable the transparency feature\n\
transparency=yes\n\
\n\
; userpasssshnosave: if yes, SSH password will not be saved internally (and can't be reused automatically) \n\
userpasssshnosave=no\n\
\n\
; winrol: enable/disable the rolling feature (rolling into the title bar)\n\
winrol=yes\n\
\n\
; wintitle: enable the KiTTY title decorations: the size suffix (size=yes) and the (PROTECTED)/(ONTOP) markers; no = plain titles\n\
;wintitle=yes\n\
\n\
; WinSCPPath: the full path to the winscp.exe binary: \n\
;WinSCPPath=\n\
\n\
; zmodem: enable/disable zmodem feature (to send/receive files with sz/rz commands)\n\
zmodem=yes\n\
\n\
\n\
\n\
[Shortcuts]\n\
\n\
; Shortcuts: definition for the menu shortcuts keys\n\
\n\
; (re)send automatic command (default is SHIFT+F12)\n\
;autocommand=\n\
\n\
; Change settings ...\n\
;changesettings=\n\
\n\
; Clear scrollback\n\
;clearscrollback=\n\
\n\
; Clear log file\n\
;clearlogfile=\n\
\n\
; Close and restart current session\n\
;closerestart=\n\
\n\
; run a local command (default is CONTROL+F5)\n\
;command=\n\
\n\
; Copy all window buffers to clipboard\n\
;copyall=\n\
\n\
; Open a duplicate window (with same session settings)\n\
;duplicate=\n\
\n\
; open text editor connected to the main window (default is SHIFT+F2)\n\
;editor=\n\
\n\
; open text editor with clipboard content, connected to the main window (default is CONTROL+SHIFT+F2)\n\
;editorclipboard=\n\
\n\
; Show event log\n\
;eventlog=\n\
\n\
; Switch font to black on white colors\n\
;fontblackandwhite=\n\
\n\
; Decrease font size\n\
;fontdown=\n\
\n\
; Switch font to negative colors\n\
;fontnegative=\n\
\n\
; Increase font size\n\
;fontup=\n\
\n\
; Switch to full screen\n\
;fullscreen=\n\
\n\
; receive a remote file with pscp.exe: the full path must be selected in clipboard (default is CONTROL+F4)\n\
;getfile=\n\
\n\
; change the background image (default is CONTROL+F11)\n\
;imagechange=\n\
\n\
; special command box (default is CONTROL+F8)\n\
;input=\n\
\n\
; special command with multi-line editor (default is SHIFT+F8)\n\
;inputm=\n\
\n\
; Repeat key exchange\n\
;keyexchange=\n\
\n\
; New session ...\n\
;opennew=\n\
\n\
; Start a new configuration box with current settings but without hostname\n\
;opennewcurrent=\n\
\n\
; Print current clipboard content (default if SHIFT+F7)\n\
;print=\n\
\n\
; Print all window buffer content (default is F7)\n\
;printall=\n\
\n\
; Protect the window, disable keyboard and mouse input (default is CONTROL+F9)\n\
;protect=\n\
\n\
; Reset terminal\n\
;resetterminal=\n\
\n\
; Roll-up the window into the title bar (default is CONTROL+F12)\n\
;rollup=\n\
\n\
; Load a local script and run it remotely (default is CONTROL+F2)\n\
;script=\n\
\n\
; Send a local file with pscp.exe (default is CONTROL+F3)\n\
;sendfile=\n\
\n\
; Show current port forwarding definition (default is SHIFT+F6)\n\
;showportforward=\n\
\n\
; Enable or disable logging (default is SHIFT+F5)\n\
;switchlogmode=\n\
\n\
; Send the window to the system tray (default is CONTROL+F6)\n\
;tray=\n\
\n\
; Switch to embedded image viewer (default SHIFT+F11)\n\
;viewer=\n\
\n\
; Switch to always visible (default is CONTROL+F7)\n\
;visible=\n\
\n\
; Start WinSCP (default is SHIFT+F3)\n\
;winscp=\n\
\n\
\n\
\n\
[Print]\n\
\n\
; height: characters size (Printing)\n\
height=100\n\
\n\
; maxline: maximum number of lines per page\n\
maxline=60\n\
\n\
; maxchar: maximum number of characters per line\n\
maxchar=85\n\
\n\
\n\
\n\
[Launcher]\n\
\n\
; reload: enable session list reload on each menu respawn\n\
reload=yes\n\
\n\
; exitwithworkplace: close the launcher again when workplace proxy mode is\n\
; switched off (default yes), but only when the mode itself started it. A\n\
; launcher you started yourself is never closed this way. Set no to keep it.\n\
;exitwithworkplace=yes\n\
\n\
; noticeseconds: how long the workplace proxy mode notice stays up (default 15)\n\
;noticeseconds=15\n\
" ;
