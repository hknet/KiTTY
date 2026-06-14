static char *default_help_file_content = "\r\n\
\r\n\
In KiTTY some new command-line options are available:\r\n\
\r\n\
* -auto-store-sshkey: automatically store server SSH key without prompting\r\n\
* -classname: to define a specific class name for the window\r\n\
* -cmd: to define a startup auto-command\r\n\
* -codepage: to select a new remote character set (use in combination with [-localproxy](cygtermd.md) option)\r\n\
* -convert-dir: convert registry settings to [savemode=dir mode](Portability.md)\r\n\
* -defini: create a default configuration kitty.template.ini\r\n\
* -edit: edit the settings of a session\r\n\
* -fileassoc: associate .ktx files with KiTTY. See [Portability feature](Portability.md) to define file extention\r\n\
* -folder: directly open a specific folder (for [savemode=dir mode](Portability.md) only). It must precede -load option\r\n\
* -fullscreen: start directly in full screen mode\r\n\
* -help: print this help message\r\n\
* -icon: choose a specific [build-in icon](kitty_icon.md)\r\n\
* -iconfile: choose an external icon file\r\n\
* -initdelay: delay (in seconds) before initial configured actions (send to tray, autocommand ...). Default is 2.0\r\n\
* -keygen: start the integrated ssh key generator\r\n\
* -knock: set port knocking sequence\r\n\
* -kload: load a .ktx file (that contains session settings)\r\n\
* -launcher: start [the session launcher](SessionLauncher.md)\r\n\
* -localproxy: define a local proxy for new [Cygterm](cygtermd.md) feature\r\n\
* -log: create a log file\r\n\
* -loginscript: load a login script file\r\n\
* -nobgimage: to disable background image feature\r\n\
* -noctrltab: disable CTRL+TAB feature\r\n\
* -nofiles: disable the creation of default ini file if it does not exist\r\n\
* -noicon: disable [icons](ThatsAllFolks.md) support\r\n\
* -noshortcuts: disable all shortcuts\r\n\
* -notrans: disable [Transparency](Transparency.md) support\r\n\
* -nozmodem: disable [ZModem](ZModem.md) support\r\n\
* -pass: set a password\r\n\
* -putty: disable with one option all KiTTY new features\r\n\
* -runagent: start the integrated SSH agent\r\n\
* -send-to-tray: start a session directly in the [system tray](SendToTray.md) (useful for SSH tunnels)\r\n\
* -sendcmd: to send a command to all windows with the same class name\r\n\
* -sshhandler: create protocols associations (telnet://, ssh://) for internet explorer\r\n\
* -title: set a window title\r\n\
* -version: only open the about box\r\n\
* -xpos: to set the initial X position\r\n\
* -ypos: to set the initial Y position\r\n\
\r\n\
In Klink there is one of the KiTTY option:\r\n\
\r\n\
* -auto-store-sshkey: automatically store server SSH key without prompting\r\n\
\r\n\
In Kageant there is only one:\r\n\
\r\n\
* -pass: to set the passphrase of the ssh key to add\r\n\
\r\n\
\r\n\
" ;
