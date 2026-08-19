/* The text printed by "kitty -help" (and shown by the /help console command).
 *
 * AUDITED 2026-08-01 against the real command line: this list had been carried
 * forward from KiTTY 0.76 and documented seventeen options that do not exist in
 * this port (-pass, -runagent, -knock, -localproxy, -keygen, -defini,
 * -convert-dir, -auto-store-sshkey, the -noXXX family) plus three that are
 * kitty.ini keys rather than switches (icon, iconfile, initdelay). None of it
 * showed anywhere - GetHelpMessage() had no callers - until -help was added, so
 * the rot was invisible. Anything added here from now on must exist in
 * windows/putty.c, cmdline.c or windows/window.c; the links must point at files
 * that exist in docs/.
 */
static char *default_help_file_content = "\r\n\
KiTTY adds these options to PuTTY's own (see the PuTTY manual for the rest):\r\n\
\r\n\
Starting a session\r\n\
\r\n\
* ssh://[user@]host[:port] - connect to a host; telnet:// works the same way\r\n\
* kitty://<session> - open a saved session by name (putty://<session> too)\r\n\
* -edit <session>: open a saved session's settings instead of connecting\r\n\
* -kload <file> (also -loadfile): load session settings from a .ktx file\r\n\
* -folder <folder>: open a specific folder; must come before -load\r\n\
* -fullscreen: start directly in full screen mode\r\n\
* -send-to-tray: start the session in the system tray, useful for SSH tunnels\r\n\
* -launcher: start the tray session launcher instead of a session\r\n\
* -cmd <command>: run a command in the session once it is up\r\n\
* -loginscript <file>: run a login script\r\n\
* -preconnectcommand <command>: run a local command before connecting\r\n\
* -rcmd <command>: send a command to the running session\r\n\
* -sendcmd <command>: send a command to every window of the same class name\r\n\
* -sendcmdkey <key>: send it only to sessions carrying this broadcast key\r\n\
\r\n\
The window\r\n\
\r\n\
* -title <text>: set the window title\r\n\
* -classname <name>: set the window class name (default KiTTY)\r\n\
* -xpos <x> / -ypos <y>: set the initial window position\r\n\
* -hwndparent <handle>: embed the terminal in another program's window, given\r\n\
    that window's handle as a decimal number (mRemoteNG, Remote4Support)\r\n\
* -noconfirm: close the window without the \"Are you sure?\" prompt. Security\r\n\
    confirmations - host keys, weak crypto - are not affected\r\n\
* -noctrltab: do not switch between KiTTY windows with Ctrl+Tab\r\n\
* -putty: turn off KiTTY's additions and behave like stock PuTTY\r\n\
\r\n\
Logging and passwords\r\n\
\r\n\
* -log <file>: write a session log\r\n\
* -sessionlog <file>: the same, with KiTTY's file-name placeholders expanded\r\n\
* -masterpwfile <file>: read the master password from a file (scripted starts)\r\n\
* -mpwkey <key>: hand an already-unlocked master password to this process\r\n\
* -codepage <name>: select the remote character set\r\n\
\r\n\
Sessions in and out of this machine (see docs/KITTY-PORTABLE.md)\r\n\
\r\n\
* -exportall <dir>: write every saved session to a directory as a bundle\r\n\
* -importdir <dir>: read sessions back in from such a directory\r\n\
* -bundlepwfile <file>: the bundle's password, read from a file\r\n\
* -bundlethispc: bind the exported bundle to this computer\r\n\
\r\n\
Windows integration\r\n\
\r\n\
* -help (also --help, -h, -?): print this list and quit\r\n\
* -sshhandler: make KiTTY the program that opens telnet://, ssh:// and\r\n\
    kitty:// links. Machine-wide when run as administrator, for your account\r\n\
    otherwise; a protocol another program already opens is reported and left\r\n\
    alone. Options:\r\n\
      -force      take those over too - the old setting is exported to a .reg\r\n\
                  file and the report names the command that restores it\r\n\
      -user       register for your account, without administrator rights\r\n\
      -yes        skip the question a portable KiTTY asks before writing to\r\n\
                  the registry\r\n\
      -puttyurl   also register putty:// (KiTTY reads putty:// links whether\r\n\
                  or not it is registered for them)\r\n\
      -uninstall  remove the handlers again - only ones pointing at a KiTTY\r\n\
* -fileassoc: open .ktx session files with KiTTY. Same options as -sshhandler\r\n\
    (-force, -user, -yes, -uninstall)\r\n\
* -cleanup: remove KiTTY's settings from this machine\r\n\
\r\n\
Quick connect (see docs/KITTY-INI.md)\r\n\
\r\n\
The configuration box opens with the session you used last. To type a host\r\n\
instead, load \"Default Settings\" once: KiTTY remembers that, and from then on\r\n\
opens on the defaults with the cursor in Host Name, until you load another\r\n\
session. Put loadlastsession=no in the [ConfigBox] section of kitty.ini to work\r\n\
that way permanently.\r\n\
\r\n\
" ;
