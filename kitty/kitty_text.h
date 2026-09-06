/*
 * kitty_text.h: the words KiTTY shows, in one place.
 *
 * Wording is edited here, not hunted for across the source. Two rules go with
 * that:
 *
 *  - keep it SHORT. A control names the thing; it does not explain it.
 *  - the explanation belongs in the manual (FEATURES.md, which generates the
 *    KiTTY chapter of the help), not in a string here and not on a panel.
 *
 * New text is added to the section for the panel or dialog it belongs to.
 */
#ifndef KITTY_TEXT_H
#define KITTY_TEXT_H

#include "kitty_defs.h"  /* limits that appear inside label text (KITTY_STR) */

/* ---- Application > Migration ---- */

#define KT_MIG_STORE_GROUP "Move this KiTTY's sessions between computers"
#define KT_MIG_TITLE       "Sessions from an older KiTTY or from PuTTY"

#define KT_MIG_OLD_GROUP   "Old session stores"
#define KT_MIG_OLD_INTRO   "This machine has sessions in an old 9bis-KiTTY " \
                           "or PuTTY registry hive."
#define KT_MIG_SHOW_BOX    "Show / edit / delete old PuTTY or KiTTY sessions"

#define KT_MIG_IMP_GROUP   "Import into this KiTTY++"
#define KT_MIG_IMP_INTRO   "These Sessions are found in the Registry of this user for old PuTTY/KiTTY:"
#define KT_MIG_IMP_BUTTON  "Import selected Sessions"
#define KT_MIG_IMP_NOSEL   "Nothing was selected."
#define KT_MIG_IMP_DONE    "Imported. The copies are in the session list."
#define KT_MIG_IMP_NONE    "Nothing was imported."

/* Application > Migration > KiTTY storage (kitty_storemove.c) */
#define KT_INIMIG_TITLE      "KiTTY storage"
#define KT_INIMIG_OUT_GROUP  "Make a portable copy"
#define KT_INIMIG_OUT_INTRO  "Copies kitty.exe, its companions, all sessions, named proxies, host keys and settings into a folder of your choice. That copy runs from the folder with its own kitty.ini. The registry stays as it is."
#define KT_INIMIG_OUT_BUTTON "Make a portable copy..."
#define KT_INIMIG_IN_GROUP   "Take a folder store into this registry"
#define KT_INIMIG_IN_INTRO   "Takes the sessions, named proxies, host keys and settings of a folder store into the registry. The folder is left as it is."
#define KT_INIMIG_IN_BUTTON  "Take a folder store..."
#define KT_INIMIG_IN_NOTE    "The folder is merged into the registry: what it holds replaces, what it lacks stays."

/* Application > Migration > old KiTTY Folders (kitty_migrate.c) */
#define KT_MIGF_TITLE        "Import an old KiTTY Folder Store"
#define KT_MIGF_SCAN_GROUP   "Where to look"
#define KT_MIGF_SCAN_INTRO   "The folder and its subfolders are searched for session files."
#define KT_MIGF_FOLDER       "Folder to scan:"
#define KT_MIGF_BROWSE       "Browse..."
#define KT_MIGF_SCAN         "Scan"
#define KT_MIGF_TARGET_GROUP "Where the sessions go"
#define KT_MIGF_TARGET       "Import into folder:"
#define KT_MIGF_ASSIGN       "Assign to selected"
#define KT_MIGF_TARGET_NOTE  "Set the Folder and press Scan to use it as default."
#define KT_MIGF_DEFAULT_FOLDER "KiTTYimport"
#define KT_MIGF_LIST_GROUP   "Session files found"
#define KT_MIGF_COL_HEAD     "Session\tState\tFolder\tSaved as\tPath below the scanned folder"
#define KT_MIGF_IMPORT       "Import selected"
#define KT_MIGF_ST_READY     "ready"
#define KT_MIGF_ST_EXISTS    "already exists"
#define KT_MIGF_ST_PASSWORD  "password cannot be deciphered"
#define KT_MIGF_ST_MPW       "under a master password (asked at import)"
#define KT_MIGF_MPW_PROMPT   "The sessions being imported are protected by the master password of " \
                             "the store they come from. Enter THAT store's master password - not this KiTTY's."
#define KT_MIGF_ST_UNREADABLE "unreadable"
#define KT_MIGF_NO_FOLDER    "Choose a folder to scan first."
#define KT_MIGF_NOT_A_DIR    "That folder does not exist."
#define KT_MIGF_FOUND        "Found %d session file%s in %d folder%s."
#define KT_MIGF_FOUND_NONE   "No session files found."
#define KT_MIGF_LIMIT_DEPTH  " Stopped at %d folder levels deep: deeper folders were not searched."
#define KT_MIGF_LIMIT_COUNT  " Stopped after %d files: the rest was not searched."
#define KT_MIGF_NOSEL        "Nothing was selected."
#define KT_MIGF_ASSIGNED     "Assigned. Press Import selected when the list is right."
#define KT_MIGF_DONE         "Imported. The copies are in the session list."
#define KT_MIGF_NONE         "Nothing was imported."
#define KT_MIGF_BOX_TITLE    "Import from a folder store"
#define KT_MIGF_BOX_OK       "%d session%s imported: %s\n\n"
#define KT_MIGF_BOX_NOPW     "%d of them without a password this KiTTY could not decode: type it once and save.\n\n"
#define KT_MIGF_BOX_FAILED   "%d session%s could not be read or saved.\n\n"

#define KT_MIG_BOX_TITLE   "KiTTY session import"
#define KT_MIG_BOX_OK      "Imported %d session%s: %s."
#define KT_MIG_BOX_FAILED  "\r\n%d session%s could not be read."
#define KT_MIG_BOX_DROPPED "\r\n\r\nNot carried over: %s."
#define KT_MIG_BOX_SEEHELP "\r\nWhy: see Importing old sessions in the help."

/* ---- Application panels, shared ---- */

#define KT_APP_SAVED_LIVE  "Settings here are saved as you change them."


/* ------------------------------------------------------------------ *
 * The configuration box, panel by panel, in the order the panels are
 * built. One macro per distinct wording: editing it here changes every
 * place that wording is shown.
 * ------------------------------------------------------------------ */

/* general */
#define KT_KITTY_START                               "Start"
#define KT_KITTY_CANCEL                              "Cancel"

/* Session */
#define KT_SESSION_SAVE                              "Save"
#define KT_SESSION_NEW_FOLDER                        "New folder"
#define KT_SESSION_PROXY_OVERRIDE_OPTIONS            "Proxy override options:"
#define KT_SESSION_EDIT                              "Edit"
#define KT_SESSION_LOAD                              "Load"
#define KT_SESSION_DELETE                            "Delete"
#define KT_SESSION_DEL_FOLDER                        "Del folder"
#define KT_SESSION_EXPORT_ALL                        "Export all..."
#define KT_SESSION_IMPORT_ALL                        "Import all..."
#define KT_SESSION_THIS_LIST_ALSO_HOLDS_SESSIONS     "This list also holds sessions from an older KiTTY or " \
        "from PuTTY."
#define KT_SESSION_OLD_SESSIONS                      "Old sessions..."
#define KT_SESSION_SPECIFY_THE_DESTINATION           "Specify the destination you want to connect to"
#define KT_SESSION_CLOSE_TERMINAL_WINDOW_ON_EXIT     "Close terminal window on exit:"
#define KT_SESSION_ALWAYS                            "Always"
#define KT_SESSION_NEVER                             "Never"
#define KT_SESSION_ONLY_ON_CLEAN_EXIT                "Only on clean exit"
#define KT_SESSION_SAVE_SETTINGS_ON_EXIT             "Save settings on exit"
#define KT_SESSION_HIDE_THIS_SESSION                 "Hide this session from the launcher"

/* Session/Logging */
#define KT_LOGGING_OPTIONS_CONTROLLING_SESSION_LOGGING "Options controlling session logging"
#define KT_LOGGING_SESSION_LOGGING                   "Session logging:"
#define KT_LOGGING_NONE                              "None"
#define KT_LOGGING_PRINTABLE_OUTPUT                  "Printable output"
#define KT_LOGGING_ALL_SESSION_OUTPUT                "All session output"
#define KT_LOGGING_LOG_FILE_NAME                     "Log file name:"
#define KT_LOGGING_SELECT_SESSION_LOG_FILE_NAME      "Select session log file name"
#define KT_LOGGING_LOG_FILE_NAME_CAN_CONTAIN         "(Log file name can contain &Y, &M, &D for date," \
        " &T for time, &H for host name, and &P for port number)"
#define KT_LOGGING_WHAT_TO_DO                        "What to do if the log file already exists:"
#define KT_LOGGING_ALWAYS_OVERWRITE                  "Always overwrite it"
#define KT_LOGGING_ALWAYS_APPEND_TO_THE_END          "Always append to the end of it"
#define KT_LOGGING_ASK_THE_USER_EVERY_TIME           "Ask the user every time"
#define KT_LOGGING_FLUSH_LOG_FILE_FREQUENTLY         "Flush log file frequently"
#define KT_LOGGING_INCLUDE_HEADER                    "Include header"
#define KT_LOGGING_AUTOMATIC_LOGROTATION_EVERY       "Automatic logrotation every"
#define KT_LOGGING_SEC                               "sec."
#define KT_LOGGING_0_OFF_THE_LOG_FILE                "(0 = off. The file name needs &T in it, or rotation is " \
        "declined.)"
#define KT_LOGGING_TIMESTAMP_STRFTIME_FORMAT         "Timestamp (strftime format)"
#define KT_LOGGING_USE_A_DEFAULT_TIMESTAMP           "Use a default timestamp"
#define KT_LOGGING_WRITTEN_AT_THE_START              "(Starts each logged line, e.g. %Y-%m-%d %H:%M:%S. " \
        "Empty = none.)"
#define KT_LOGGING_OPTIONS_SPECIFIC_TO_SSH_PACKET    "Options specific to SSH packet logging"
#define KT_LOGGING_OMIT_KNOWN_PASSWORD_FIELDS        "Omit known password fields"
#define KT_LOGGING_OMIT_SESSION_DATA                 "Omit session data"
#define KT_LOGGING_BROADCAST_KEY                     "Broadcast key:"
#define KT_LOGGING_COPY                              "Copy"
#define KT_LOGGING_CLEAR                             "Clear"
#define KT_LOGGING_DEFAULT_KEY_GENERATED             "Default key, generated for this KiTTY " \
        "installation here."

/* Session/Scripting */
#define KT_SCRIPTING_OPTIONS_CONTROLLING_AUTOMATED_SCRIPTING "Options controlling automated scripting"
#define KT_SCRIPTING_SEND_A_SCRIPT_FILE              "Send a script file to the host"
#define KT_SCRIPTING_RUN_THE_SCRIPT_ON_CONNECT       "Run the script on connect"
#define KT_SCRIPTING_SCRIPT_FILE                     "Script file:"
#define KT_SCRIPTING_SELECT_SCRIPT_FILE              "Select script file"
#define KT_SCRIPTING_WAIT_FOR_A_PROMPT_BEFORE        "Wait for a prompt before each line"
#define KT_SCRIPTING_WAIT_FOR_TEXT                   "Wait-for text:"
#define KT_SCRIPTING_HALT_ON_TEXT                    "Halt-on text:"
#define KT_SCRIPTING_LINE_DELAY_MS                   "Line delay (ms):"
#define KT_SCRIPTING_TIMEOUT_S                       "Timeout (s):"
#define KT_SCRIPTING_CHARACTER_DELAY_MS              "Character delay (ms):"
#define KT_SCRIPTING_START_OF_CONDITION_COMMENT_LINE "Start of condition/comment line:"
#define KT_SCRIPTING_CR_LF_TRANSLATION               "CR/LF translation:"
#define KT_SCRIPTING_OFF                             "Off"
#define KT_SCRIPTING_EXCEPT_FOR_FIRST_COMMAND        "Except for first command"
#define KT_SCRIPTING_USE_CONDITIONS_FROM_FILE        "Use conditions from file"
#define KT_SCRIPTING_TEXT                            " "

/* Session/Broadcast */
#define KT_BROADCAST_OPTIONS_CONTROLLING_BROADCASTS  "Options controlling broadcasts between KiTTY windows"
#define KT_SCRIPTING_ACCEPT_BROADCASTS_FROM_OTHER_KITTY "Accept broadcasts from other KiTTY windows"
#define KT_SCRIPTING_ACCEPT_BROADCAST_MESSAGES       "Accept broadcast messages for this session"
#define KT_SCRIPTING_ANOTHER_KITTY_CAN_TYPE_INTO     "Another KiTTY can type into this session (/command). " \
        "Needs sendcmdmode=yes in kitty.ini."
#define KT_SCRIPTING_ONLY_MESSAGES_CARRYING_THE_KEY  "Only messages carrying the key below are accepted."

/* Session/Startup */
#define KT_STARTUP_OPTIONS_CONTROLLING_HOW_THIS_SESSION "Options controlling how this session starts"
#define KT_STARTUP_KITTY_LAUNCHER_GLOBAL_HOTKEY      "KiTTY Launcher global hotkey"
#define KT_STARTUP_ENABLE_GLOBAL_HOTKEY              "Enable global hotkey for this session"
#define KT_STARTUP_HOTKEY                            "Hotkey:"
#define KT_STARTUP_CHECK_HOTKEY_AVAILABILITY         "Check hotkey availability"
#define KT_STARTUP_EXAMPLE_CTRL_ALT_K                "Example: Ctrl+Alt+K. Works while KiTTY Launcher runs."
#define KT_STARTUP_LAUNCHER_CONFIGURATION            "Launcher configuration"

/* Terminal */
#define KT_TERMINAL_OPTIONS_CONTROLLING_THE_TERMINAL_EMULATION "Options controlling the terminal emulation"
#define KT_TERMINAL_SET_VARIOUS_TERMINAL_OPTIONS     "Set various terminal options"
#define KT_TERMINAL_AUTO_WRAP_MODE_INITIALLY         "Auto wrap mode initially on"
#define KT_TERMINAL_DEC_ORIGIN_MODE_INITIALLY        "DEC Origin Mode initially on"
#define KT_TERMINAL_IMPLICIT_CR_IN_EVERY_LF          "Implicit CR in every LF"
#define KT_TERMINAL_IMPLICIT_LF_IN_EVERY_CR          "Implicit LF in every CR"
#define KT_TERMINAL_USE_BACKGROUND_COLOUR_TO_ERASE   "Use background colour to erase screen"
#define KT_TERMINAL_ENABLE_BLINKING_TEXT             "Enable blinking text"
#define KT_TERMINAL_ANSWERBACK_TO_E                  "Answerback to ^E:"
#define KT_TERMINAL_LINE_DISCIPLINE_OPTIONS          "Line discipline options"
#define KT_TERMINAL_LOCAL_ECHO                       "Local echo:"
#define KT_TERMINAL_AUTO                             "Auto"
#define KT_TERMINAL_FORCE                            "Force on"
#define KT_TERMINAL_FORCE_OFF                        "Force off"
#define KT_TERMINAL_LOCAL_LINE_EDITING               "Local line editing:"
#define KT_TERMINAL_REMOTE_CONTROLLED_PRINTING       "Remote-controlled printing"
#define KT_TERMINAL_PRINTER_TO_SEND_ANSI_PRINTER     "Printer to send ANSI printer output to:"
#define KT_TERMINAL_PRINT_TO_CLIPBOARD_INSTEAD       "Print to clipboard instead of printer"

/* Terminal/Keyboard */
#define KT_KEYBOARD_OPTIONS_CONTROLLING_THE_EFFECTS  "Options controlling the effects of keys"
#define KT_KEYBOARD_CHANGE_THE_SEQUENCES_SENT_BY     "Change the sequences sent by:"
#define KT_KEYBOARD_THE_BACKSPACE_KEY                "The Backspace key"
#define KT_KEYBOARD_CONTROL_H                        "Control-H"
#define KT_KEYBOARD_CONTROL_127                      "Control-? (127)"
#define KT_KEYBOARD_THE_HOME_AND_END_KEYS            "The Home and End keys"
#define KT_KEYBOARD_STANDARD                         "Standard"
#define KT_KEYBOARD_RXVT                             "rxvt"
#define KT_KEYBOARD_THE_FUNCTION_KEYS_AND_KEYPAD     "The Function keys and keypad"
#define KT_KEYBOARD_ESC_N                            "ESC[n~"
#define KT_KEYBOARD_LINUX                            "Linux"
#define KT_KEYBOARD_XTERM_R6                         "Xterm R6"
#define KT_KEYBOARD_VT400                            "VT400"
#define KT_KEYBOARD_VT100                            "VT100+"
#define KT_KEYBOARD_SCO                              "SCO"
#define KT_KEYBOARD_XTERM_216                        "Xterm 216+"
#define KT_KEYBOARD_SHIFT_CTRL_ALT                   "Shift/Ctrl/Alt with the arrow keys"
#define KT_KEYBOARD_CTRL_TOGGLES_APP_MODE            "Ctrl toggles app mode"
#define KT_KEYBOARD_XTERM_STYLE_BITMAP               "xterm-style bitmap"
#define KT_KEYBOARD_WORD_NAVIGATION_LEFT_RIGHT_ARROWS "Word navigation (Left/Right arrows)"
#define KT_KEYBOARD_ALT                              "Alt"
#define KT_KEYBOARD_CTRL                             "Ctrl"
#define KT_KEYBOARD_BOTH                             "Both"
#define KT_KEYBOARD_ENTER_KEY_SENDS_CR_LF            "Enter key sends CR LF"
#define KT_KEYBOARD_DISABLE_ALTGR_ACTS_AS_PLAIN      "Disable AltGr (acts as plain Alt)"
#define KT_KEYBOARD_OFF_DEFAULT_ALTGR_COMPOSES_CHARACTERS "Off: AltGr types @ and other layout characters. On: it " \
        "sends Alt+key."
#define KT_KEYBOARD_APPLICATION_KEYPAD_SETTINGS      "Application keypad settings:"
#define KT_KEYBOARD_OPTIONS_CONTROLLING_THE_APPLICATION_KEYPAD "Options controlling the application keypad"
#define KT_KEYBOARD_INITIAL_STATE_OF_CURSOR_KEYS     "Initial state of cursor keys:"
#define KT_KEYBOARD_NORMAL                           "Normal"
#define KT_KEYBOARD_APPLICATION                      "Application"
#define KT_KEYBOARD_INITIAL_STATE_OF_NUMERIC_KEYPAD  "Initial state of numeric keypad:"
#define KT_KEYBOARD_NETHACK                          "NetHack"

/* Terminal/Bell */
#define KT_BELL_OPTIONS_CONTROLLING_THE_TERMINAL_BELL "Options controlling the terminal bell"
#define KT_BELL_SET_THE_STYLE_OF_BELL                "Set the style of bell"
#define KT_BELL_ACTION_TO_HAPPEN_WHEN                "Action to happen when a bell occurs:"
#define KT_BELL_NONE_BELL_DISABLED                   "None (bell disabled)"
#define KT_BELL_MAKE_DEFAULT_SYSTEM_ALERT_SOUND      "Make default system alert sound"
#define KT_BELL_VISUAL_BELL_FLASH_WINDOW             "Visual bell (flash window)"
#define KT_BELL_PUT_WINDOW_IN_FOREGROUND             "Put window in foreground on bell"
#define KT_BELL_CONTROL_THE_BELL_OVERLOAD_BEHAVIOUR  "Control the bell overload behaviour"
#define KT_BELL_BELL_IS_TEMPORARILY_DISABLED_WHEN    "Bell is temporarily disabled when over-used"
#define KT_BELL_OVER_USE_MEANS_THIS_MANY             "Over-use means this many bells..."
#define KT_BELL_IN_THIS_MANY_SECONDS                 "... in this many seconds"
#define KT_BELL_THE_BELL_IS_RE_ENABLED               "The bell is re-enabled after a few seconds of silence."
#define KT_BELL_SECONDS_OF_SILENCE_REQUIRED          "Seconds of silence required"

/* Terminal/Features */
#define KT_FEATURES_ENABLING_AND_DISABLING_ADVANCED_TERMINAL "Enabling and disabling advanced terminal features"
#define KT_FEATURES_DISABLE_APPLICATION_CURSOR_KEYS_MODE "Disable application cursor keys mode"
#define KT_FEATURES_DISABLE_APPLICATION_KEYPAD_MODE  "Disable application keypad mode"
#define KT_FEATURES_DISABLE_XTERM_STYLE_MOUSE_REPORTING "Disable xterm-style mouse reporting"
#define KT_FEATURES_DISABLE_REMOTE_CONTROLLED_TERMINAL_RESIZING "Disable remote-controlled terminal resizing"
#define KT_FEATURES_DISABLE_SWITCHING_TO_ALTERNATE_TERMINAL "Disable switching to alternate terminal screen"
#define KT_FEATURES_DISABLE_REMOTE_CONTROLLED_WINDOW_TITLE "Disable remote-controlled window title changing"
#define KT_FEATURES_RESPONSE_TO_REMOTE_TITLE_QUERY   "Response to remote title query (SECURITY):"
#define KT_FEATURES_EMPTY_STRING                     "Empty string"
#define KT_FEATURES_WINDOW_TITLE                     "Window title"
#define KT_FEATURES_DISABLE_REMOTE_CONTROLLED_CLEARING "Disable remote-controlled clearing of scrollback"
#define KT_FEATURES_DISABLE_DESTRUCTIVE_BACKSPACE_ON_SERVER "Disable destructive backspace on server sending ^?"
#define KT_FEATURES_DISABLE_REMOTE_CONTROLLED_CHARACTER_SET "Disable remote-controlled character set configuration"
#define KT_FEATURES_DISABLE_ARABIC_TEXT_SHAPING      "Disable Arabic text shaping"
#define KT_FEATURES_DISABLE_BIDIRECTIONAL_TEXT_DISPLAY "Disable bidirectional text display"
#define KT_FEATURES_DISABLE_BRACKETED_PASTE_MODE     "Disable bracketed paste mode"
#define KT_FEATURES_DISABLE_FOCUS_REPORTING          "Disable focus reporting"

/* Window */
#define KT_WINDOW_SET_THE_SIZE                       "Set the size of the window"
#define KT_WINDOW_COLUMNS                            "Columns"
#define KT_WINDOW_ROWS                               "Rows"
#define KT_WINDOW_CONTROL_THE_SCROLLBACK             "Control the scrollback in the window"
#define KT_WINDOW_LINES_OF_SCROLLBACK                "Lines of scrollback"
#define KT_WINDOW_DISPLAY_SCROLLBAR                  "Display scrollbar"
#define KT_WINDOW_LINES_SCROLLED_PER_WHEEL_TURN      "Lines scrolled per wheel turn"
#define KT_WINDOW_1_HALF_A_SCREEN                    "-1 = half a screen (the default), -2 = a whole screen,"
#define KT_WINDOW_OR_A_POSITIVE_NUMBER               "or a positive number of lines."
#define KT_WINDOW_RESET_SCROLLBACK_ON_KEYPRESS       "Reset scrollback on keypress"
#define KT_WINDOW_RESET_SCROLLBACK_ON_DISPLAY_ACTIVITY "Reset scrollback on display activity"
#define KT_WINDOW_PUSH_ERASED_TEXT_INTO_SCROLLBACK   "Push erased text into scrollback"
#define KT_WINDOW_TURN_OFF_WHERE_A_CLEARED           "Turn off where a cleared screen must not stay readable."

/* Window/Appearance */
#define KT_APPEARANCE_ADJUST_THE_USE                 "Adjust the use of the cursor"
#define KT_APPEARANCE_CURSOR_APPEARANCE              "Cursor appearance:"
#define KT_APPEARANCE_BLOCK                          "Block"
#define KT_APPEARANCE_UNDERLINE                      "Underline"
#define KT_APPEARANCE_VERTICAL_LINE                  "Vertical line"
#define KT_APPEARANCE_CURSOR_BLINKS                  "Cursor blinks"
#define KT_APPEARANCE_FONT_SETTINGS                  "Font settings"
#define KT_APPEARANCE_FONT_USED_IN_THE_TERMINAL      "Font used in the terminal window"
#define KT_APPEARANCE_ADJUST_THE_USE_2               "Adjust the use of the mouse pointer"
#define KT_APPEARANCE_HIDE_MOUSE_POINTER_WHEN_TYPING "Hide mouse pointer when typing in window"
#define KT_APPEARANCE_ADJUST_THE_WINDOW_BORDER       "Adjust the window border"
#define KT_APPEARANCE_GAP_BETWEEN_TEXT_AND_WINDOW    "Gap between text and window edge:"

/* Window/Title */
#define KT_TITLE_WINDOW_TITLE_AND_ICON_OPTIONS       "Window Title and Icon Options"
#define KT_TITLE_ADJUST_THE_BEHAVIOUR                "Adjust the behaviour of the window title"
#define KT_TITLE_WINDOW_TITLE                        "Window title:"
#define KT_TITLE_PLACEHOLDERS_H_S                    "Placeholders (%h, %s, ...)"
#define KT_TITLE_SEPARATE_WINDOW_AND_ICON_TITLES     "Host may set window and taskbar titles separately"

/* Window/Behaviour */
#define KT_BEHAVIOUR_CLOSING_THE_WINDOW              "Closing the window"
#define KT_BEHAVIOUR_REMEMBERING                     "Remembering"
#define KT_BEHAVIOUR_WARN_BEFORE_CLOSING_WINDOW      "Warn before closing window"
#define KT_BEHAVIOUR_SEND_TO_TRAY_ON_STARTUP         "Send to tray on startup"
#define KT_BEHAVIOUR_MAXIMIZE_ON_STARTUP             "Maximize on startup"
#define KT_BEHAVIOUR_FULL_SCREEN_ON_STARTUP          "Full screen on startup"
#define KT_BEHAVIOUR_SWITCH_KITTY_WINDOWS_WITH_CTRL  "Switch KiTTY windows with Ctrl + TAB"
#define KT_BEHAVIOUR_REMEMBER_WINDOW_POSITION_PER_MONITOR "Remember window position (per monitor layout)"
#define KT_BEHAVIOUR_WINDOW_BUTTONS_FOR_KIOSK        "Window buttons (for kiosk or embedded use)"
#define KT_BEHAVIOUR_SYSTEM_MENU_OFF_HIDES_ALL       "System menu (off hides all buttons)"
#define KT_BEHAVIOUR_ALLOW_CLOSING_ALSO_DISABLES     "Allow closing (also disables the X and Alt+F4)"
#define KT_BEHAVIOUR_MINIMIZE_BUTTON                 "Minimize button"
#define KT_BEHAVIOUR_MAXIMIZE_BUTTON                 "Maximize button"

/* Window/Transparency */
#define KT_BACKGROUND_TITLE                          "Background, Pictures and More"
#define KT_TRANSPARENCY_TRANSPARENCY_SETTING         "Transparency setting"
#define KT_TRANSPARENCY_TRANSPARENCY                 "Transparency:"
#define KT_TRANSPARENCY_FROM_0_VISIBLE_TO_255        "from 0 (visible) to 255 (transparent)"
#define KT_TRANSPARENCY_1_TO_DISABLE_COMPLETELY      "-1 to disable completely"

/* Window/Hyperlinks */
#define KT_HYPERLINKS_OPTIONS_CONTROLLING_CLICKABLE_URL_HYPERLINKS "Options controlling clickable URL hyperlinks"
#define KT_HYPERLINKS_HYPERLINK_BEHAVIOUR            "Hyperlink behaviour"
#define KT_HYPERLINKS_REQUIRE_CTRL_KEY_TO_CLICK      "Require Ctrl key to click hyperlinks"
#define KT_HYPERLINKS_UNDERLINE_HYPERLINKS           "Underline hyperlinks"
#define KT_HYPERLINKS_SHOW_HAND_CURSOR_WHEN_HOVERING "Show hand cursor when hovering over hyperlinks"
#define KT_HYPERLINKS_USE_THE_DEFAULT_BROWSER        "Use the default browser"
#define KT_HYPERLINKS_OTHER_BROWSER                  "Other browser:"
#define KT_HYPERLINKS_SELECT_BROWSER_EXECUTABLE      "Select browser executable"
#define KT_HYPERLINKS_USE_THE_DEFAULT_REGULAR_EXPRESSION "Use the default regular expression"
#define KT_HYPERLINKS_CUSTOM_REGEX                   "Custom regex:"
#define KT_HYPERLINKS_RESET_REGEX                    "Reset to KiTTY default"

/* Window/Appearance */
#define KT_APPEARANCE_WHERE_THE_WINDOW_OPENS         "Where the window opens"
#define KT_APPEARANCE_OPTIONS_CONTROLLING_WHERE_THE_WINDOW "Options controlling where the window opens"
#define KT_APPEARANCE_OPEN_THE_WINDOW                "Open the window at a fixed position"
#define KT_APPEARANCE_TOP                            "Top:"
#define KT_APPEARANCE_LEFT                           "Left:"
#define KT_APPEARANCE_A_FIXED_POSITION_WINS_OVER     "Wins over \"Remember window position\". Off-screen is " \
        "moved onto the nearest monitor."

/* Window/Icon */
#define KT_ICON_DEFINE_THE_WINDOW_ICON               "Define the window icon"
#define KT_ICON_ICON_FROM_INTERNAL_RESOURCES         "Icon (from internal resources)"
#define KT_ICON_EXTERNAL_ICON_FILE                   "External icon file:"
#define KT_ICON_SELECT_ICON_FILE                     "Select icon file"

/* Window/Back.&Image */
#define KT_BACK_IMAGE_BACKGROUND_SETTINGS            "Background settings"
#define KT_BACK_IMAGE_BACKGROUND_STYLE               "Background Style:"
#define KT_BACK_IMAGE_SOLID                          "Solid"
#define KT_BACK_IMAGE_DESKTOP                        "Desktop"
#define KT_BACK_IMAGE_IMAGE                          "Image"
#define KT_BACK_IMAGE_DESKTOP_AND_IMAGE_SETTINGS     "Desktop and image settings"
#define KT_BACK_IMAGE_OPACITY_NEGATIVE_WITH_IMAGE    "Opacity: (negative with Image for gradient)"
#define KT_BACK_IMAGE_SLIDESHOW                      "Slideshow:"
#define KT_BACK_IMAGE_IMAGE_SETTINGS                 "Image settings"
#define KT_BACK_IMAGE_IMAGE_FILE_OR_RRGGBB           "Image file: (or #RRGGBB for gradient)"
#define KT_BACK_IMAGE_SELECT_BACKGROUND_IMAGE_FILE   "Select background image file"
#define KT_BACK_IMAGE_IMAGE_PLACEMENT                "Image placement:"
#define KT_BACK_IMAGE_TILE                           "Tile"
#define KT_BACK_IMAGE_CENTER                         "Center"
#define KT_BACK_IMAGE_STRETCH                        "Stretch"
#define KT_BACK_IMAGE_ABSOLUTE_X_Y                   "Absolute (X,Y)"
#define KT_BACK_IMAGE_BLANK_BACK                     "Blank back."
#define KT_BACK_IMAGE_STRETCH_2                      "Stretch+"
#define KT_BACK_IMAGE_ABSOLUTE_LEFT_X                "Absolute Left (X):"
#define KT_BACK_IMAGE_ABSOLUTE_TOP_Y                 "Absolute Top (Y):"
#define KT_BACK_IMAGE_IMAGE_PLACEMENT_IS_RELATIVE    "Image placement is relative to:"
#define KT_BACK_IMAGE_TERMINAL_WINDOW                "Terminal Window"

/* Window/Charset translation */
#define KT_CHARSET_TRANSLATION_OPTIONS_CONTROLLING_CHARACTER_SET_TRANSLATION "Options controlling character set translation"
#define KT_CHARSET_TRANSLATION_CHARACTER_SET_TRANSLATION "Character set translation"
#define KT_CHARSET_TRANSLATION_REMOTE_CHARACTER_SET  "Remote character set:"
#define KT_CHARSET_TRANSLATION_TREAT_CJK_AMBIGUOUS_CHARACTERS "Treat CJK ambiguous characters as wide"
#define KT_CHARSET_TRANSLATION_HANDLING_OF_LINE_DRAWING_CHARACTERS "Handling of line drawing characters:"
#define KT_CHARSET_TRANSLATION_USE_UNICODE_LINE_DRAWING_CODE "Use Unicode line drawing code points"
#define KT_CHARSET_TRANSLATION_POOR_MAN_S_LINE_DRAWING "Poor man's line drawing (+, - and |)"
#define KT_CHARSET_TRANSLATION_COPY_AND_PASTE_LINE_DRAWING "Copy and paste line drawing characters as lqqqk"
#define KT_CHARSET_TRANSLATION_ENABLE_VT100_LINE_DRAWING_EVEN "Enable VT100 line drawing even in UTF-8 mode"

/* Window/Selection */
#define KT_SELECTION_OPTIONS_CONTROLLING_COPY_AND_PASTE "Options controlling copy and paste"
#define KT_SELECTION_CONTROL_USE_OF_MOUSE            "Control use of mouse"
#define KT_SELECTION_SHIFT_OVERRIDES_APPLICATION_S_USE "Shift overrides application's use of mouse"
#define KT_SELECTION_DEFAULT_SELECTION_MODE_ALT_DRAG "Default selection mode (Alt+drag does the other one):"
#define KT_SELECTION_RECTANGULAR_BLOCK               "Rectangular block"
#define KT_SELECTION_ASSIGN_COPY_PASTE_ACTIONS       "Assign copy/paste actions to clipboards"
#define KT_SELECTION_CONTROL_PASTING_OF_TEXT         "Control pasting of text from clipboard to terminal"
#define KT_SELECTION_PERMIT_CONTROL_CHARACTERS_IN_PASTED "Permit control characters in pasted text"
#define KT_SELECTION_RUNNING_THE_CLIPBOARD           "Running the clipboard as a local command (Ctrl+F5)"
#define KT_SELECTION_CONFIRM_BEFORE_RUNNING_THE_CLIPBOARD "Confirm before running the clipboard as a command"
#define KT_SELECTION_SHOW_A_TRAY_NOTIFICATION_AFTER  "Show a tray notification after running a clipboard command"

/* Window/Selection/Remote clipboard */
#define KT_REMOTE_CLIPBOARD_WHAT_A_REMOTE_HOST_MAY   "What a remote host may do with your clipboard"
#define KT_REMOTE_CLIPBOARD_PERMISSIONS              "Permissions"
#define KT_REMOTE_CLIPBOARD_FAR2L_SHARED_CLIPBOARD   "far2l shared clipboard:"
#define KT_REMOTE_CLIPBOARD_DENY                     "Deny"
#define KT_REMOTE_CLIPBOARD_ALLOW                    "Allow"
#define KT_REMOTE_CLIPBOARD_ASK                      "Ask"
#define KT_REMOTE_CLIPBOARD_WRITES_HOST_SETS_YOUR_CLIPBOARD "Writes - host sets your clipboard (OSC 52):"
#define KT_REMOTE_CLIPBOARD_READS_HOST_ASKS_FOR_YOUR "Reads - host asks for your clipboard (OSC 52):"
#define KT_REMOTE_CLIPBOARD_ONLY_WHILE_THIS_WINDOW_HAS "Only while this window has focus"

/* Window/Selection/Remote clipboard/Limits */
#define KT_LIMITS_BOUNDS_ON_WHAT_A_PERMITTED         "Bounds on what a permitted host can do"
#define KT_LIMITS_ANY_PROTOCOL_OSC_52_OSC            "Any protocol (OSC 52, OSC 5522, far2l)"
#define KT_LIMITS_LARGEST_PAYLOAD_IN_MB              "Largest payload, in MB:"
#define KT_LIMITS_MOST_WRITES_PER_SECOND_0           "Most writes per second (0 = no limit):"
#define KT_LIMITS_A_GRANTED_CLIPBOARD_READ           "A granted clipboard read"
#define KT_LIMITS_GRANT_OFFERED_IN_MINUTES           "Grant offered, in minutes:"
#define KT_LIMITS_GRANT_OFFERED_IN_REQUESTS          "Grant offered, in requests:"
#define KT_LIMITS_SHORTEST_GAP_BETWEEN_READS         "Shortest gap between reads, in seconds:"
#define KT_LIMITS_MOST_READS_PER_WINDOW_0            "Most reads per window (0 = no limit):"
#define KT_LIMITS_UNANSWERED_PROMPT_EXPIRES_IN_SECONDS "Unanswered prompt expires, in seconds:"
#define KT_LIMITS_MOST_PROMPTS_PER_TEN_SECONDS       "Most prompts per ten seconds:"

/* Window/Selection/Remote clipboard/Notices */
#define KT_NOTICES_BEING_TOLD_ABOUT_REMOTE_CLIPBOARD "Being told about remote clipboard use"
#define KT_NOTICES_TITLE_BAR                         "Title bar"
#define KT_NOTICES_MARK_WHILE_A_PERMISSION           "Mark while a permission is live  (end, in brackets)"
#define KT_NOTICES_ALSO_MARK_A_STANDING_ALLOW        "Also mark a standing \"Allow\""
#define KT_NOTICES_MARK_WHEN_THE_HOST_ACTUALLY       "Mark when the host actually uses it  (front)"
#define KT_NOTICES_THAT_MARKER_STAYS_UP              "That marker stays up, in seconds:"
#define KT_NOTICES_TINT_THE_TITLE_BAR                "Tint the title bar and border (Windows 11 only)"
#define KT_NOTICES_NOTIFICATION_AREA                 "Notification area"
#define KT_NOTICES_SHOW_TRAY_NOTIFICATIONS_FOR_CLIPBOARD "Show tray notifications for clipboard events"

/* Window/Selection/Copy */
#define KT_COPY_CLASSES_EXPLAIN                      "These Classes decide which characters can be combined to words. " \
        "Selecting words in the Terminal is steered by these Classes."
#define KT_COPY_FORMATTING_OF_COPIED_CHARACTERS      "Formatting of copied characters"
#define KT_COPY_CLASSES_OF_CHARACTER_THAT_GROUP      "Classes of character that group together"
#define KT_COPY_CHARACTER_CLASSES                    "Character classes:"
#define KT_COPY_SET_TO_CLASS                         "Set to class"
#define KT_COPY_SET                                  "Set"

/* Window/Colours */
#define KT_COLOURS_OPTIONS_CONTROLLING_USE_OF_COLOURS "Options controlling use of colours"
#define KT_COLOURS_GENERAL_OPTIONS_FOR_COLOUR_USAGE  "General options for colour usage"
#define KT_COLOURS_ALLOW_TERMINAL_TO_SPECIFY_ANSI    "Allow terminal to specify ANSI colours"
#define KT_COLOURS_ALLOW_TERMINAL_TO_USE_XTERM       "Allow terminal to use xterm 256-colour mode"
#define KT_COLOURS_ALLOW_TERMINAL_TO_USE_24          "Allow terminal to use 24-bit colours"
#define KT_COLOURS_INDICATE_BOLDED_TEXT_BY_CHANGING  "Indicate bolded text by changing:"
#define KT_COLOURS_THE_FONT                          "The font"
#define KT_COLOURS_THE_COLOUR                        "The colour"
#define KT_COLOURS_COLOUR_UNDERLINED_TEXT            "Colour underlined text"
#define KT_COLOURS_COLOUR_SELECTED_TEXT              "Colour selected text"
#define KT_COLOURS_PRECISE_COLOURS                   "Precise colours"
#define KT_COLOURS_SELECT_A_COLOUR                   "Select a colour from the list, and then click the" \
        " Modify button to change its appearance."
#define KT_COLOURS_SELECT_A_COLOUR_TO_ADJUST         "Select a colour to adjust:"
#define KT_COLOURS_RGB_VALUE                         "RGB value:"
#define KT_COLOURS_RED                               "Red"
#define KT_COLOURS_GREEN                             "Green"
#define KT_COLOURS_BLUE                              "Blue"
#define KT_COLOURS_MODIFY                            "Modify"

/* Connection */
#define KT_CONNECTION_OPTIONS_CONTROLLING_THE_CONNECTION "Options controlling the connection"
#define KT_CONNECTION_SENDING_OF_NULL_PACKETS        "Sending of null packets to keep session active"
#define KT_CONNECTION_SECONDS_BETWEEN_KEEPALIVES_0   "Seconds between keepalives (0 to turn off)"
#define KT_CONNECTION_ANTI_IDLE_STRING               "Anti-idle string"
#define KT_CONNECTION_LOW_LEVEL_TCP_CONNECTION_OPTIONS "Low-level TCP connection options"
#define KT_CONNECTION_DISABLE_NAGLE_S_ALGORITHM_TCP  "Disable Nagle's algorithm (TCP_NODELAY option)"
#define KT_CONNECTION_ENABLE_TCP_KEEPALIVES_SO_KEEPALIVE "Enable TCP keepalives (SO_KEEPALIVE option)"
#define KT_CONNECTION_INTERNET_PROTOCOL_VERSION      "Internet protocol version"
#define KT_CONNECTION_IPV4                           "IPv4"
#define KT_CONNECTION_IPV6                           "IPv6"
#define KT_CONNECTION_RECONNECT_OPTIONS              "Reconnect options"
#define KT_CONNECTION_ATTEMPT_TO_RECONNECT_ON_SYSTEM "Attempt to reconnect on system wakeup"
#define KT_CONNECTION_ATTEMPT_TO_RECONNECT_ON_CONNECTION "Attempt to reconnect on connection failure"
#define KT_CONNECTION_CONNECTION_PREPARATION         "Connection preparation"
#define KT_CONNECTION_COMMAND_TO_RUN_BEFORE_CONNECTION "Command to run before connection:"
#define KT_CONNECTION_PORT_KNOCKING_SEQUENCE         "Port knocking sequence:"
#define KT_CONNECTION_A_COMMA_SEPARATED_LIST         "A comma-separated list of port:protocol knocks. " \
        "Protocols are tcp and udp; use s for a pause between " \
        "knocks."
#define KT_CONNECTION_EXAMPLE_2001_TCP_1_S           "Example:  2001:tcp, 1:s, 2002:udp"

/* Connection/Data */
#define KT_LOGIN_OPTIONS_CONTROLLING_THE_LOGIN       "Options controlling the login"
#define KT_DATA_DATA_TO_SEND                         "Data to send to the server"
#define KT_DATA_LOGIN_DETAILS                        "Login details"
#define KT_DATA_AUTO_LOGIN_USERNAME                  "Auto-login username"
#define KT_DATA_WHEN_USERNAME_IS_NOT_SPECIFIED       "When username is not specified:"
#define KT_DATA_PROMPT                               "Prompt"
#define KT_DATA_AUTO_LOGIN_PASSWORD                  "Auto-login password"
#define KT_DATA_SHOW_PASSWORD                        "Show password"
#define KT_DATA_AUTO_COMMAND_AFTER_LOGIN             "Auto-command after login"
#define KT_DATA_LOGIN_SCRIPT_WAIT                    "Login script (wait-for and" \
        " send-text, one per line):"
#define KT_DATA_LOAD_SCRIPT_FROM_FILE                "Load script from file..."
#define KT_DATA_TERMINAL_DETAILS                     "Terminal details"
#define KT_DATA_TERMINAL_DETAILS_SENT                "Terminal details sent to the server"
#define KT_DATA_ENVIRONMENT_VARIABLES_SENT           "Environment variables sent to the server"
#define KT_DATA_HEADER_NAME                          "Name"
#define KT_DATA_HEADER_CONTENT                       "Content"
#define KT_DATA_TERMINAL_TYPE_STRING                 "Terminal-type string"
#define KT_DATA_TERMINAL_SPEEDS                      "Terminal speeds"
#define KT_DATA_ENVIRONMENT_VARIABLES                "Environment variables"
#define KT_DATA_VARIABLE                             "Variable"
#define KT_DATA_VALUE                                "Value"
#define KT_DATA_ADD                                  "Add"
#define KT_DATA_REMOVE                               "Remove"

/* Connection/Proxy */
#define KT_PROXY_OPTIONS_CONTROLLING_PROXY_USAGE     "Options controlling proxy usage"
#define KT_PROXY_NAMED_PROXIES_PROXY_TEMPLATES       "Named Proxies (proxy templates)"
#define KT_PROXY_EDIT_NAMED_PROXIES                  "Edit named proxies..."
#define KT_PROXY_THIS_SESSION_S_OWN_PROXY            "This session's own proxy"
#define KT_PROXY_NAMED_PROXY_PRE_SETS                "Named proxy pre-sets"
#define KT_PROXY_LOAD                                "Load"
#define KT_PROXY_PROXY_TYPE                          "Proxy type:"
#define KT_PROXY_PROXY_HOSTNAME                      "Proxy hostname"
#define KT_PROXY_PORT                                "Port"
#define KT_PROXY_EXCLUDE_HOSTS_IPS                   "Exclude Hosts/IPs"
#define KT_PROXY_CONSIDER_PROXYING_LOCAL_HOST_CONNECTIONS "Consider proxying localhost connections"
#define KT_PROXY_DO_DNS_NAME_LOOKUP                  "Do DNS name lookup at proxy end:"
#define KT_PROXY_NO                                  "No"
#define KT_PROXY_YES                                 "Yes"
#define KT_PROXY_USERNAME                            "Username"
#define KT_PROXY_PASSWORD                            "Password"
#define KT_PROXY_COMMAND_TO_SEND_TO_PROXY            "Command to send to proxy (for some types)"
#define KT_PROXY_PRINT_PROXY_DIAGNOSTICS             "Print proxy diagnostics " \
        "in the terminal window"
#define KT_PROXY_ONLY_UNTIL_SESSION_STARTS           "Only until session starts"

/* Application/Workplace Proxy */
#define KT_WORKPLACE_PROXY_WORKPLACE_PROXY_MODE_APPLICATION_WIDE "Workplace proxy mode (application-wide)"
#define KT_WORKPLACE_PROXY_WHILE_IT_IS_ON_EVERY      "While on, EVERY connection uses the proxy below. No " \
        "session is changed."
#define KT_WORKPLACE_PROXY_PROXY_FOR_EVERYTHING      "Proxy for everything:"
#define KT_WORKPLACE_PROXY_SWITCH_OFF_AFTER          "Switch off after:"
#define KT_WORKPLACE_PROXY_SWITCH                    "Switch on"
#define KT_WORKPLACE_PROXY_NEEDS_NAMED               "Named Proxies need to be configured first."

/* Connection/SSH */
#define KT_SSH_OPTIONS_CONTROLLING_SSH_CONNECTIONS   "Options controlling SSH connections"
#define KT_SSH_NOTHING_ON_THIS_PANEL_MAY             "Nothing on this panel may be reconfigured in mid-" \
        "session; it is only here so that sub-panels of it can " \
        "exist without looking strange."
#define KT_SSH_REMOTE_COMMAND                        "Remote command:"
#define KT_SSH_PROTOCOL_OPTIONS                      "Protocol options"
#define KT_SSH_DON_T_START_A_SHELL                   "Don't start a shell or command at all"
#define KT_SSH_ENABLE_COMPRESSION                    "Enable compression"
#define KT_SSH_SHARING_AN_SSH_CONNECTION_BETWEEN     "Sharing an SSH connection between KiTTY tools"
#define KT_SSH_SHARE_SSH_CONNECTIONS_IF_POSSIBLE     "Share SSH connections if possible"
#define KT_SSH_PERMITTED_ROLES_IN_A_SHARED           "Permitted roles in a shared connection:"
#define KT_SSH_UPSTREAM_CONNECTING_TO_THE_REAL       "Upstream (connecting to the real server)"
#define KT_SSH_DOWNSTREAM_CONNECTING_TO_THE_UPSTREAM "Downstream (connecting to the upstream KiTTY)"
#define KT_SSH_SSH_PROTOCOL_VERSION                  "SSH protocol version:"
#define KT_SSH_2                                     "2"
#define KT_SSH_1_INSECURE                            "1 (INSECURE)"

/* Connection/SSH/Kex */
#define KT_KEX_OPTIONS_CONTROLLING_SSH_KEY_EXCHANGE  "Options controlling SSH key exchange"
#define KT_KEX_KEY_EXCHANGE_ALGORITHM_OPTIONS        "Key exchange algorithm options"
#define KT_KEX_ALGORITHM_SELECTION_POLICY            "Algorithm selection policy:"
#define KT_KEX_WARN_IF_KEY_EXCHANGE                  "Warn if Key Exchange is not post-quantum secure"
#define KT_KEX_ATTEMPT_GSSAPI_KEY_EXCHANGE           "Attempt GSSAPI key exchange"
#define KT_KEX_OPTIONS_CONTROLLING_KEY_RE_EXCHANGE   "Options controlling key re-exchange"
#define KT_KEX_MAX_MINUTES_BEFORE_REKEY_0            "Max minutes before rekey (0 for no limit)"
#define KT_KEX_MINUTES_BETWEEN_GSS_CHECKS_0          "Minutes between GSS checks (0 for never)"
#define KT_KEX_MAX_DATA_BEFORE_REKEY_0               "Max data before rekey (0 for no limit)"
#define KT_KEX_USE_1M_FOR_1_MEGABYTE                 "(Use 1M for 1 megabyte, 1G for 1 gigabyte etc)"

/* Connection/SSH/Host keys */
#define KT_HOST_KEYS_OPTIONS_CONTROLLING_SSH_HOST_KEYS "Options controlling SSH host keys"
#define KT_HOST_KEYS_HOST_KEY_ALGORITHM_PREFERENCE   "Host key algorithm preference"
#define KT_HOST_KEYS_PREFER_ALGORITHMS_FOR_WHICH     "Prefer algorithms for which a host key is known"
#define KT_HOST_KEYS_MANUALLY_CONFIGURE_HOST_KEYS    "Manually configure host keys for this connection"
#define KT_HOST_KEYS_HOST_KEYS_OR_FINGERPRINTS       "Host keys or fingerprints to accept:"
#define KT_HOST_KEYS_KEY                             "Key"
#define KT_HOST_KEYS_ADD_KEY                         "Add key"
#define KT_HOST_KEYS_CONFIGURE_TRUSTED_CERTIFICATION_AUTHORITIES "Configure trusted certification authorities"
#define KT_HOST_KEYS_CONFIGURE_HOST_CAS              "Configure host CAs"

/* Connection/SSH/Cipher */
#define KT_CIPHER_OPTIONS_CONTROLLING_SSH_ENCRYPTION "Options controlling SSH encryption"
#define KT_CIPHER_ENCRYPTION_OPTIONS                 "Encryption options"
#define KT_CIPHER_ENCRYPTION_CIPHER_SELECTION_POLICY "Encryption cipher selection policy:"
#define KT_CIPHER_ENABLE_LEGACY_USE_OF_SINGLE        "Enable legacy use of single-DES in SSH-2"

/* Connection/SSH/Auth */
#define KT_AUTH_OPTIONS_CONTROLLING_SSH_AUTHENTICATION "Options controlling SSH authentication"
#define KT_AUTH_DISPLAY_PRE_AUTHENTICATION_BANNER_SSH "Display pre-authentication banner (SSH-2 only)"
#define KT_AUTH_BYPASS_AUTHENTICATION_ENTIRELY_SSH_2 "Bypass authentication entirely (SSH-2 only)"
#define KT_AUTH_DISCONNECT_IF_AUTHENTICATION_SUCCEEDS_TRIVIALLY "Disconnect if authentication succeeds trivially"
#define KT_AUTH_AUTHENTICATION_METHODS               "Authentication methods"
#define KT_AUTH_ATTEMPT_AUTHENTICATION_USING_KAGEANT_PAGEANT "Attempt authentication using kageant (Pageant)"
#define KT_AUTH_ATTEMPT_TIS_OR_CRYPTOCARD_AUTH       "Attempt TIS or CryptoCard auth (SSH-1)"
#define KT_AUTH_ATTEMPT_KEYBOARD_INTERACTIVE_AUTH_SSH "Attempt \"keyboard-interactive\" auth (SSH-2)"
#define KT_AUTH_OTHER_AUTHENTICATION_RELATED_OPTIONS "Other authentication-related options"
#define KT_AUTH_ALLOW_AGENT_FORWARDING               "Allow agent forwarding"
#define KT_AUTH_ALLOW_ATTEMPTED_CHANGES_OF_USERNAME  "Allow attempted changes of username in SSH-2"

/* Connection/SSH/Auth/Credentials */
#define KT_CREDENTIALS_CREDENTIALS_TO_AUTHENTICATE   "Credentials to authenticate with"
#define KT_CREDENTIALS_PUBLIC_KEY_AUTHENTICATION     "Public-key authentication"
#define KT_CREDENTIALS_PRIVATE_KEY_FILE_FOR_AUTHENTICATION "Private key file for authentication:"
#define KT_CREDENTIALS_SELECT_PRIVATE_KEY_FILE       "Select private key file"
#define KT_CREDENTIALS_CERTIFICATE_TO_USE            "Certificate to use with the private key " \
        "(optional):"
#define KT_CREDENTIALS_SELECT_CERTIFICATE_FILE       "Select certificate file"
#define KT_CREDENTIALS_PINNED_KEY_FINGERPRINT_EMPTY_NO "Pinned key fingerprint (empty = no check):"
#define KT_CREDENTIALS_RECORD_FINGERPRINT_OF_THE_KEY "Record fingerprint of the key file"
#define KT_CREDENTIALS_PLUGIN_TO_PROVIDE_AUTHENTICATION_RESPONSES "Plugin to provide authentication responses"
#define KT_CREDENTIALS_PLUGIN_COMMAND_TO_RUN         "Plugin command to run"

/* Connection/SSH/Auth/GSSAPI */
#define KT_GSSAPI_OPTIONS_CONTROLLING_GSSAPI_AUTHENTICATION "Options controlling GSSAPI authentication"
#define KT_GSSAPI_ATTEMPT_GSSAPI_AUTHENTICATION_SSH_2 "Attempt GSSAPI authentication (SSH-2 only)"
#define KT_GSSAPI_ATTEMPT_GSSAPI_KEY_EXCHANGE_SSH    "Attempt GSSAPI key exchange (SSH-2 only)"
#define KT_GSSAPI_ALLOW_GSSAPI_CREDENTIAL_DELEGATION "Allow GSSAPI credential delegation"
#define KT_GSSAPI_PREFERENCE_ORDER_FOR_GSSAPI_LIBRARIES "Preference order for GSSAPI libraries:"
#define KT_GSSAPI_USER_SUPPLIED_GSSAPI_LIBRARY_PATH  "User-supplied GSSAPI library path:"
#define KT_GSSAPI_SELECT_LIBRARY_FILE                "Select library file"

/* Connection/SSH/TTY */
#define KT_TTY_REMOTE_TERMINAL_SETTINGS              "Remote terminal settings"
#define KT_TTY_DON_T_ALLOCATE_A_PSEUDO               "Don't allocate a pseudo-terminal"
#define KT_TTY_TERMINAL_MODES                        "Terminal modes"
#define KT_TTY_TERMINAL_MODES_TO_SEND                "Terminal modes to send:"
#define KT_TTY_FOR_SELECTED_MODE_SEND                "For selected mode, send:"
#define KT_TTY_NOTHING                               "Nothing"
#define KT_TTY_THIS                                  "This:"

/* Connection/SSH/X11 */
#define KT_X11_OPTIONS_CONTROLLING_SSH_X11_FORWARDING "Options controlling SSH X11 forwarding"
#define KT_X11_X11_FORWARDING                        "X11 forwarding"
#define KT_X11_ENABLE_X11_FORWARDING                 "Enable X11 forwarding"
#define KT_X11_X_DISPLAY_LOCATION                    "X display location"
#define KT_X11_REMOTE_X11_AUTHENTICATION_PROTOCOL    "Remote X11 authentication protocol"
#define KT_X11_MIT_MAGIC_COOKIE_1                    "MIT-Magic-Cookie-1"
#define KT_X11_XDM_AUTHORIZATION_1                   "XDM-Authorization-1"

/* Connection/SSH/Tunnels */
#define KT_TUNNELS_OPTIONS_CONTROLLING_SSH_PORT_FORWARDING "Options controlling SSH port forwarding"
#define KT_TUNNELS_PORT_FORWARDING                   "Port forwarding"
#define KT_TUNNELS_LOCAL_PORTS_ACCEPT_CONNECTIONS    "Local ports accept connections from other hosts"
#define KT_TUNNELS_REMOTE_PORTS_DO_THE_SAME          "Remote ports do the same (SSH-2 only)"
#define KT_TUNNELS_PRINT_DYNAMIC_PORTS_IN_WINDOW     "Print dynamic ports in window title"
#define KT_TUNNELS_FORWARDED_PORTS                   "Forwarded ports:"
#define KT_TUNNELS_ADD_NEW_FORWARDED_PORT            "Add new forwarded port:"
#define KT_TUNNELS_SOURCE_PORT                       "Source port"
#define KT_TUNNELS_DESTINATION                       "Destination"
#define KT_TUNNELS_LOCAL                             "Local"
#define KT_TUNNELS_REMOTE                            "Remote"
#define KT_TUNNELS_DYNAMIC                           "Dynamic"

/* Connection/SSH/Bugs */
#define KT_BUGS_WORKAROUNDS_FOR_SSH_SERVER_BUGS      "Workarounds for SSH server bugs"
#define KT_BUGS_DETECTION_OF_KNOWN_BUGS              "Detection of known bugs in SSH servers"
#define KT_BUGS_CHOKES_ON_SSH_2_IGNORE               "Chokes on SSH-2 ignore messages"
#define KT_BUGS_HANDLES_SSH_2_KEY_RE                 "Handles SSH-2 key re-exchange badly"
#define KT_BUGS_CHOKES_ON_PUTTY_S_SSH                "Chokes on PuTTY's SSH-2 'winadj' requests"
#define KT_BUGS_REPLIES_TO_REQUESTS_ON_CLOSED        "Replies to requests on closed channels"
#define KT_BUGS_IGNORES_SSH_2_MAXIMUM_PACKET         "Ignores SSH-2 maximum packet size"
#define KT_BUGS_FURTHER_DETECTION_OF_KNOWN_BUGS      "Further detection of known bugs in SSH servers"
#define KT_BUGS_OLD_RSA_SHA2_CERT_ALGORITHM          "Old RSA/SHA2 cert algorithm naming"
#define KT_BUGS_REQUIRES_PADDING_ON_SSH_2            "Requires padding on SSH-2 RSA signatures"
#define KT_BUGS_ONLY_SUPPORTS_PRE_RFC4419_SSH        "Only supports pre-RFC4419 SSH-2 DH GEX"
#define KT_BUGS_MISCOMPUTES_SSH_2_HMAC_KEYS          "Miscomputes SSH-2 HMAC keys"
#define KT_BUGS_MISUSES_THE_SESSION_ID               "Misuses the session ID in SSH-2 PK auth"
#define KT_BUGS_MISCOMPUTES_SSH_2_ENCRYPTION_KEYS    "Miscomputes SSH-2 encryption keys"
#define KT_BUGS_CHOKES_ON_SSH_1_IGNORE               "Chokes on SSH-1 ignore messages"
#define KT_BUGS_REFUSES_ALL_SSH_1_PASSWORD           "Refuses all SSH-1 password camouflage"
#define KT_BUGS_CHOKES_ON_SSH_1_RSA                  "Chokes on SSH-1 RSA authentication"
#define KT_BUGS_MANUALLY_ENABLED_WORKAROUNDS         "Manually enabled workarounds"
#define KT_BUGS_DISCARDS_DATA_SENT_BEFORE_ITS        "Discards data sent before its greeting"
#define KT_BUGS_CHOKES_ON_PUTTY_S_FULL               "Chokes on PuTTY's full KEXINIT"

/* Connection/SSH/KSCP */
#define KT_KSCP_KSCP_FILE_TRANSFER_INTEGRATION       "KSCP file-transfer integration"
#define KT_KSCP_KSCP_INTEGRATION                     "KSCP integration"
#define KT_KSCP_TRACK_REMOTE_DIRECTORY_OSC_7         "Track remote directory (OSC 7 shell integration)"
#define KT_KSCP_DRAG_DROP_UPLOADS_AND_WINSCP         "Uploads go to the shell's current directory (OSC 7), " \
        "not your home."
#define KT_KSCP_FIXED_REMOTE_UPLOAD_DIRECTORY        "Fixed remote upload directory"
#define KT_KSCP_ALWAYS_UPLOAD_HERE_INSTEAD_MUTUALLY  "Always upload here instead - mutually exclusive with " \
        "OSC 7 tracking above."
#define KT_KSCP_KSCP_OPTIONS                         "KSCP options"
#define KT_KSCP_FLAGS_PASSED_TO_KSCP_DEFAULT         "Flags passed to kscp; default -r uploads dropped " \
        "folders recursively."
#define KT_KSCP_KEEP_THE_TRANSFER_WINDOW_OPEN        "Keep the transfer window open after success"

/* Connection/SSH/WinSCP */
#define KT_WINSCP_WINSCP_INTEGRATION                 "WinSCP integration"
#define KT_WINSCP_GENERAL_PROTOCOL_SETTING           "General protocol setting"
#define KT_WINSCP_PREFERED_PROTOCOL                  "Prefered protocol:"
#define KT_WINSCP_SCP                                "scp"
#define KT_WINSCP_SFTP                               "sftp"
#define KT_WINSCP_FTP                                "ftp"
#define KT_WINSCP_FTPS                               "ftps"
#define KT_WINSCP_FTPES                              "ftpes"
#define KT_WINSCP_HTTP                               "http"
#define KT_WINSCP_HTTPS                              "https"
#define KT_WINSCP_SFTP_CONNECT_USER_HOSTNAME_PORT    "SFTP connect ([user@]hostname[:port])"
#define KT_WINSCP_WINSCP_ADDITIONAL_OPTIONS          "WinSCP additional options"
#define KT_WINSCP_WINSCP_ADDITIONAL_RAWSETTINGS      "WinSCP additional rawsettings"
#define KT_WINSCP_SHELL_SCP_MODE_ONLY                "Shell (scp mode only)"

/* Connection/Serial */
#define KT_SERIAL_OPTIONS_CONTROLLING_LOCAL_SERIAL_LINES "Options controlling local serial lines"
#define KT_SERIAL_SELECT_A_SERIAL_LINE               "Select a serial line"
#define KT_SERIAL_SERIAL_LINE_TO_CONNECT             "Serial line to connect to"
#define KT_SERIAL_CONFIGURE_THE_SERIAL_LINE          "Configure the serial line"
#define KT_SERIAL_SPEED_BAUD                         "Speed (baud)"
#define KT_SERIAL_DATA_BITS                          "Data bits"
#define KT_SERIAL_STOP_BITS                          "Stop bits"
#define KT_SERIAL_PARITY                             "Parity"
#define KT_SERIAL_FLOW_CONTROL                       "Flow control"

/* Connection/Telnet */
#define KT_TELNET_OPTIONS_CONTROLLING_TELNET_CONNECTIONS "Options controlling Telnet connections"
#define KT_TELNET_TELNET_PROTOCOL_ADJUSTMENTS        "Telnet protocol adjustments"
#define KT_TELNET_HANDLING_OF_OLD_ENVIRON_AMBIGUITY  "Handling of OLD_ENVIRON ambiguity:"
#define KT_TELNET_BSD_COMMONPLACE                    "BSD (commonplace)"
#define KT_TELNET_RFC_1408_UNUSUAL                   "RFC 1408 (unusual)"
#define KT_TELNET_TELNET_NEGOTIATION_MODE            "Telnet negotiation mode:"
#define KT_TELNET_PASSIVE                            "Passive"
#define KT_TELNET_ACTIVE                             "Active"
#define KT_TELNET_KEYBOARD_SENDS_TELNET_SPECIAL_COMMANDS "Keyboard sends Telnet special commands"
#define KT_TELNET_RETURN_KEY_SENDS_TELNET_NEW        "Return key sends Telnet New Line instead of ^M"

/* Connection/Rlogin */
#define KT_RLOGIN_OPTIONS_CONTROLLING_RLOGIN_CONNECTIONS "Options controlling Rlogin connections"
#define KT_RLOGIN_LOCAL_USERNAME                     "Local username:"

/* Connection/SUPDUP */
#define KT_SUPDUP_OPTIONS_CONTROLLING_SUPDUP_CONNECTIONS "Options controlling SUPDUP connections"
#define KT_SUPDUP_WHAT                               "A 1977 protocol for PDP-10 machines, unrelated to SSH. " \
        "Press Help if you are not sure you want it."
#define KT_SUPDUP_LOCATION_STRING                    "Location string"
#define KT_SUPDUP_EXTENDED_ASCII_CHARACTER_SET       "Extended ASCII Character set:"
#define KT_SUPDUP_ITS                                "ITS"
#define KT_SUPDUP_WAITS                              "WAITS"
#define KT_SUPDUP_MORE_PROCESSING                    "**MORE** processing"
#define KT_SUPDUP_TERMINAL_SCROLLING                 "Terminal scrolling"

/* Connection/ZModem */
#define KT_ZMODEM_OPTIONS_CONTROLLING_Z_MODEM_TRANSFERS "Options controlling Z Modem transfers"
#define KT_ZMODEM_DOWNLOAD_FOLDER                    "Download folder"
#define KT_ZMODEM_LOCATION                           "Location:"
#define KT_ZMODEM_RECEIVE_COMMAND_RZ                 "Receive command (rz)"
#define KT_ZMODEM_OPTIONS                            "Options"
#define KT_ZMODEM_CTRL_X_TO_QUIT_RZ                  "Ctrl+X to quit rz before completing"
#define KT_ZMODEM_SEND_COMMAND_SZ                    "Send command (sz)"

/* Application/Config Window */
#define KT_CONFIG_WINDOW_THIS_WINDOW                 "This window"
#define KT_CONFIG_WINDOW_CATEGORY_TREE               "Tree Navigation"
#define KT_APPEARANCE_TITLE                          "Appearance"
#define KT_APPEARANCE_COLOURS                        "Colours"
#define KT_CONFIG_WINDOW_COLOURS                     "Choose the default Appearance:"
#define KT_CONFIG_WINDOW_ONE_SETTING_FOR_THE_WHOLE   "One setting for the whole suite - kitty, kageant and " \
        "kittygen all read it. Dark needs Windows 10 1809 or newer."
#define KT_CONFIG_WINDOW_CHANGES_APPLY_TO_WINDOWS_OPENED "Changes apply to windows opened afterwards - this " \
        "configuration window keeps the colours it opened with."
#define KT_CONFIG_WINDOW_CATEGORY_TREE_OPENS_SHOWING "Levels of the trees to open:"
#define KT_CONFIG_WINDOW_SIZE                        "Size"
#define KT_CONFIG_WINDOW_WINDOW_HEIGHT_IN_PIXELS_BLANK "Window height, in pixels (blank = default):"
#define KT_CONFIG_WINDOW_WINDOW_WIDTH_IN_PIXELS_BLANK "Window width, in pixels (blank = default):"
#define KT_CONFIG_WINDOW_LOCK_WINDOW_SIZE            "Lock window size"
#define KT_CONFIG_WINDOW_CLOSING_A_TERMINAL_WINDOW   "Terminal Window on Exit"
#define KT_CONFIG_WINDOW_COME_BACK_TO_THIS_WINDOW    "always start a new KiTTY after the terminal window closes"

/* Application/Security */
#define KT_SECURITY_SECURITY                         "Security"
#define KT_SECURITY_SSH_AGENT                        "SSH key authentication"
#define KT_SECURITY_WARN_WHEN_AN_UNVERIFIED_AGENT    "Warn if an unknown agent serves our SSH keys"
#define KT_SECURITY_WINDOWS_SUPPORTED_FEATURES       "Windows: supported features"
#define KT_SECURITY_NOTIFY_UNSUPPORTED_LIBS          "Notify the terminal about missing Windows library features"
#define KT_SECURITY_MISSING_CAN_LIMIT                "Missing libraries can limit:"
#define KT_SECURITY_LIMIT_HELLO                      "  -  Windows Hello key protection and confirmations"
#define KT_SECURITY_LIMIT_DARK                       "  -  dark mode and themed dialogs"
#define KT_SECURITY_LIMIT_DPI                        "  -  per-monitor DPI scaling"
#define KT_SECURITY_LIMIT_MEMENC                     "  -  secrets encrypted in memory"
#define KT_SECURITY_LIMIT_SSO                        "  -  GSSAPI/Kerberos single sign-on"
#define KT_SECURITY_LIMIT_IPV6                       "  -  IPv6 name resolution"
#define KT_SECURITY_EVENTLOG_ALWAYS                  "Event Log always reports missing Windows library features."

/* Application/Security/Certificate Authorities */
#define KT_CERTIFICATE_AUTHORITIES_TRUSTED_HOST_CERTIFICATE_AUTHORITIES "Trusted host Certificate Authorities"
#define KT_CERTIFICATE_AUTHORITIES_A_CERTIFICATE_AUTHORITY_SIGNS_HOST "A CA signs host keys, so a new server needs no " \
        "fingerprint check."

/* Application/External tools */
#define KT_EXTERNAL_TOOLS_HELPER_PROGRAMS            "Helper programs"

/* Application/External tools/WinSCP */
#define KT_WINSCP_WINSCP                             "WinSCP"
#define KT_WINSCP_EXECUTABLE                         "Executable"
#define KT_WINSCP_WINSCP_EXECUTABLE                  "WinSCP executable:"
#define KT_WINSCP_SELECT_WINSCP_EXECUTABLE           "Select WinSCP executable"
#define KT_WINSCP_THE_OTHER_WINSCP_SETTINGS_BELONG   "The other WinSCP settings belong to a session and stay on " \
        "Connection > SSH > WinSCP."

/* Application/External tools/ZModem */
#define KT_ZMODEM_ZMODEM                             "ZModem"
#define KT_ZMODEM_RECEIVE_COMMAND_RZ_2               "Receive command (rz):"
#define KT_ZMODEM_SELECT_COMMAND_TO_RECEIVE_ZMODEM   "Select command to receive zmodem data"
#define KT_ZMODEM_SEND_COMMAND_SZ_2                  "Send command (sz):"
#define KT_ZMODEM_SELECT_COMMAND_TO_SEND_ZMODEM      "Select command to send zmodem data"
#define KT_ZMODEM_THEIR_OPTIONS_AND_THE_DOWNLOAD     "Their options, and the download folder, belong to a session " \
        "and stay on Connection > ZModem."

/* Application/KiTTY++ Settings */
#define KT_KSET_TITLE                                "KiTTY++ Settings"
#define KT_KSET_INTRO_WHOLE                          "Settings of this KiTTY as a whole, not of a session."
#define KT_KSET_INTRO_WHERE                          "Each leaf says where its values are kept."

/* Application/KiTTY++ Settings/System */
#define KT_SYSTEM_TITLE                              "Windows integration"
#define KT_SYSTEM_STATE_GROUP                        "Registered with Windows"
#define KT_SYSTEM_REGISTER_GROUP                     "Change the registration"
#define KT_SYSTEM_NOTE                               "Written to the registry: for all users when administrator rights are given, for your account otherwise. Each button asks before it writes."
#define KT_SYSTEM_REGISTER_LINE                      "Point telnet://, ssh://, kitty:// and the session files at this KiTTY++. An entry another program owns stays as it is."
#define KT_SYSTEM_REGISTER                           "Register this KiTTY++..."
#define KT_SYSTEM_PATH_GROUP                         "Command-line tools"
#define KT_SYSTEM_PATH_CHECK                         "Add this KiTTY++ folder to the user PATH (klink, kscp, ksftp in every shell)"
#define KT_SYSTEM_PATH_NOTE                          "Takes effect in shells opened from now on."
#define KT_SYSTEM_PATH_FAIL                          "The PATH could not be changed:\n\n%s"
#define KT_SYSTEM_TAKEOVER_LINE                      "The same, and entries other programs own are replaced too (each is backed up to a .reg file first)."
#define KT_SYSTEM_TAKEOVER                           "Register and take over from other programs..."
#define KT_SYSTEM_UNREGISTER_LINE                    "Remove the entries that point at a KiTTY. Other programs' entries are not touched."
#define KT_SYSTEM_UNREGISTER                         "Unregister..."
#define KT_SYSTEM_REGISTER_Q                         "Point telnet://, ssh://, kitty:// and the session files at this KiTTY++?\n\nEntries other programs own are left alone and reported."
#define KT_SYSTEM_TAKEOVER_Q                         "Point telnet://, ssh://, kitty:// and the session files at this KiTTY++, replacing entries other programs own?\n\nEach replaced setting is exported to a .reg file first, and the report names the command that puts it back."
#define KT_SYSTEM_UNREGISTER_Q                       "Remove the telnet://, ssh://, kitty:// and session-file entries that point at a KiTTY?\n\nEach is exported to a .reg file first. Entries other programs own are not touched."

/* Application/KiTTY++ Settings/Storage & Backup */
#define KT_KSET_STORAGE_TITLE                        "Where settings are kept"
#define KT_KSET_STORAGE_THIS_KITTY                   "This KiTTY"
#define KT_KSET_STORAGE_STORE_REGISTRY               "Settings store: registry (HKCU\\%s)"
#define KT_KSET_STORAGE_STORE_SAV                    "Settings store: registry, loaded from a .sav file at start " \
        "(savemode=file)"
#define KT_KSET_STORAGE_STORE_FOLDER                 "Settings store: folder (%s)"
#define KT_KSET_STORAGE_INI                          "Configuration file: %s"
#define KT_KSET_STORAGE_INI_NONE                     "Configuration file: none (conf=no)"
#define KT_KSET_STORAGE_READONLY                     "KiTTY is running read-only (readonly=yes) - the configuration " \
        "file and the backups are not written."
#define KT_KSET_STORAGE_NOCONF                       "KiTTY is running without a configuration file (conf=no)."

/* Application/KiTTY++ Settings/Storage & Backup/KiTTY.ini (the view) */
#define KT_INIVIEW_TITLE                             "kitty.ini"
#define KT_INIVIEW_SHOW                              "Show:"
#define KT_INIVIEW_SHOW_INI                          "kitty.ini (this installation)"
#define KT_INIVIEW_SHOW_EXAMPLE                      "kitty.ini.example (shipped reference)"
#define KT_INIVIEW_PATH_INI                          "kitty.ini: %s"
#define KT_INIVIEW_NONE                              "(none)"
#define KT_INIVIEW_NO_EXAMPLE                        "kitty.ini.example is not installed beside kitty.exe."
#define KT_INIVIEW_NO_FILE                           "KiTTY is running without a configuration file."
#define KT_INIVIEW_NOT_FOUND                         "(the file does not exist)"
#define KT_INIVIEW_MASK                              "********"
#define KT_INIVIEW_TAKES_EFFECT                      "kitty.ini is read when KiTTY starts. A change takes effect in " \
        "the next KiTTY you start, not in this one."
#define KT_INIVIEW_EDIT                              "Edit kitty.ini..."
#define KT_INIVIEW_EDIT_WARN                         "You are about to edit kitty.ini directly. Changes take effect in " \
        "the next KiTTY you start. A line KiTTY cannot read is ignored without a message. Continue?"
#define KT_KSET_BACKUPS                              "Backups"
#define KT_KSET_BACKUPS_REG                          "Registry backups to keep (0 = none):"
#define KT_KSET_BACKUPS_DIR                          "Folder-store backups to keep (0 = none):"
#define KT_KSET_BACKUPS_REG_SHOWN                    "Registry backups to keep: %d"
#define KT_KSET_BACKUPS_DIR_SHOWN                    "Folder-store backups to keep: %d"
#define KT_KSET_BACKUPS_SAV_PATH                     "Backup file base path: %s"
#define KT_KSET_BACKUPS_DIR_PATH                     "Backup folder: %s\\Backups"
#define KT_KSET_HARDENING                            "Hardening"
#define KT_KSET_RESTRICTACL_ON                       "Restricted process ACL (restrictacl): on"
#define KT_KSET_RESTRICTACL_OFF                      "Restricted process ACL (restrictacl): off"
#define KT_KSET_RESTRICTACL_NOTE                     "Read from kitty.ini only, never the registry. Edit the file " \
        "to change it."
#define KT_KSET_STORAGE_KICLASS                      "Hive selector (KiClassName): %s"
#define KT_KSET_STORAGE_KICLASS_DEFAULT              "(default)"
#define KT_KSET_STORAGE_FILEEXT                      "Session file extension: %s"
#define KT_KSET_STORAGE_CRYPTSALT                    "Legacy password salt variant: %s"
#define KT_KSET_STORAGE_PWGROUP                      "Passwords in a folder store"
#define KT_KSET_STORAGE_PWPROT                       "Passwords protected with:"
#define KT_KSET_STORAGE_PWPROT_MASTER                "a master password"
#define KT_KSET_STORAGE_PWPROT_DPAPI                 "this Windows account (DPAPI)"
#define KT_KSET_STORAGE_PWPROT_LEGACY                "nothing (unprotected)"
#define KT_KSET_STORAGE_PWPROT_NOTE                  "Passwords already stored are not re-encrypted by this change."
#define KT_KSET_STORAGE_WARNLEGACY                   "Warn before re-encrypting an old-format password"

/* Application/KiTTY++ Settings/Terminal windows (+ Shortcuts) */
#define KT_KSET_TW_TITLE                             "Keys and mouse"
#define KT_KSET_TW_BEHAVIOUR                         "Behaviour"
#define KT_KSET_TW_MOUSECHORDS                       "Mouse chords (duplicate session, send to tray)"
#define KT_KSET_TW_MOUSECHORDS_NOTE                  "Ctrl+Shift+click duplicates, Ctrl+middle-click sends to the tray."
#define KT_KSET_TW_HYPERLINK                         "Hyperlinks in the terminal"
#define KT_KSET_TW_FUNKEYS                           "Function keys of a new session:"
#define KT_KSET_TW_FUNKEYS_NOTE                      "What the Keyboard panel of a new session starts with; a " \
        "session's own choice always wins."
#define KT_KSET_TW_PASTESIZE                         "Warn before pasting more than this many characters (0 = never):"
#define KT_CLIPBOARD_TITLE                           "Clipboard"
#define KT_CLIPBOARD_PASTE                           "Large pastes"
#define KT_CLIPBOARD_PASTE_WHAT                      "A paste above the limit is held back and asked about first, so " \
        "a stray Enter cannot flood the shell."
#define KT_CLIPBOARD_PASTE_SCOPE                     "There is no per-session setting for this."

/* Application/Security/Host keys (kitty_hostkeys.c) */
#define KT_HK_TITLE                                  "Host keys"
#define KT_HK_GROUP                                  "Host keys this KiTTY++ has accepted"
#define KT_HK_INTRO                                  "A host key is stored per host and port, for every session that connects there."
#define KT_HK_COL_HEAD                               "Host\tType\tSHA256\tFirst seen\tLast written\tVerified"
#define KT_HK_DETAIL_NONE                            "Select a key to see its fingerprints in full."
#define KT_HK_DETAIL                                 "%s:%d  %s  %d bits\r\n%s\r\nMD5:%s\r\nFirst seen %s\r\nLast written %s"
#define KT_HK_DETAIL_CHECK                           "Check it yourself:  %s"
#define KT_HK_DETAIL_VERIFIED                        "Verified %s: %s"
#define KT_HK_DETAIL_PRESENTED                       "Presented %s  MD5:%s"
#define KT_HK_COPY                                   "Copy"
#define KT_HK_DELETE                                 "Delete"
#define KT_HK_VERIFY                                 "Verify"
#define KT_HK_RUNNING                                "running"
#define KT_HK_VERIFYING                              "Verifying %s:%d ..."
#define KT_HK_VERIFY_STARTING                        "Verifying ..."
#define KT_HK_VERIFY_BUSY                            "A verification is still running."
#define KT_HK_VERIFY_NOTHREAD                        "The verification could not be started."
#define KT_HK_VERIFY_EMPTY                           "No host keys to verify."
#define KT_HK_VERIFIED_SUMMARY                       "Verified %d key%s: %d OK, %d MISMATCH, %d not answered."
#define KT_HK_NOSEL                                  "Nothing was selected."
#define KT_HK_COPIED                                 "Copied %d key%s to the clipboard."
#define KT_HK_DELETED                                "Deleted %d key%s."
#define KT_HK_COUNT                                  "%d host key%s stored."
#define KT_HK_CONFIRM_DELETE                         "Delete the stored host key for %s?\n\nThe next connection to it will ask the first-contact question again."
#define KT_HK_CONFIRM_DELETE_N                       "Delete %d stored host keys?\n\nThe next connection to each of those hosts will ask the first-contact question again."
/* Connection/SSH/Host keys: "Scan this host" and its box */
#define KT_HKSCAN_GROUP                              "This host's keys"
#define KT_HKSCAN_BUTTON                             "Scan/Edit"
#define KT_HKSCAN_STORED                             "Stored keys for %s:%d: %d"
#define KT_HKSCAN_NONE                               "Stored keys for %s:%d: none"
#define KT_HKSCAN_TYPES_WORST                        "ssh-ed25519, ecdsa-sha2-nistp256, ecdsa-sha2-nistp384, ecdsa-sha2-nistp521, ssh-rsa, ssh-ed448, ssh-dss"
#define KT_HKSCAN_NO_HOST                            "No host name in this session yet."
#define KT_HKS_CAPTION                               "Host keys of %s:%d"
#define KT_HKS_INTRO                                 "What %s:%d presents now, against the store. Accept stores a key; Decline leaves the store as it is."
#define KT_HKS_COL_HEAD                              "Type\tBits\tSHA256\tStatus"
#define KT_HKS_STATUS_SCANNING                       "scanning"
#define KT_HKS_SCANNING                              "Scanning: %d of %d key types answered ..."
#define KT_HKS_DONE                                  "%d stored, %d new, %d MISMATCH."
#define KT_HKS_DETAIL_NONE                           "Select a row to see the fingerprints in full."
#define KT_HKS_DETAIL_PRESENTED                      "Presented %s  MD5:%s"
#define KT_HKS_DETAIL_STORED                         "Stored    %s"
#define KT_HKS_ACCEPT                                "Accept"
#define KT_HKS_DECLINE                               "Delete stored"
#define KT_HKS_CLOSE                                 "Close"
#define KT_HKS_SELECT_ROW                            "Select a row first."
#define KT_HKS_NOTHING_TO_ACCEPT                     "The host presented no key of this type."
#define KT_HKS_ALREADY_STORED                        "This key is already stored."
#define KT_HKS_CONFIRM_REPLACE                       "Replace the stored key for %s:%d (%s)?\n\nStored:    %s\nPresented: %s\n\nDo this only if you know the server's key was changed on purpose. If not, somebody may sit between you and the server."
#define KT_HKS_CONFIRM_DELETE                        "Delete the stored key for %s:%d (%s)?\n\nThe next connection to it will ask the first-contact question again."
#define KT_HKS_CONFIRM_DELETE_N                      "Delete the stored key%s (%s) for %s:%d?\n\nThe next connection will ask the first-contact question again."
#define KT_HKS_NONE_STORED                           "None of the selected rows has a stored key."
#define KT_HKS_STORED                                "Stored the %s key."
#define KT_HKS_STORED_N                              "Stored %d key%s."
#define KT_HKS_STORED_LEFT                           "Stored %d key%s; %d left as it is."
#define KT_HKS_DELETED                               "Deleted the stored key."
#define KT_HKS_DELETED_N                             "Deleted %d stored key%s."
#define KT_HKS_PIN                                   "Pin"
#define KT_HKS_PINNED                                "Pinned %d fingerprint%s to this session's manual host keys (%d already listed)."
#define KT_HKS_PIN_NONE                              "Nothing to pin: no key was presented."
#define KT_HKS_LEFT                                  "Left as it is."
#define KT_HOST_KEYS_CAS_JUMP                        "Certificate Authorities are configured for the whole installation: Application > Security."
#define KT_KSET_TW_DEBUG                             "Extra tracing in the Event Log"
#define KT_KSET_SC_TITLE                             "Keyboard shortcuts"
#define KT_KSET_SC_ENABLE                            "Keyboard shortcuts in the terminal window"
#define KT_KSET_SC_FUTURE                            "The Shortcut-Editor will be available in the future."
#define KT_KSET_SC_DEFINED                           "Key combinations that type text"
#define KT_KSET_SC_NONE                              "None defined."
#define KT_KSET_FK_TILDE                             "ESC[n~"
#define KT_KSET_FK_LINUX                             "Linux"
#define KT_KSET_FK_XTERMR6                           "Xterm R6"
#define KT_KSET_FK_VT400                             "VT400"
#define KT_KSET_FK_VT100P                            "VT100+"
#define KT_KSET_FK_SCO                               "SCO"
#define KT_KSET_FK_XTERM216                          "Xterm 216+ (KiTTY's default)"

/* Application/KiTTY++ Settings/Automation */
#define KT_KSET_AU_TITLE                             "Automatic input: pacing, scripts, broadcast"
#define KT_KSET_AU_PACING                            "Pacing"
#define KT_KSET_AU_INITDELAY                         "Wait before auto-typing starts, seconds:"
#define KT_KSET_AU_BCDELAY                           "Pause per typed character, ms (0 = off):"
#define KT_KSET_AU_INTERNALDELAY                     "Pause per typed line and modifier key, ms:"
#define KT_KSET_AU_COMMANDDELAY                      "Pause between auto-command lines, seconds:"
#define KT_KSET_AU_SCRIPTS                           "Scripts typed into the terminal"
#define KT_KSET_AU_SCRIPTMODE                        "Script engine (types a script file into the terminal, line " \
        "by line)"
#define KT_KSET_AU_SCRIPTFILTER                      "File types offered by \"Send a script file\":"
#define KT_KSET_AU_SCRIPTFILTER_NOTE                 "A Windows file-dialog filter: Description|*.ext;*.ext|... " \
        "Blank = scripts (*.ksh, *.sh), SQL files and all files."
#define KT_KSET_AU_BROADCAST                         "Broadcast"
#define KT_KSET_AU_DIAGNOSTICS                       "Diagnostics"
#define KT_KSET_AU_SENDCMD                           "Enable \"Accept broadcast\" on application level"
#define KT_KSET_AU_SENDCMD_NOTE                      "A broadcast (/command, kitty -sendcmd) types its text into " \
        "every window that accepts it. Tools > Accept broadcast switches one window either way."
#define KT_KSET_AU_GROUP                             "Group key of this KiTTY: %s"
#define KT_KSET_AU_GROUP_INI                         "Group key of this KiTTY: %s (set by sendcmdgroup in kitty.ini)"
#define KT_KSET_AU_GROUP_NOTE                        "Only KiTTYs with the same key hear each other. The key is " \
        "derived from where this copy is installed, so a second installation gets a different one; give both " \
        "the same sendcmdgroup in kitty.ini to join them."

/* Application/KiTTY++ Settings/Window & display */
#define KT_KSET_WD_TITLE                             "Terminal Windows and Printing"
#define KT_KSET_WD_TITLEBAR                          "Title bar"
#define KT_KSET_WD_WINTITLE                          "Decorate the window title (size, PROTECTED, ONTOP)"
#define KT_KSET_WD_SIZE                              "Show the terminal size in the title"
#define KT_KSET_WD_WINROLL                           "Double-click the title bar rolls the window up"
#define KT_KSET_WD_FEATURES                          "Terminal Features"
#define KT_KSET_WD_RENDERER                          "Renderer:"
#define KT_KSET_WD_RENDERER_GDI                      "GDI (default)"
#define KT_KSET_WD_RENDERER_D2D                      "Direct2D"
#define KT_KSET_WD_RENDERER_D2D_OLD                  "Direct2D (Win8.1+ only)"
#define KT_D2D_BADGE                                 "D2D"   /* the renderer badge, paint-d2d.c */
#define KT_KSET_WD_FRAMEPACE                         "Frame pacing:"
#define KT_KSET_WD_FP_AUTO                           "Follow the display (auto)"
#define KT_KSET_WD_FP_30                             "30fps"
#define KT_KSET_WD_FP_20                             "20fps"
#define KT_KSET_WD_FP_FIXED                          "PuTTY's fixed (20ms)"
#define KT_KSET_WD_RENDERER_NOTE                     "Renderer and frame pacing apply to new terminal windows only."
#define KT_KSET_WD_CTRLTAB                           "Ctrl+Tab window switching"
#define KT_KSET_WD_TRANSPARENCY                      "Window transparency"
#define KT_KSET_WD_BGIMAGE                           "Background images"
#define KT_KSET_WD_ICONS                             "Icons"
#define KT_KSET_WD_SLIDEDELAY                        "Slideshow interval fallback, seconds (0 = none):"
#define KT_KSET_WD_SHRINK                            "Resample a large image for Stretch+"
#define KT_KSET_WD_ICONFILE                          "Icon library for session icons (.exe, .dll or .icl):"
#define KT_KSET_WD_ICONFILE_NOTE                     "A session's Window > Icon panel picks one of its icons for " \
        "the terminal window. Read at the next start; blank = kitty.dll beside kitty.exe, else kitty.exe."
#define KT_KSET_WD_ICONFILE_SELECT                   "Select the icon file"
#define KT_KSET_WD_PRINTING                          "Printing"
#define KT_KSET_WD_PRINT_PITCH                       "Line pitch, printer units:"
#define KT_KSET_WD_PRINT_LINES                       "Lines per page:"
#define KT_KSET_WD_PRINT_CHARS                       "Characters per line:"
#define KT_KSET_WD_FONTFB                            "Font fallback"
#define KT_KSET_WD_FONTFB_ACTIVE                     "Draw missing characters from fallback fonts"
#define KT_KSET_WD_FONTFB_LIST                       "Fonts to try first (comma-separated):"
#define KT_KSET_WD_FONTFB_LIST_NOTE                  "Font names as Windows shows them, e.g. Symbols Nerd Font Mono, " \
        "JetBrains Mono. A leading ! replaces the built-in list instead of preceding it."
#define KT_KSET_WD_FONTFB_FILEONLY                   "Kept in kitty.ini in every store mode, with its override and " \
        "log keys."
#define KT_KSET_WD_FILEONLY                          "Kept in kitty.ini whatever the store mode."

/* Application/KiTTY++ Settings/Connection & reconnect */
#define KT_KSET_CN_TITLE                             "Auto-Reconnect-Option and In-Line-Confirmations"
#define KT_PXFWD_TITLE                               "Proxy forwards"
#define KT_PXFWD_CHAINS                              "Jump-host chains"
#define KT_PXFWD_NOTE                                "A named proxy may itself go through a named proxy. This caps " \
        "how long such a chain may get before the connection is refused."
#define KT_KSET_CN_RECONNECT                         "Reconnect"
#define KT_KSET_CN_AUTORECONNECT                     "Reconnect automatically when the link drops"
#define KT_KSET_CN_AUTORECONNECT_NOTE                "Master switch for every session's reconnect boxes."
#define KT_KSET_CN_DELAY                             "Wait between reconnect attempts, seconds:"
#define KT_KSET_CN_LIMITS                            "Limits"
#define KT_KSET_CN_CHAINMAX                          "Longest allowed chain of jump hosts:"
#define KT_KSET_CN_NOSAVE                            "Do not keep a typed login name and password in the session"
#define KT_PASSWORDS_TITLE                           "Passwords"
#define KT_PASSWORDS_TYPED                           "A login you typed"
#define KT_PASSWORDS_TYPED_DEFAULT                   "Unticked (the default): after a login, the name and password " \
        "you typed stay in the running session."
#define KT_PASSWORDS_TYPED_CONSEQUENCE               "A duplicate session logs in with them, and Save in the terminal " \
        "window's Change Settings writes them into the saved session."
#define KT_KSET_CN_CONFIRM                           "Confirmations"
#define KT_KSET_CN_MODALERRORS                       "Connection errors as pop-up boxes, not in the terminal"
#define KT_KSET_CN_NEWKEY                            "Unknown host key:"
#define KT_KSET_CN_CHANGEDKEY                        "Changed host key:"
#define KT_KSET_CN_WEAKKEY                           "Weak key or algorithm:"
#define KT_KSET_CH_POPUP                             "Pop-up box"
#define KT_KSET_CH_TERMINAL                          "Prompt in the terminal"
#define KT_CLIENT_IDENTITY_TITLE                     "Client identity"
#define KT_CLIENT_IDENTITY_BANNER                    "SSH banner"
#define KT_KSET_CN_SSHVERSION                        "Client version string (what the server is told):"
#define KT_KSET_CN_SSHVERSION_NOTE                   "Blank = KiTTY's own. Not the protocol selector - that is a " \
        "session's SSH panel."
#define KT_KSET_CN_SSHVERSION_PREVIEW                "The server is told: %s"

/* Application/KiTTY++ Settings/Transfers & Tools */
#define KT_KSET_TT_TITLE                             "Helper programs and transfers"
#define KT_KSET_TT_KSCP                              "File copy (kscp)"
#define KT_KSET_TT_PSCPPATH                          "File-copy helper (kscp.exe or pscp.exe):"
#define KT_KSET_TT_PSCPPATH_SELECT                   "Select the file-copy helper"
#define KT_KSET_TT_PSCPPATH_NOTE                     "Blank = KiTTY finds it at each start (kscp.exe beside it, then " \
        "PuTTY's pscp.exe) and stores nothing. Set it only to force one binary."
#define KT_KSET_TT_PSCPPATH_FOUND                    "Found at this start: %s"
#define KT_KSET_TT_PSCPPATH_NONE                     "nothing - no kscp.exe or pscp.exe in the usual places"
#define KT_KSET_TT_PSCPPORT                          "Port for file transfers (* = the session's port):"
#define KT_KSET_TT_DOWNLOADDIR                       "Download folder:"
#define KT_KSET_TT_UPLOADDIR                         "Remote upload folder:"
#define KT_KSET_TT_CYGWIN                            "Cygwin"
#define KT_KSET_TT_CTHELPER                          "Cygwin helper (cthelper.exe):"
#define KT_KSET_TT_CTHELPER_SELECT                   "Select cthelper.exe"

/* Application/KiTTY++ Settings/Launcher */
#define KT_KSET_LA_TITLE                             "The launcher"
#define KT_KSET_LA_MENU                              "Menu"
#define KT_KSET_LA_RELOAD                            "Rebuild the session list each time the menu opens"
#define KT_KSET_LA_SECOND                            "A second launcher:"
#define KT_KSET_LA_SECOND_EXITS                      "Exits"
#define KT_KSET_LA_SECOND_STARTS                     "Starts anyway"
#define KT_KSET_LA_WORKPLACE                         "Workplace proxy mode"
#define KT_KSET_LA_EXITWITH                          "Workplace mode closes the launcher it started"
#define KT_KSET_LA_NOTICE                            "Workplace notice stays on screen, seconds:"
#define KT_KSET_LA_READ_AT_START                     "The launcher is a separate program: a change here applies " \
        "the next time it starts."

/* The three panel side-jobs of the settings tree */
#define KT_CONNECTION_RECONNECT_GLOBAL_OFF           "Switched off for every session, on Application > KiTTY " \
        "Settings > Reconnect & Prompts."
#define KT_ZMODEM_GLOBAL_ENABLE                      "Enable ZModem transfers in every session"
#define KT_ZMODEM_GLOBAL_OFF_NOTE                    "Switch it on and reopen the configuration window for the " \
        "settings."
#define KT_NAMED_PROXIES_DEFAULTS_TITLE              "Defaults for the proxy Host field"
#define KT_NAMED_PROXIES_HOSTFIELD_GROUP             "What the Host field of a named proxy means"
#define KT_NAMED_PROXIES_HOSTFIELD_INTRO             "It can hold a host name, or the name of a saved session whose " \
        "settings then make the jump connection. Each definition chooses on its \"..this is..\" line; " \
        "\"as globally configured\" uses this default:"
#define KT_NAMED_PROXIES_HOSTFIELD                   "Host field default:"
#define KT_NAMED_PROXIES_HOSTFIELD_SESSION           "session name, then host name"
#define KT_NAMED_PROXIES_HOSTFIELD_HOST              "host name only"
#define KT_NAMED_PROXIES_HOSTFIELD_SESSION_MEANS     "Session name, then host name: if a saved session of that name " \
        "exists, its settings - its own proxy included - make the jump; otherwise the field is a host name. " \
        "PuTTY's classic rule."
#define KT_NAMED_PROXIES_HOSTFIELD_HOST_MEANS        "Host name only: always a host name. Safer - a jump host that " \
        "happens to share a saved session's name cannot pull that session's settings in."
#define KT_NAMED_PROXIES_HOSTFIELD_OVERRIDE          "A definition that chooses for itself ignores this default."

/* Application/Session parameter */
#define KT_SESSION_PARAMETER_THE_SESSION_LIST        "The Session Panel"
#define KT_SESSION_PARAMETER_THE_LIST                "The List-View settings"
#define KT_SESSION_PARAMETER_LENGTH_IN_ROWS_7        "Least length, in rows (" \
        KITTY_STR(KITTY_CFG_SESSION_ROWS_MIN) " or more):"
#define KT_SESSION_PARAMETER_SHOW_DEFAULT_SETTINGS   "Show \"Default Settings\" in the list"
#define KT_SESSION_PARAMETER_QUICK_CONNECT_NEEDS_IT_LOADING "Quick connect needs it: loading Default Settings is how you " \
        "get back to that mode."
#define KT_SESSION_PARAMETER_SHOW_FOLDERS_AS_ROWS_NOT "Foldernavigation in the List"
#define KT_SESSION_PARAMETER_SEARCH_THE_LIST_AS_YOU  "Search the list as you type"
#define KT_SESSION_PARAMETER_OPENING                 "Load Last Session / Quick-Connect-Mode"
#define KT_SESSION_PARAMETER_OPEN_ON_THE_LAST_USED   "Open on the last used session (off = quick connect)"
#define KT_SESSION_PARAMETER_QUICK_CONNECT_STARTS_EVERY_KITTY "Quick connect starts every KiTTY on Default Settings with the " \
        "cursor already in Host Name: type an address and press Enter."
#define KT_SESSION_PARAMETER_DOUBLE_CLICK_A_SESSION  "Double-click a session to:"
#define KT_SESSION_PARAMETER_PROXY                   "Proxy"
#define KT_SESSION_PARAMETER_SHOW_THE_PROXY_CHOOSER  "Show the proxy chooser:"
#define KT_SESSION_PARAMETER_NEVER_ALSO_HIDES_THE_EDIT "Never also hides the Edit button. Definitions stay on " \
        "Application > Named Proxies."

/* Application/Updates */
#define KT_UPDATES_KEEPING_KITTY_UP_TO_DATE          "Keeping KiTTY up to date"
#define KT_UPDATES_UPDATE_CHECK                      "Update check"
#define KT_UPDATES_CHECK_FOR_UPDATES_WHEN_KITTY      "Check for updates when KiTTY starts"
#define KT_UPDATES_CHECK_FOR_UPDATES_NOW             "Check for updates now"

/* Comment */
#define KT_COMMENT_COMMENT_FOR_THIS_SESSION          "Comment for this session"
#define KT_COMMENT_SESSION_COMMENT                   "Session comment"
#define KT_COMMENT_NOTIFY                            "Notify the user at login"

/* Application/Named Proxies */
#define KT_NAMED_PROXIES_PROXY_DEFINITIONS_SHARED_BY_EVERY "Proxy Definitions"
#define KT_NAMED_PROXIES_DEFINITION                  "Definition"
#define KT_NAMED_PROXIES_SHOW                        "show"
#define KT_NAMED_PROXIES_NAME_PICK_ONE_TO_EDIT       "Name (pick one to edit, or type a new one):"
#define KT_NAMED_PROXIES_TYPE                        "Type:"
#define KT_NAMED_PROXIES_NAME_IP                     "Name/IP:"
#define KT_NAMED_PROXIES_PORT                        "Port:"
#define KT_NAMED_PROXIES_THIS                        ".. this is .."
#define KT_NAMED_PROXIES_USERNAME                    "Username:"
#define KT_NAMED_PROXIES_PASSWORD                    "Password:"
#define KT_NAMED_PROXIES_COMMAND_TO_SEND_TELNET_LOCAL "Command to send (Telnet / Local / SSH execute or " \
        "subsystem types):"
#define KT_NAMED_PROXIES_EXCLUDE_HOSTS_IPS_SEPARATE  "Exclude Hosts/IPs (separate with commas or spaces):"
#define KT_NAMED_PROXIES_DNS_LOOKUP_AT_PROXY         "DNS lookup"
#define KT_NAMED_PROXIES_PRINT_DIAGNOSTICS           "Diagnostics"
#define KT_NAMED_PROXIES_NOTHING_IS_STORED_UNTIL_SAVE "Nothing is stored until Save. Leaving this " \
        "panel discards an unsaved edit."

/* ---- Shared captions and wordings (every binary) ---- */

/* Message-box, notice and window captions */
#define KT_CAP_KITTY                                 "KiTTY"
#define KT_CAP_ERROR                                 "Error"
#define KT_CAP_INFO                                  "Info"
#define KT_CAP_KAGEANT                               "kageant"
#define KT_CAP_KITTYGEN                              "KiTTYgen"
#define KT_CAP_KITTYGEN_ERROR                        "KiTTYgen Error"
#define KT_CAP_KITTYGEN_WARNING                      "KiTTYgen Warning"
#define KT_CAP_KITTYGEN_FATAL                        "KiTTYgen Fatal Error"
#define KT_CAP_HELLO                                 "Windows Hello"
#define KT_CAP_HELLO_KEYS                            "Windows Hello protected keys"
#define KT_CAP_LAUNCHER                              "KiTTY Launcher"
#define KT_CAP_LAUNCHER_HOTKEY                       "KiTTY Launcher hotkey"
#define KT_CAP_SESSION_IMPORT                        KT_MIG_BOX_TITLE  /* the Migration panel's box carries the same title */
#define KT_CAP_SESSION_EXPORT                        "KiTTY session export"
#define KT_CAP_UPDATE                                "KiTTY Update"
#define KT_CAP_CLIPBOARD                             "KiTTY clipboard"
#define KT_CAP_FILE_ASSOC                            "KiTTY file association"
#define KT_CAP_SAVE_SESSION                          "Save session"
#define KT_CAP_SELECT_FOLDER                         "Select a folder..."

/* Message texts shown at more than one place */
#define KT_MSG_UNKNOWN_ERROR                         "unknown error"
#define KT_MSG_INIT_REGISTRY                         "Initializing registry."
#define KT_MSG_LAUNCHER_DIR_FAILED                   "Unable to create the menu launcher directory"
#define KT_MSG_SSH_ONLY                              "This function is only available with SSH connections."
#define KT_MSG_EXEC_KITTY_FAILED                     "Unable to execute KiTTY!"

/* System-menu and tray-menu items */
#define KT_MENU_EXIT                                 "E&xit"
#define KT_MENU_ABOUT                                "&About"
#define KT_MENU_HELP                                 "&Help"

/* Command-line diagnostics */
#define KT_CLI_OPTION_NEEDS_ARG                      "option \"%s\" requires an argument"

/* @@BATCH_SECTIONS@@ - later sections are added below this line */

/* ---- Batch 3: kitty core modules (kitty.c, launcher, commands, transfers, update) ---- */

/* Captions of this batch's boxes, notices and dialogs */
#define KT_CAP_PORT_FORWARDING                       KT_TUNNELS_PORT_FORWARDING  /* "Port forwarding" - the Tunnels panel's group title */
#define KT_CAP_NOTES                                 "Notes"
#define KT_CAP_CFGDIR_NOT_FOUND                      "KiTTY: configdir not found"
#define KT_CAP_RESTORE_REG_SESSIONS                  "KiTTY - restore your registry sessions?"
#define KT_CAP_REG_SESSIONS_SET_ASIDE                "KiTTY - your registry sessions were set aside"
#define KT_CAP_OPEN_FILE                             "Open file..."
#define KT_CAP_SAVE_FILE                             "Save file..."
#define KT_CAP_SEND_FILE                             "Send file..."
#define KT_CAP_CONFIG_INFO                           "Configuration infomations"   /* the historical spelling, kept */
#define KT_CAP_SESSION_NAME                          "Session name"
#define KT_CAP_URL_REGEX                             "URL regex"
#define KT_CAP_DELETE_HIVE                           "KiTTY - delete the registry hive"
#define KT_CAP_COPY_TO_PUTTY                         "KiTTY - copy to PuTTY"
#define KT_CAP_COPY_FROM_PUTTY                       "KiTTY - copy from PuTTY"
#define KT_CAP_SETTING_REMOVED                       "KiTTY - this setting has been removed"
#define KT_CAP_RUTTY_ALSO                            "KiTTY - a rutty script is also configured"
#define KT_CAP_PASSWORD                              KT_PROXY_PASSWORD  /* "Password" - the /passwd box carries the panel's word */
#define KT_CAP_PRINT_REPORT                          "Print report"
#define KT_CAP_UPDATE_SIG_REJECTED                   "KiTTY Update - signature rejected"
#define KT_CAP_UPDATE_AVAILABLE                      "KiTTY update available"
#define KT_CAP_AUTOPW                                "KiTTY auto-login password"
#define KT_CAP_AGENT_UNVERIFIED                      "KiTTY: SSH agent not verified"
#define KT_CAP_HOTKEY_CONFLICT                       "KiTTY session hotkey conflict"
#define KT_CAP_WORKPLACE_ON                          "Workplace proxy mode is ON"
#define KT_CAP_WORKPLACE_TIMEOUT                     "Workplace proxy mode has timed out"
#define KT_CAP_WORKPLACE_OFF                         "Workplace proxy mode is OFF"
#define KT_CAP_WORKPLACE_INACTIVE                    "Workplace proxy mode is not active"
#define KT_CAP_URL_HANDLERS                          "KiTTY URL handlers"
#define KT_CAP_ZMODEM                                "KiTTY ZModem"
#define KT_CAP_XFER                                  "KiTTY transfer"
#define KT_CAP_XFER_CANCELLED                        "KiTTY transfer - cancelled"
#define KT_CAP_TRANSFER_PROBLEM                      "Transfer problem"
#define KT_CAP_RUN_CLIP_CMD                          "KiTTY - run clipboard command"
#define KT_CAP_KEY_NOT_IN_AGENT                      "KiTTY - the key is not in the agent"
#define KT_CAP_OSC52_SESSION                         "KiTTY - allow for the whole session?"

/* Tray-menu and system-menu items (kitty.c, kitty_launcher.c, kitty_specialmenu.c) */
#define KT_MENU_HIDE_ALL                             "&Hide all"
#define KT_MENU_UNHIDE_ALL                           "&Unhide all"
#define KT_MENU_WINDOW_UNIQUE                        "&Window unique"
#define KT_MENU_OPENED_SESSIONS                      "&Opened sessions"
#define KT_MENU_REFRESH                              "&Refresh"
#define KT_MENU_CONFIGURATION                        "&Configuration"
#define KT_MENU_TTYED                                "&TTY-ed"
#define KT_MENU_START_AT_LOGIN                       "Start &at login"
#define KT_MENU_UPDATE_AVAILABLE                     "Update available: KiTTY %s%s - install..."
#define KT_MENU_WORKPLACE_ON_LEFT                    "&Workplace proxy ON: \"%.200s\" for another %s - switch off now"
#define KT_MENU_WORKPLACE_ON                         "&Workplace proxy ON: every connection uses \"%.200s\" - switch off"
#define KT_MENU_WORKPLACE_LAST_USED                  "%.250s (last used)"
#define KT_MENU_WORKPLACE_OFF                        "&Workplace proxy mode (off) - use one proxy for everything"
#define KT_MENU_WORKPLACE_NEEDS_PROXY                "Workplace proxy mode (needs a named proxy)"
#define KT_MENU_USER_COMMAND                         "&User Command"

/* kitty.c: startup, the registry store, the .sav loader, port forwardings */
#define KT_MAIN_INFO_CLEANING_BACKUP                 "Cleaning backup registry"
#define KT_MAIN_INFO_SAVING_REGISTRY                 "Saving registry"
#define KT_MAIN_INFO_PREPARING_REGISTRY              "Preparing local registry"
#define KT_MAIN_INI_CREATE_FAILED                    "Unable to create configuration file !"
#define KT_MAIN_INFO_LOADING_SESSIONS                "Loading saved sessions."
#define KT_MAIN_INFO_LOADING_FROM_FILE               "Loading saved sessions from file."
#define KT_MAIN_INFO_FIRST_RUN_PUTTY                 "First time running. Loading saved sessions from PuTTY registry."
#define KT_MAIN_INFO_RESTORING                       "Restoring registry sessions."
#define KT_MAIN_INFO_LOADING_KEY                     "Loading %s"
#define KT_MAIN_WRONG_PASSWORD                       "Wrong password"
#define KT_MAIN_SAV_UNKNOWN_TYPE                     "Unknown value type"
#define KT_MAIN_KCHAT_LIB_FAILED                     "Unable to load library kchat.dll"
#define KT_MAIN_KCHAT_FUNC_FAILED                    "Unable to load main chat function from library kchat.dll"
#define KT_MAIN_PORTFWD_LEGEND                       "\n[C] Listening in the current process\n[X] Listening in another process\n[-] No Listening\n"
#define KT_MAIN_SESSION_SAVED                        "Settings saved to session\n-%s-"
#define KT_MAIN_CFGDIR_DIAG_DRIVE                    "Drive %c: is not available, so this looks like a " \
        "disconnected disk rather than a missing folder."
#define KT_MAIN_CFGDIR_DIAG_PARENT                   "The folder above it does exist, so only the last part " \
        "of the path is missing - a typo or a rename."
#define KT_MAIN_CFGDIR_DIAG_NEITHER                  "Neither it nor the folder above it exists."
#define KT_MAIN_CFGDIR_DIAG_LOOKED                   "KiTTY has not created or removed anything here - it only " \
        "looked."
#define KT_MAIN_CFGDIR_MISSING                       "kitty.ini points configdir at a directory that is not there:\n\n" \
        "    %s\n\n" \
        "%s\n\n" \
        "Start anyway, as if configdir had not been set?\n\n" \
        "Yes  -  start now; whatever is kept in that directory is not listed.\n" \
        "No   -  quit, so you can fix the path in kitty.ini first."
#define KT_MAIN_CFGDIR_WARN                          "KiTTY has not written to or removed that directory - this " \
        "check runs before anything is opened."
#define KT_MAIN_RESTORE_QUESTION                     "KiTTY previously ran in file mode (savemode=file in kitty.ini) and " \
        "set your registry sessions aside at:\n\n" \
        "    HKEY_CURRENT_USER\\%s\n\n" \
        "It is now starting in normal (registry) mode. Put those sessions " \
        "back?\n\n" \
        "The sessions currently in the registry are the working copy of " \
        "kitty.sav and are kept in that file, so nothing is lost either way.\n\n" \
        "Answer No and they stay set aside; you will not be asked again."
#define KT_MAIN_SET_ASIDE_NOTICE                     "KiTTY is starting in file mode (savemode=file in kitty.ini), " \
        "which keeps its sessions in kitty.sav and uses the registry " \
        "as its working copy.\n\n" \
        "Your existing registry sessions have been SET ASIDE, not " \
        "deleted. They are at:\n\n" \
        "    HKEY_CURRENT_USER\\%s\n\n" \
        "Start KiTTY without savemode=file and it will offer to put " \
        "them back."

/* kitty_commands.c: the /commands console */
#define KT_CMD_INIT_INFO                             "ConfigDirectory=%s\nIniFileFlag=%d\nDirectoryBrowseFlag=%d\nInitialDirectory=%s\nKittyIniFile=%s\nKittySavFile=%s\nKiTTYClassName=%s\n"
#define KT_CMD_SESSION_NAME_IS                       "Your session name is\n-%s-"
#define KT_CMD_NO_SESSION_NAME                       "No session name."
#define KT_CMD_SAVE_NO_SESSION                       "No saved session is associated with this window.\n" \
        "Use  /savenew <name>  to create one."
#define KT_CMD_SAVEMODE_REGISTRY                     "Savemode is \"registry\""
#define KT_CMD_SAVEMODE_FILE                         "Savemode is \"file\""
#define KT_CMD_SAVEMODE_DIR                          "Savemode is \"dir\""
#define KT_CMD_DELREG_QUESTION                       "Delete KiTTY's registry hive?\n\n" \
        "    HKEY_CURRENT_USER\\%s\n\n" \
        "This removes every saved session, named proxy, cached host key and " \
        "setting stored there. It cannot be undone from inside KiTTY - the most " \
        "recent backup is your .sav file.\n\n" \
        "Delete it?"
#define KT_CMD_COPYTOPUTTY_SAME_HIVE                 "This KiTTY is already using PuTTY's registry hive " \
        "(KiClassName=PuTTY), so there is nothing to copy: the source and the " \
        "destination are the same key. Nothing was changed."
#define KT_CMD_COPYTOKITTY_SAME_HIVE                 "This KiTTY is already using PuTTY's registry hive " \
        "(KiClassName=PuTTY): its sessions ARE PuTTY's sessions, so there is " \
        "nothing to copy. Nothing was changed."
#define KT_CMD_SWITCHCRYPT_REMOVED                   "Encrypted configuration files are no longer written.\n\n" \
        "This setting used to scramble exported .ktx files with a key built " \
        "into every copy of KiTTY, so anyone with KiTTY could unscramble them. " \
        "It protected nothing, and it is gone.\n\n" \
        "Existing encrypted .ktx files are still read normally. Saved passwords " \
        "are unaffected - those are protected properly, with Windows DPAPI or " \
        "your master password."
#define KT_CMD_RUTTY_ALSO_TEXT                       "A login script has just been loaded, and this session also has " \
        "a rutty script.\n\n" \
        "They are separate features. At CONNECT they are sequenced - the " \
        "login script gets you in, then the rutty script sends its file. " \
        "Loading one by hand mid-session skips that ordering, so if the " \
        "rutty script is already running the two will now be watching " \
        "the same output and both sending.\n\n" \
        "You can see them here:\n" \
        "    Session > Scripting        - the rutty script file\n" \
        "    Connection > Data          - the login script\n\n" \
        "Nothing has been stopped; this is only a warning."
#define KT_CMD_PASSWORD_IS                           "Your password is\n-%s-"
#define KT_CMD_NO_PASSWORD                           "No password."
/* /help: the category headings and the one-liner per command */
#define KT_CMD_CAT_WINDOW                            "Window & title (runtime toggles - persist via kitty.ini [KiTTY] size= / wintitle=)"
#define KT_CMD_CAT_INFO                              KT_CAP_INFO  /* "Info" */
#define KT_CMD_CAT_STORE                             "Settings & storage"
#define KT_CMD_CAT_ALLWIN                            "All KiTTY windows"
#define KT_CMD_CAT_DIAG                              "Behaviour & diagnostics"
#define KT_CMD_HELP_SIZE                             "toggle the [rows x cols] title suffix"
#define KT_CMD_HELP_WINTITLE                         "toggle the title decorations"
#define KT_CMD_HELP_TITLE                            "set the window title"
#define KT_CMD_HELP_TRANSPARENCY                     "toggle window transparency"
#define KT_CMD_HELP_BACKGROUNDIMAGE                  "toggle the background image feature"
#define KT_CMD_HELP_HYPERLINK                        "toggle clickable URLs"
#define KT_CMD_HELP_WINROLL                          "toggle title-bar double-click roll-up"
#define KT_CMD_HELP_REDRAW                           "repaint the window"
#define KT_CMD_HELP_REFRESH                          "refresh the background image"
#define KT_CMD_HELP_INIT                             "show configuration paths"
#define KT_CMD_HELP_SESSION                          "show the session name"
#define KT_CMD_HELP_URLREGEX                         "show the URL detection regex"
#define KT_CMD_HELP_MESSAGE                          "show a message box"
#define KT_CMD_HELP_HELP                             "show this list"
#define KT_CMD_HELP_SAVE                             "save the live settings to this window's saved session"
#define KT_CMD_HELP_SAVENEW                          "save as a NEW session and switch this window to it"
#define KT_CMD_HELP_SAVEKTX                          "export the settings to a .ktx connection file"
#define KT_CMD_HELP_SAVEMODE                         "cycle the save mode (registry / file / dir)"
#define KT_CMD_HELP_SAVEREG                          "export the KiTTY registry to kitty.sav"
#define KT_CMD_HELP_LOADREG                          "import the KiTTY registry from kitty.sav"
#define KT_CMD_HELP_DELREG                           "DELETE the whole KiTTY registry"
#define KT_CMD_HELP_SAVESESSIONS                     "export the saved sessions to kitty.ses"
#define KT_CMD_HELP_COPYTOPUTTY                      "copy the sessions to stock PuTTY (replaces its sessions)"
#define KT_CMD_HELP_COPYTOKITTY                      "copy stock PuTTY's sessions into KiTTY"
#define KT_CMD_HELP_SWITCHCRYPT                      "(removed) encrypted config files are no longer written"
#define KT_CMD_HELP_DELFOLDER                        "delete a session folder"
#define KT_CMD_HELP_LOADINITSCRIPT                   "(re)load the init script"
#define KT_CMD_HELP_COMMAND                          "run a command or send text in ALL windows"
#define KT_CMD_HELP_SIZEALL                          "resize all windows to this window's size"
#define KT_CMD_HELP_SHORTCUTS                        "reload the [Shortcuts] key bindings"
#define KT_CMD_HELP_NOSHORTCUTS                      "disable the keyboard-shortcut layer"
#define KT_CMD_HELP_NOMOUSESHORTCUTS                 "disable the mouse-shortcut layer"
#define KT_CMD_HELP_BCDELAY                          "between-character send delay"
#define KT_CMD_HELP_PRINTCHARSIZE                    "printing font size"
#define KT_CMD_HELP_PRINTMAXLINE                     "printing lines per page"
#define KT_CMD_HELP_PRINTMAXCHAR                     "printing characters per line"
#define KT_CMD_HELP_ZMODEM                           "toggle the ZModem file-transfer feature"
#define KT_CMD_HELP_FILEASSOC                        "register the .ktx file association"
#define KT_CMD_HELP_INITLAUNCHER                     "(re)create the launcher registry key"
#define KT_CMD_HELP_DEBUG                            "toggle debug mode"
#define KT_CMD_HELP_PASSWD                           "show + copy the session password (debug mode only)"
#define KT_CMD_HELP_SCREENSHOT                       "save a screenshot of the terminal"

/* kitty_launcher.c: tray tip, balloons, notices, About, the Startup-folder shortcut */
#define KT_LAUNCHER_UPDATE_BALLOON                   "KiTTY %s is available%s.\nClick here to install it."
#define KT_LAUNCHER_TIP_UPDATE                       "KiTTY Launcher - update %s%s available"
#define KT_LAUNCHER_TIP_PORTABLE                     "KiTTY Launcher\r\n(portable)"
#define KT_LAUNCHER_TIP_RESTRICTED                   "\r\n(RESTRICTED)"
#define KT_LAUNCHER_TIP_WORKPLACE                    "\r\nWorkplace proxy: %.100s%s%s"
#define KT_LAUNCHER_TIP_SWITCHES_OFF                 "\r\nSwitches off in "
#define KT_LAUNCHER_HOTKEY_DUP                       "%s%s: \"%s\" has it, \"%s\" does not."
#define KT_LAUNCHER_HOTKEY_HELD                      "%s%s (\"%s\"): held by another application."
#define KT_LAUNCHER_HOTKEY_OVERFLOW                  "%s%d more session hotkey%s beyond the %d-slot limit."
#define KT_LAUNCHER_WP_ON_LEFT                       "Every connection now uses the proxy \"%.200s\", whatever each " \
        "session says. Switches off in %s, or when this launcher stops."
#define KT_LAUNCHER_WP_ON                            "Every connection now uses the proxy \"%.200s\", whatever each " \
        "session says. It stays on until you switch it off or this " \
        "launcher stops."
#define KT_LAUNCHER_WP_TIMEOUT                       "The time you set for workplace proxy mode has run out, so it is off. " \
        "New connections use each session's own proxy settings again. Click " \
        "here to switch it on again with \"%.200s\"."
#define KT_LAUNCHER_WP_OFF                           "Workplace proxy mode is off. New connections use each session's own " \
        "proxy settings again; connections already open keep the proxy they " \
        "connected through."
/* About box: BUILD_VERSION / KITTY_TEST_BUILD_LABEL are pasted in between these where they are used */
#define KT_LAUNCHER_ABOUT_PREFIX                     "KiTTY Launcher "
#define KT_LAUNCHER_ABOUT_TESTBUILD                  "TEST BUILD: "
#define KT_LAUNCHER_ABOUT_BODY                       "\r\nQuick-launch for your saved KiTTY sessions, from the system tray.\r\n" \
        "Part of the KiTTY suite \xe2\x80\x94 a fork of PuTTY 0.84.\r\n\r\n" \
        "\xc2\xa9 KAPPER NETWORK-COMMUNICATIONS GmbH\r\n" \
        "Based on KiTTY by Cyril Dupont and PuTTY by Simon Tatham."
#define KT_LAUNCHER_STARTUP_ALLUSERS                 "This KiTTY already starts at login for all users " \
        "(an all-users Startup shortcut, usually placed by " \
        "the installer). To stop it, disable it in Settings " \
        "> Apps > Startup (that leaves the shortcut in place " \
        "but prevents it running); deleting the shortcut " \
        "itself needs administrator access to the all-users " \
        "Startup folder."
#define KT_LAUNCHER_STARTUP_OTHER_KITTY              "A \"KiTTY Launcher\" startup shortcut for a different " \
        "KiTTY already exists in your Startup folder, so none " \
        "was added.\n\nDelete it from your Startup folder " \
        "(open shell:startup) first if you want THIS KiTTY to " \
        "start at login - disabling it in Settings does not " \
        "remove the file, and its name would still clash."

/* kitty_workplace.c */
#define KT_WORKPLACE_INACTIVE_TEXT                   "It ended when the launcher holding it stopped. " \
        "Connections use each session's own proxy settings."

/* kitty_win.c: printing, the terminal notices, the title-placeholder list, the confirm box */
#define KT_WIN_PRINT_OK                              "Print successful"
#define KT_WIN_PRINT_ERR1                            "ERROR Type 1"
#define KT_WIN_PRINT_ERR2                            "ERROR Type 2."
#define KT_WIN_OK                                    "OK"
#define KT_WIN_FATAL_ERROR                           "Fatal Error"
#define KT_WIN_AUTOPW_WARN                           "You are setting a KiTTY auto-login password.\r\n\r\n" \
        "SECURITY: this password is saved in your session settings in a " \
        "REVERSIBLY-ENCRYPTED form. Anyone with access to this machine or to " \
        "your saved configuration can recover the plain-text password.\r\n\r\n" \
        "SSH public-key authentication is significantly more secure and is the " \
        "recommended way to log in automatically. Use a stored password only for " \
        "legacy hosts (such as network devices) that genuinely cannot accept key " \
        "authentication.\r\n\r\n" \
        "Store this auto-login password?"
#define KT_WIN_SESSION_NOTE_FRAME                    "\r\n\x1b[1;36m-------------------- KiTTY++ session note " \
        "--------------------\x1b[0m\r\n%s\r\n" \
        "\x1b[1;36m---------------------------------------------" \
        "-----------------\x1b[0m\r\n"
#define KT_WIN_MISSING_FEATURES_LINE                 "\r\n\x1b[1;33mNOTE:\x1b[0m this version of Windows cannot do: " \
        "%s. Everything else works as usual. Silence this with " \
        "warnmissingfeatures=no in kitty.ini.\r\n"
#define KT_WIN_AGENT_UNVERIFIED                      "This KiTTY terminal window checked which program answers its " \
        "SSH agent requests. The answer came from an unverified " \
        "program:\n\n%s\n\nThat program sees, and can sign with, every " \
        "key this session uses. That is expected if you chose to run " \
        "stock Pageant, the Windows OpenSSH agent or another agent - " \
        "click this notice to open the setting that turns the warning " \
        "off. If you did not choose that agent, find out what that " \
        "program is before trusting this session."
#define KT_WIN_TITLEVAR_H                            "Hostname (the configured host if none is known yet)"
#define KT_WIN_TITLEVAR_S                            "Saved session name"
#define KT_WIN_TITLEVAR_U                            "Username configured for the session"
#define KT_WIN_TITLEVAR_P                            "Port number"
#define KT_WIN_TITLEVAR_PROTO                        "Protocol name, e.g. SSH"
#define KT_WIN_TITLEVAR_F                            "Folder the saved session lives in"
#define KT_WIN_TITLEVAR_L                            "Local forwarded ports (blank if none)"
#define KT_WIN_TITLEVAR_D                            "Dynamic/SOCKS forwarded ports (blank if none)"

/* The update flow (kitty_win.c, kitty_launcher.c) */
#define KT_UPD_BETA_SUFFIX                           " (beta)"
#define KT_UPD_BETA_WORD                             " beta"
#define KT_UPD_BETA_MARK                             "  (BETA)"
#define KT_UPD_TERM_NOTICE                           "\r\n[KiTTY] An update is available: %s (you have %s)%s.\r\n" \
        "        System menu \xe2\x86\x92 Check for updates to install it.\r\n\r\n"
#define KT_UPD_TMP_FAILED                            "Could not create a temporary installer path; aborting the update."
#define KT_UPD_DOWNLOAD_FAILED                       "Download failed. Opening the download page instead."
#define KT_UPD_SECURE_FAILED                         "Could not secure the downloaded installer; aborting the update."
#define KT_UPD_SIG_REJECTED                          "The downloaded installer FAILED signature verification " \
        "and was NOT run; it has been deleted.\n\nPlease install KiTTY only " \
        "from the official release page."
#define KT_UPD_INSTALLER_FAILED                      "Could not start the verified installer. The downloaded file has been deleted."
#define KT_UPD_AVAILABLE_HEAD                        "An update is available.\r\n\r\nInstalled: %s\r\nLatest:    %s%s\r\n\r\n%s"
#define KT_UPD_AVAILABLE_MSI_TAIL                    "KiTTY will close and reconnect during the upgrade; the installer's " \
        "signature is verified before it runs."
#define KT_UPD_PORTABLE_OPEN_PAGE                    "Portable copy - auto-install is disabled. Open the download page?"
#define KT_UPD_NO_ASSET_OPEN_PAGE                    "The matching installer wasn't found. Open the download page?"
#define KT_UPD_STABLE_TAKING_BETA                    "You are on a STABLE release and the newest build is a BETA (less tested). "
#define KT_UPD_UP_TO_DATE_TITLE                      "KiTTY - up to date (%s%s is the latest)"
#define KT_UPD_UP_TO_DATE                            "You are running the latest version.\r\n\r\nInstalled: %s\r\nLatest:    %s"

/* kitty_bridge.c: the About version line, session export / import, the moved master password */
#define KT_BRIDGE_ABOUT_VERSION_TEST                 "KiTTY - %s\r\nTEST BUILD: %s"
#define KT_BRIDGE_ABOUT_VERSION                      "KiTTY - %s"
#define KT_BRIDGE_EXPORT_PW_SHORT                    "Please enter an export password of at least " \
        "5 characters.\n\n" \
        "If you do not want a password, choose \"Protect for this " \
        "PC only\" instead - those files can then only be imported " \
        "with this Windows account on this PC."
#define KT_BRIDGE_EXPORT_FOLDER_USED                 "This folder already contains exported session files.\n\n" \
        "Export into an empty or new folder so old and new sessions " \
        "are not mixed (use the \"New Folder\" button in the picker).\n\n" \
        "Export here anyway?"
#define KT_BRIDGE_EXPORTED                           "Exported %d session%s (%d failed) to:\n%s\n\n%s"
#define KT_BRIDGE_EXPORTED_DPAPI                     "These sessions can only be imported with THIS Windows " \
        "account on THIS PC."
#define KT_BRIDGE_EXPORTED_PW                        "This password is required to import these sessions - on ANY " \
        "PC, including this one. It is not your master password, and " \
        "nothing here was changed."
#define KT_BRIDGE_MPW_MOVED                          "This portable KiTTY kept its master password in the Windows " \
        "registry of this PC. It has now been moved into a Security folder " \
        "next to your sessions, so this copy works the same way on any PC.\r\n" \
        "\r\n" \
        "If you use other portable copies of KiTTY that share this master " \
        "password, copy this Security folder into each of them as well - " \
        "without it they cannot open their saved passwords on another PC.\r\n" \
        "\r\n" \
        "If you never knowingly set a master password: earlier versions " \
        "quietly turned the password you typed when exporting sessions into " \
        "one. That is most likely what this is."
#define KT_BRIDGE_IMPORT_PW_PROMPT                   "These sessions are password-protected.\n\nEnter the import " \
        "password - the one that was shown when they were exported. " \
        "It is not your master password."
#define KT_BRIDGE_IMPORT_PW_WRONG                    "That password did not open these files.%s"
#define KT_BRIDGE_IMPORT_PW_LAST                     " This is the last try."
#define KT_BRIDGE_IMPORT_PW_TWO                      " Two tries left."
#define KT_BRIDGE_IMPORT_PW_FAILED                   "That password does not open these sessions, so nothing was " \
        "imported.\n\n" \
        "The import password is the one that was shown when the files were " \
        "exported - not your master password."
#define KT_BRIDGE_IMPORT_DPAPI_FOREIGN               "These sessions were exported with \"this PC only\" " \
        "protection, and this is not the Windows account or the PC " \
        "they were exported from, so their saved passwords cannot be " \
        "read.\n\n" \
        "Nothing was imported. Export them again with a password to " \
        "move them to another PC."
#define KT_BRIDGE_IMPORT_COLLIDE                     "Some sessions or proxy definitions in this folder already exist " \
        "here (%d in total).\n\n" \
        "Yes  -  overwrite all matching sessions and proxies\n" \
        "No  -  import only new sessions and proxies\n" \
        "Cancel  -  do nothing"
#define KT_BRIDGE_IMPORTED_COUNTS                    "Imported %d session%s and %d prox%s"
#define KT_BRIDGE_IMPORTED_KEPT                      ", %d kept (already existed)"
#define KT_BRIDGE_IMPORTED_FAILED                    ", %d failed"
#define KT_BRIDGE_IMPORTED_FROM                      "%s from:\n%s\n\n" \
        "Saved passwords were re-protected for this storage backend " \
        "(registry: Windows DPAPI; portable files: master password)."
#define KT_BRIDGE_IMPORT_HOTKEY_SHARED               "After this import, some sessions share a launcher " \
        "hotkey:\n\n%s\n\nA hotkey works for only one session; " \
        "the launcher gives it to the first one it finds. Edit " \
        "the others to resolve this."
#define KT_BRIDGE_IMPORT_HOTKEY_LIMIT                "%s%d sessions now have a hotkey enabled, but the " \
        "launcher registers at most %d - the rest stay " \
        "inactive."

/* kitty_registry.c: -sshhandler / -fileassoc reports (console or box) and the System leaf */
#define KT_REG_CLI_CONFIRM_PROMPT                    "\r\nWrite these registry entries? [y/N] "   /* 38 bytes: the WriteFile length beside it */
#define KT_REG_PORTABLE_QUESTION                     "This is a portable KiTTY. Registering %s writes to this " \
        "machine's registry, pointing at:\r\n\r\n    %s\r\n\r\n" \
        "Those entries stay behind when this copy is removed, and then " \
        "point at nothing."
#define KT_REG_NOTHING_REGISTERED                    "Nothing was registered."
#define KT_REG_ELEVATION_REFUSED                     "This is the machine-wide installation of KiTTY, so this belongs " \
        "to the whole machine - and that was refused or cancelled.\r\n" \
        "Re-run from an administrator prompt, or add -user to register " \
        "for your account only."
#define KT_REG_URL_WHAT                              "the telnet://, ssh:// and kitty:// handlers"
#define KT_REG_ASSOC_WHAT                            "the %s file association"
#define KT_REG_FOR_ALL_USERS                         "For all users of this machine (HKEY_LOCAL_MACHINE)."
#define KT_REG_FOR_YOUR_ACCOUNT                      "For your account only (HKEY_CURRENT_USER)."
#define KT_REG_YOUR_ACCOUNT                          "your account"
#define KT_REG_ALL_USERS                             "all users"
#define KT_REG_ANOTHER_PROGRAM                       "another program"
#define KT_REG_PREVIOUS_PROGRAM                      "the previous program"
#define KT_REG_NEEDS_ADMIN                           " - needs administrator rights"
#define KT_REG_URL_REPORT_HEAD                       "%s\r\nRegistering: %s\r\n\r\n"
#define KT_REG_URL_ALREADY                           "%s://  already registered for this KiTTY\r\n"
#define KT_REG_URL_LEFT_ALONE                        "%s://  LEFT ALONE, currently opened by %s\r\n" \
        "           %s\r\n"
#define KT_REG_URL_NOT_WRITTEN                       "%s://  COULD NOT BE WRITTEN\r\n"
#define KT_REG_URL_TAKEN_OVER                        "%s://  taken over from %s\r\n           %s\r\n"
#define KT_REG_URL_UNDO_IMPORT                       "           to undo:  reg import \"%s\"\r\n"
#define KT_REG_URL_NO_BACKUP                         "           (the old setting could NOT be backed up)\r\n"
#define KT_REG_URL_UNDO_DELETE                       "           to undo:  reg delete \"%s\\%s%s\" /f\r\n" \
        "           (%s was not changed - deleting the entry above " \
        "lets %s open %s:// links again)\r\n"
#define KT_REG_URL_REG_YOUR_ACCOUNT                  "the registration for your account"
#define KT_REG_URL_REG_ALL_USERS                     "the registration for all users of this machine"
#define KT_REG_URL_REGISTERED                        "%s://  registered\r\n"
#define KT_REG_URL_KEPT_NOTE                         "\r\n%d left untouched because %s already opened by another " \
        "program. Add -force to take %s over as well; the setting " \
        "replaced is exported to a .reg file first, and the report then " \
        "names the command that puts it back."
#define KT_REG_URL_REMOVING                          "Removing KiTTY's telnet://, ssh://, kitty:// and putty:// " \
        "handlers.\r\n\r\n"
#define KT_REG_URL_RM_LEFT_ALONE                     "%s:// (%s)  LEFT ALONE, opened by %s\r\n"
#define KT_REG_URL_REMOVED                           "%s:// (%s)  removed\r\n"
#define KT_REG_URL_NOT_REMOVED                       "%s:// (%s)  could not be removed%s\r\n"
#define KT_REG_URL_NOTHING_REGISTERED                "Nothing of KiTTY's was registered%s."
#define KT_REG_URL_OTHERS                            " (the handlers above belong to other programs)"
#define KT_REG_ASSOC_REPORT_HEAD                     "%s\r\nAssociating %s with: %s\r\n\r\n"
#define KT_REG_ASSOC_LEFT_ALONE                      "%s  LEFT ALONE, currently opened by \"%s\"\r\n\r\n" \
        "Add -force to take it over; the current setting is exported to " \
        "a .reg file first and this report then names the command that " \
        "restores it."
#define KT_REG_ASSOC_NOT_WRITTEN                     "The registration could NOT be written."
#define KT_REG_ASSOC_TAKEN_OVER                      "%s  taken over from \"%s\"\r\n"
#define KT_REG_ASSOC_UNDO_IMPORT                     "     to undo:  reg import \"%s\"\r\n"
#define KT_REG_ASSOC_NO_BACKUP                       "     (the old setting could NOT be backed up)\r\n"
#define KT_REG_ASSOC_UNDO_DELETE                     "     to undo:  reg delete \"%s\\Software\\Classes\\%s\" /f\r\n" \
        "     (%s was not changed - deleting the entry above lets " \
        "\"%s\" open %s files again)\r\n"
#define KT_REG_ASSOC_YOUR_ACCOUNT                    "the association for your account"
#define KT_REG_ASSOC_ALL_USERS                       "the association for all users of this machine"
#define KT_REG_ASSOC_DONE                            "%s  associated\r\n"
#define KT_REG_ASSOC_REMOVING                        "Removing KiTTY's %s file association.\r\n\r\n"
#define KT_REG_ASSOC_RM_LEFT_ALONE                   "%s (%s)  LEFT ALONE, opened by \"%s\"\r\n"
#define KT_REG_ASSOC_REMOVED                         "%s (%s)  removed\r\n"
#define KT_REG_ASSOC_NOT_REMOVED                     "%s (%s)  could not be removed%s\r\n"
#define KT_REG_PROGID_REMOVED                        "kitty.connect.1 (%s)  removed\r\n"
#define KT_REG_ASSOC_NOTHING                         "Nothing of KiTTY's was associated."
#define KT_REG_STATE_NOT_REGISTERED                  "%s  not registered"
#define KT_REG_STATE_OTHER_KITTY                     "%s  another KiTTY: %s"

/* kitty_xfer.c: the pscp transfer window, the clipboard command, WinSCP */
#define KT_XFER_HELLO_NOTE                           "This session's key is protected by Windows Hello, and the agent " \
        "does not hold it.\r\n" \
        "A transfer client cannot open a protected key file itself. Load " \
        "the key in kageant (one Hello) and start the transfer again.\r\n" \
        "Key: %s\r\n\r\n"
#define KT_XFER_TRANSFER                             "Transfer"
#define KT_XFER_FILE                                 "file"
#define KT_XFER_BTN_CANCEL                           "&Cancel"
#define KT_XFER_BTN_CLOSE                            "&Close"
#define KT_XFER_BTN_STOPPING                         "Stopping..."
#define KT_XFER_COMPLETE                             "%s complete."
#define KT_XFER_COMPLETE_LINE                        "\r\n==== %s complete ====\r\n"
#define KT_XFER_CANCELLED_LINE                       "\r\n==== %s cancelled ====\r\n"
#define KT_XFER_HINT_127                             "\r\n\r\nExit 127 = the server could not start the SCP/SFTP " \
        "subsystem (command not found). Try switching the transfer " \
        "protocol (Connection -> SSH -> WinSCP) between SCP " \
        "and SFTP, or check the server's sftp-server/scp."
#define KT_XFER_FAILED_LINE                          "\r\n==== %s FAILED  (pscp exit code %lu) ====%s\r\n"
#define KT_XFER_FAILED_TITLE                         "KiTTY transfer - FAILED (exit %lu)"
#define KT_XFER_LAUNCH_FAILED                        "Could not launch the transfer client (pscp)."
#define KT_XFER_WINDOW_TITLE                         "KiTTY transfer - %s"
#define KT_XFER_UPLOAD_OF                            "Upload of \"%s\""
#define KT_XFER_UPLOADING                            "%sUploading  %s  ->  %s\r\n\r\n"
#define KT_XFER_DOWNLOAD_OF                          "Download of \"%s\""
#define KT_XFER_RUN_CLIP_PROMPT                      "Run this command from the clipboard?\n\n%s"
#define KT_XFER_RAN_CLIP                             "Ran clipboard command:\n%s"
#define KT_XFER_START_WINSCP_ANYWAY                  "%sStart WinSCP anyway?"

/* kitty_zmodem.c */
#define KT_ZM_HELPER_LOG                             "ZModem helper: %s"
#define KT_ZM_UNSET                                  "(unset)"
#define KT_ZM_RZ_NOT_FOUND                           "Unable to find ZModem receive program:\n%s"
#define KT_ZM_RZ_START_FAILED                        "Unable to start ZModem receive."
#define KT_ZM_SZ_NOT_FOUND                           "Unable to find ZModem send program:\n%s"
#define KT_ZM_SZ_START_FAILED                        "Unable to start ZModem send."
#define KT_ZM_SELECT_FILES                           "Select file(s) to send by ZModem..."

/* kitty_rutty.c: Event Log lines */
#define KT_RUTTY_TIMEOUT                             "script timeout !"
#define KT_RUTTY_FILE_NOT_FOUND                      "script file not found"
#define KT_RUTTY_READ_FAILED                         "script file read failed"
#define KT_RUTTY_SENDING                             "sending script to host ..."
#define KT_RUTTY_HALTED                              "script halted"

/* kitty_osc52.c: the clipboard-read permission dialog */
#define KT_OSC52_SUMMARY                             "It would send %d character%s on %d line%s, starting \"%s\"..."
#define KT_OSC52_SUMMARY_HIDDEN                      "\r\nThe rest is hidden until you press View."
#define KT_OSC52_WHERE_SESSION                       "Session \"%s\" (%s)"
#define KT_OSC52_WHERE_UNSAVED                       "Unsaved session to %s"
#define KT_OSC52_COUNTDOWN                           "No answer in %d s = refused, and nothing remembered."
#define KT_OSC52_WHAT                                "This server wants to READ your clipboard and send the " \
        "contents back to it. Your clipboard may hold a password."
#define KT_OSC52_CLAIM                               "A program calling itself \"%s\""
#define KT_OSC52_NO_CLAIM                            "The request does not say which program sent it, and " \
        "cannot."
#define KT_OSC52_MINUTES                             "the next %d &minute%s"
#define KT_OSC52_REQUESTS                            "the next %d re&quests"
#define KT_OSC52_SESSION_CONFIRM                     "For the rest of this session, this server may " \
        "read your clipboard whenever it asks - not once, " \
        "but every time.\n\n" \
        "That includes anything you copy later, such as a " \
        "password from your password manager. It still " \
        "stops while the window has no focus, and there is " \
        "still a limit on how often it is handed over.\n\n" \
        "Allow that?"

/* kitty_adb.c: connection errors */
#define KT_ADB_FAILURE                               "adb failure message: '%s'"
#define KT_ADB_BAD_HELLO                             "Bad response after initial send"
#define KT_ADB_BAD_SHELL                             "Bad response waiting for shell start"

/* kitty_proxy.c */
#define KT_PROXYDEF_DIR_FAILED                       "Unable to create the proxy definitions directory"

/* kitty_hello_container.c: the owners list of a key container */
#define KT_HELLOC_UNTAGGED                           KT_KGEN_DOORS_UNTAGGED  /* "(untagged)" - kittygen's doors list says the same */
#define KT_HELLOC_HELLO_CRED                         "(Windows Hello key credential)"

/* kitty_oldwin.c: the "this Windows cannot do" reports */
#define KT_OLDWIN_UNNAMED                            "(unnamed)"
#define KT_OLDWIN_TOO_OLD                            "This version of Windows is too old to run KiTTY.\r\n\r\n" \
        "It is missing:"
#define KT_OLDWIN_NOT_AVAILABLE                      "Not available on this version of Windows:"
#define KT_OLDWIN_ITEM                               "\r\n    %s - needs %s from %s"
#define KT_OLDWIN_NEEDS_XP                           "\r\n\r\nKiTTY needs Windows XP or newer."
/* The feature names those reports print, one per runtime-resolved API. The
 * brief report de-duplicates by comparing these strings, so a feature that
 * several lookups share must use the SAME macro (kitty.c, kitty_oldwin.c,
 * kitty_theme.c, kitty_win.c, kitty_osc52.c, kitty_notice.c, kitty_image.c). */
#define KT_WINFEAT_TCP_PORT_OWNER                    "naming the program that owns a TCP port"
#define KT_WINFEAT_TICK64                            "a 64-bit millisecond clock"
#define KT_WINFEAT_PROCESS_NAME                      "naming the program behind a process"
#define KT_WINFEAT_PROCESS_NAME_OLD                  "naming the program behind a process (older Windows)"
#define KT_WINFEAT_CONSOLE                           "printing to the console that started KiTTY"
#define KT_WINFEAT_REG_DELTREE                       "one-call registry subtree deletion"
#define KT_WINFEAT_REG_GETVALUE                      "typed registry reads"
#define KT_WINFEAT_CONPTY                            "ConPTY process spawning"
#define KT_WINFEAT_AGENT_PIPE                        "identifying which process serves the SSH agent pipe"
#define KT_WINFEAT_RESTART                           "automatic restart after an in-place upgrade"
#define KT_WINFEAT_WINVER                            "detecting the Windows version"
#define KT_WINFEAT_DARK_MODE                         "dark mode"
#define KT_WINFEAT_DARK_CONTROLS                     "dark scroll bars and controls"
#define KT_WINFEAT_DARK_TITLEBARS                    "dark title bars"
#define KT_WINFEAT_ACCENT                            "matching the desktop's accent colour"
#define KT_WINFEAT_DPI                               "per-monitor DPI scaling"

/* ---- Batch 5: kageant (agent) and kittygen (key generator), Windows Hello ---- */

/* kageant: window titles and message-box captions (windows/pageant.c,
 * kitty/kitty_pageant.c) */
#define KT_CAP_KA_FATAL                              "kageant Fatal Error"
#define KT_CAP_KA_ERROR                              "kageant Error"
#define KT_CAP_KA_CMDLINE_ERROR                      "kageant command line error"
#define KT_CAP_KA_CMDLINE                            "kageant command line"
#define KT_CAP_KA_KEY_LIST                           "kageant Key List"
#define KT_CAP_KA_KEY_DETAILS                        "kageant - key details"
#define KT_CAP_KA_AGENT_LOG                          "kageant - agent log"
#define KT_CAP_KA_SETTINGS                           "kageant - settings"
#define KT_CAP_KA_LAUNCH_BLOCKED                     "kageant - session launch blocked"
#define KT_CAP_KA_HELLO_ADD_HERE                     "kageant - add Windows Hello here"
#define KT_CAP_KA_NOT_ADDED                          "kageant - not added"
#define KT_CAP_KA_HELLO_ELSEWHERE                    "kageant - Windows Hello key from elsewhere"
#define KT_CAP_KA_HELLO_ONLY                         "kageant - Windows Hello only"
#define KT_CAP_KA_NOT_PROTECTED                      "kageant - not protected"
#define KT_CAP_KA_PROTECT_KEY_Q                      "kageant - protect this key?"
#define KT_CAP_KA_CANNOT_READ_FILE                   "kageant - cannot read that file"
#define KT_CAP_KA_ACCEPT_CHANGED_Q                   "kageant - accept this changed key?"
#define KT_CAP_KA_NOT_ACCEPTED                       "kageant - not accepted"
#define KT_CAP_KA_KEY_PROTECTED                      "kageant - key protected"
#define KT_CAP_KA_FORGET_PATH                        "kageant - forget a path"
#define KT_CAP_KA_NOT_REPOINTED                      "kageant - not re-pointed"
#define KT_CAP_KA_KEYGEN_UNVERIFIED                  "kageant - key generator not verified"
#define KT_CAP_KA_AUTOSTART_CONFLICT                 "kageant - autostart conflict"
#define KT_CAP_KA_ADD_KEY_TO_STARTUP                 "kageant - add key to startup"
#define KT_CAP_KA_CONFIRM_KEY_USAGE                  "Confirm SSH key usage"

/* kageant: notice-window titles (kitty_notice_show) */
#define KT_KA_NOTICE_OLD_FORMAT                      "kageant: a remembered key uses the old file format"
#define KT_KA_NOTICE_HELLO_ELSEWHERE                 "kageant: Windows Hello key from elsewhere"
#define KT_KA_NOTICE_STARTUP_KEY_FAILED              "kageant: a remembered key did not load"
#define KT_KA_NOTICE_HELD_BACK                       "kageant: a key is being held back"
#define KT_KA_NOTICE_KEY_FILE_CHANGED                "kageant: key file changed"
#define KT_KA_NOTICE_LOADED_UNVERIFIED               "kageant: keys loaded unverified"
#define KT_KA_NOTICE_NEW_DRIVE_NOT_LOADED            "kageant: key on a new drive not loaded"
#define KT_KA_NOTICE_NOTHING_TO_RETRY                "kageant: nothing to retry"
#define KT_KA_NOTICE_KEYS_NOT_LOADED                 "kageant: keys not loaded"
#define KT_KA_NOTICE_RETRY_FINISHED                  "kageant: retry finished"
#define KT_KA_NOTICE_STARTUP_KEYS                    "kageant: startup keys"
#define KT_KA_NOTICE_KEY_ADDED                       "kageant - key added"
#define KT_KA_NOTICE_KEY_REMOVED                     "kageant - key removed"
#define KT_KA_NOTICE_ALL_KEYS_REMOVED                "kageant - ALL keys removed"
#define KT_KA_NOTICE_USE_DENIED                      "kageant: key use DENIED"
#define KT_KA_NOTICE_CONFIRM_BLOCKED                 "kageant: confirmations blocked"
#define KT_KA_NOTICE_UNPROTECTED                     "kageant: keys are NOT memory-protected"
#define KT_KA_NOTICE_KEY_USED                        "kageant: SSH key used"

/* kageant: tray menu items */
#define KT_KA_MENU_OPENSSH                           "Register as Windows &OpenSSH agent"
#define KT_KA_MENU_START_AT_LOGIN                    "&Start kageant at login"
#define KT_KA_MENU_LOAD_KEYS                         "&Load remembered keys at startup"
#define KT_KA_MENU_NOTIFY                            "&Notify when a key is used"
#define KT_KA_MENU_CONFIRM                           "Ask &confirmation before each key use"
#define KT_KA_MENU_RESUME                            "Res&ume key-use confirmations"
#define KT_KA_MENU_SETTINGS                          "Settin&gs..."
#define KT_KA_MENU_AGENT_LOG                         "Agent lo&g..."

/* kageant: tray tip (128 chars; the state suffixes are composed in
 * kitty_title.c) */
#define KT_KA_TIP                                    "kageant (KiTTY authentication agent)"
#define KT_KA_TIP_INI                                "kageant (KiTTY authentication agent)\r\n(kitty.ini mode)"
#define KT_KA_TIP_UNPROTECTED_FMT                    "%.*s\r\nkeys UNPROTECTED in memory%s"
#define KT_KA_TIP_AUTOENC_FMT                        "%.*s\r\nidle re-encrypt enforced: %s%s"
#define KT_KA_TIP_MISMATCH_ONE                       "1 key NOT loaded - fingerprint mismatch"
#define KT_KA_TIP_MISMATCH_MANY_FMT                  "%d keys NOT loaded - fingerprint mismatch"

/* kageant: the command line */
#define KT_KA_CLI_HELP \
        "kageant - the KiTTY SSH authentication agent\n" \
        "\n" \
        "Usage:  kageant [options] [keyfile ...]\n" \
        "\n" \
        "Key files named on the command line are loaded at startup.\n" \
        "\n" \
        "-encrypted, -no-decrypt\n" \
        "        load the key files that follow deferred: the\n" \
        "        passphrase is asked at first use\n" \
        "-keylist\n" \
        "        open the key list window at startup\n" \
        "-noload\n" \
        "        clean slate: do not load the stored startup keys (no\n" \
        "        passphrase prompts) and leave the stored list untouched;\n" \
        "        key files named on the command line still load\n" \
        "-c command [args ...]\n" \
        "        run the command once the agent is up; everything\n" \
        "        after -c is the command line\n" \
        "-openssh-config FILE\n" \
        "        write an OpenSSH client config file pointing ssh at\n" \
        "        this agent's named pipe\n" \
        "-unix PATH\n" \
        "        also serve an AF_UNIX agent socket at PATH\n" \
        "-restrict-acl\n" \
        "        restrict the ACL of the kageant process\n" \
        "-restrict-putty-acl\n" \
        "        pass -restrict-acl on to KiTTY sessions started\n" \
        "        from the tray menu\n" \
        "-pgpfp\n" \
        "        show the PGP fingerprints of the PuTTY release keys\n" \
        "        (deprecated)\n" \
        "-h, -help, --help\n" \
        "        this summary\n"

/* kageant: message boxes and notices (windows/pageant.c) */
#define KT_KA_RANDOM_FAILED                          "The system random number generator failed"
#define KT_KA_LAUNCH_BLOCKED                         "The kitty.exe next to kageant could not be verified as a " \
        "genuine, matching KiTTY build - its signature or version " \
        "did not check out.\n\n" \
        "It may have been replaced with something else. Because a " \
        "terminal started from here would get access to the agent's " \
        "keys, kageant will not start it."
#define KT_KA_HELLO_ENROL_OFFER_FMT                  "The recovery passphrase opened this Windows Hello protected key:\n\n" \
        "    %s\n\n" \
        "Add Windows Hello on this computer and account as a way to open " \
        "it, so the passphrase is not needed here next time?\n\n" \
        "(The file's .hello sidecar gets one more entry. Other computers " \
        "and accounts keep theirs.)"
#define KT_KA_HELLO_NOT_ADDED_FMT                    "Windows Hello was not added for this key:\n\n%s"
#define KT_KA_PASS_PROMPT_HELLO_MODAL                "Enter the passphrase, the RECOVERY " \
        "passphrase or the printed secret:"
#define KT_KA_PASS_PROMPT_HELLO_NONMODAL             "input focus, then enter the passphrase, " \
        "RECOVERY passphrase or printed secret."
#define KT_KA_OLD_FORMAT_TEXT                        "A key in the startup list is an SSH-2 key in the old PPK " \
        "format, which is not fully tamperproof and may stop being " \
        "supported. Load it into KiTTYgen and save it again to " \
        "convert it."
#define KT_KA_HELLO_NOT_ADDED_UNPROTECTED_FMT        "The key was NOT added:\n\n    %s\n\n" \
        "You asked for Windows Hello protection and it did not " \
        "complete, so the file was not loaded unprotected. Add " \
        "it again and answer No to load it as it is."
#define KT_KA_HELLO_CANCELLED_PROMPT_FMT             "Windows Hello was cancelled - enter " \
        "the recovery passphrase or the " \
        "printed secret for %s"
#define KT_KA_HELLO_ELSEWHERE_ADD_FMT                "%s\n\nwas protected with Windows Hello by: %s\n\n" \
        "This computer and account cannot open it with Hello. " \
        "Enter the recovery passphrase or the printed secret " \
        "at the next prompt; the passphrase then also offers " \
        "to add Windows Hello here."
#define KT_KA_HELLO_ELSEWHERE_USE_FMT                "%s\n\nwas protected with Windows Hello by: %s\n\n" \
        "This computer and account cannot open it with Hello. " \
        "Enter the recovery passphrase or the printed secret."
#define KT_KA_HELLO_CANCELLED_USE_FMT                "Windows Hello was cancelled - recovery passphrase or " \
        "printed secret for %s"
#define KT_KA_HELLO_RECOVERY_USE_FMT                 "recovery passphrase or printed secret for %s"
#define KT_KA_UNKNOWN_OWNER                          "(unknown)"
#define KT_KA_STARTUP_KEY_FAILED_FMT                 "%s\n\n%s\n\n" \
        "It is still in the key list, and kageant will try it again at " \
        "the next start. Click to open the list, where Remove drops it " \
        "for good."
#define KT_KA_NOT_PROTECTED_FMT                      "The key was not protected:\n\n%s"
#define KT_KA_PROTECT_OFFER_FMT                      "This key has no passphrase:\n\n    %s\n\n" \
        "Protect it with Windows Hello now? A protected COPY is written " \
        "beside it (the original is not changed) and the startup list " \
        "remembers the copy."
#define KT_KA_PROTECT_RETRY_Q                        "The key was not protected.\n\n" \
        "Yes = try protecting it again\n" \
        "No = load it UNPROTECTED\n" \
        "Cancel = do not add it at all"
#define KT_KA_KITTYGEN_MISSING                       "The KiTTY key generator (kittygen) was not found " \
        "next to kageant."
#define KT_KA_KITTYGEN_UNVERIFIED_Q                  "The key generator next to kageant could not be " \
        "verified as a genuine, matching KiTTY build - its " \
        "signature or version did not check out.\n\n" \
        "It may simply be a different version, or it may " \
        "have been replaced with something else.\n\n" \
        "Start it anyway?"
#define KT_KA_STOP_AGENT_Q                           "Stop the kageant agent?\n\n" \
        "Every loaded key is unloaded, and any program using " \
        "the agent (PuTTY sessions, ssh, WinSCP...) loses " \
        "access until kageant is started again."
#define KT_KA_OPENSSH_NO_PIPE                        "Cannot register as the Windows OpenSSH agent: " \
        "this kageant has no named-pipe listener."
#define KT_KA_OPENSSH_REGISTERED_FMT                 "kageant is now the Windows OpenSSH agent.\n\n" \
        "Added an \"Include kageant.conf\" block to:\n%s\n\n" \
        "(A one-time .kageant.bak backup was saved. Untick this " \
        "item to remove the block again.)"
#define KT_KA_AUTOSTART_CONFLICT_FMT                 "Another SSH agent is already set to start at login:\n\n" \
        "    %s\n\n" \
        "If you add this kageant too, BOTH start at login and " \
        "only one wins (single-instance) - the other exits " \
        "without loading its keys, and which wins is a race.\n\n" \
        "Add this kageant to autostart anyway?\n\n" \
        "Choose No to leave autostart unchanged. To make THIS " \
        "kageant your login agent, disable the other one in " \
        "Settings > Apps > Startup (that stops it starting, for " \
        "both Run entries and Startup shortcuts) or delete its " \
        "Run-registry value / Startup shortcut."
#define KT_KA_AUTOSTART_PORTABLE_FMT                 "kageant will load your current %d key(s) at startup, added " \
        "encrypted (passphrase asked on first use).\n\n" \
        "The key list is saved to kitty.ini so it travels with this " \
        "portable install (keys inside the install folder are stored " \
        "relative to it; a key added from elsewhere prompts to be " \
        "copied in or referenced).\n\n" \
        "A login shortcut to this kageant was placed in your Startup " \
        "folder (no registry entry). It is machine-local and pinned " \
        "to the current path, so it does not follow the stick to " \
        "another machine or drive letter - disable this here to " \
        "remove it."
#define KT_KA_AUTOSTART_REGISTRY_FMT                 "kageant will load your current %d key(s) at login, added " \
        "encrypted (passphrase asked on first use).\n\n" \
        "An autostart entry was added (HKCU ...\\Run\\%s), so you can " \
        "remove any manual kageant Startup shortcut. Newly added keys " \
        "are remembered automatically while this stays enabled."

/* kageant: the "Protect with Windows Hello" dialog (windows/pageant.c) */
#define KT_KA_HP_EXPLAIN_FMT                         "%s" \
        "The original is not changed and keeps working as it does today. " \
        "The protected copy is the same key with a random secret as its " \
        "passphrase: Windows Hello opens it here, %s and the printed " \
        "secret (shown once, next) is that passphrase itself - it opens " \
        "the key in any PuTTY tool.\r\n\r\n" \
        "Delete the original yourself once the copy works; \"Forget a " \
        "path\" drops it from this key's list."
#define KT_KA_HP_NO_PASSPHRASE                       "This key has NO passphrase. Keeping the original beside the " \
        "protected copy defeats the protection for whoever holds both " \
        "files.\r\n\r\n"
#define KT_KA_HP_HELLO_ONLY_CLAUSE                   "NO recovery passphrase: lose Windows Hello here (new PC, " \
        "re-enrolment, TPM reset) and ONLY the printout opens it -"
#define KT_KA_HP_RECOVERY_CLAUSE                     "the recovery passphrase opens it anywhere,"
#define KT_KA_HP_SAVE_AS                             "Save the protected copy as"
#define KT_KA_HP_NEED_NAME                           "Name the protected copy."
#define KT_KA_HP_SAME_FILE                           "The protected copy must be a different file - " \
        "the original is never rewritten."
#define KT_KA_HP_EXISTS                              "That file already exists. Choose another name; " \
        "nothing is overwritten."
#define KT_KA_HP_NEED_SRCPASS                        "Enter the key's current passphrase."
#define KT_KA_HP_NEED_RECPASS                        "Enter a recovery passphrase - it is the only way " \
        "to open the key where Windows Hello cannot."
#define KT_KA_HP_RECPASS_MISMATCH                    "The recovery passphrases do not match."
#define KT_KA_HP_WRONG_SRCPASS                       "That is not this key's current passphrase."
/* shown by kageant and kittygen alike */
#define KT_HELLO_ONLY_WARN_Q                         "No recovery passphrase: if Windows Hello on this " \
        "computer is lost, ONLY the printed secret opens " \
        "this key. Store the printout. Continue?"

/* kageant: the key list window - cells, columns, buttons (windows/pageant.c) */
#define KT_KAKEYS_COL_ALGORITHM                      "Algorithm"
#define KT_KAKEYS_COL_BITS                           "Bits"
#define KT_KAKEYS_COL_FINGERPRINT                    "Fingerprint"
#define KT_KAKEYS_COL_STATE                          "State"
#define KT_KAKEYS_COL_LIFETIME                       "Lifetime"
#define KT_KAKEYS_COL_CONFIRM                        "Confirm"
#define KT_KAKEYS_COL_COMMENT                        "Comment"
#define KT_KAKEYS_STATE_ENCRYPTED                    "encrypted"
#define KT_KAKEYS_STATE_REENCRYPTABLE                "re-encryptable"
#define KT_KAKEYS_STATE_LOADED                       "loaded"
#define KT_KAKEYS_STATE_MISMATCH                     "mismatch"
#define KT_KAKEYS_STATE_FAILED                       "failed"
#define KT_KAKEYS_STATE_MISSING                      "missing"
#define KT_KAKEYS_ALG_NOT_LOADED                     "(not loaded)"
#define KT_KAKEYS_LIFETIME_UNLIMITED                 "unlimited"
#define KT_KAKEYS_CONFIRM_HELLO                      "Hello"
#define KT_KAKEYS_CONFIRM_REQUIRED                   "required"
#define KT_KAKEYS_BTN_DECRYPT                        "&Decrypt"
#define KT_KAKEYS_BTN_REENCRYPT                      "Re-e&ncrypt"
#define KT_KAKEYS_INI_STATUS_FMT                     "Settings file (kitty.ini mode): %s"
#define KT_KAKEYS_REMOVE_ONE_Q                       "Remove the selected key from the agent?\n\n" \
        "It will also stop being loaded at startup."
#define KT_KAKEYS_REMOVE_MANY_Q_FMT                  "Remove the %d selected keys from the agent?\n\n" \
        "They will also stop being loaded at startup."

/* kageant: the key details dialog (windows/pageant.c, kitty_pageant.c) */
#define KT_KAKEYS_LIFETIME_NOT_LOADED                "not loaded"
#define KT_KAKEYS_LIFETIME_SET_FMT                   "set to %s - %s remaining"
#define KT_KAKEYS_LIFETIME_EXPIRED                   "expired - key removed"
#define KT_KAKEYS_AUTOENC_ENFORCED_FMT               "Enforced by the agent setting: %s"
#define KT_KAKEYS_AUTOENC_DEFAULT_FMT                "blank = the agent default (%s)"
#define KT_KAKEYS_AUTOENC_OFF                        "blank = off (no agent default)"
#define KT_KAKEYS_DETAIL_ENCRYPTED                   "encrypted - the passphrase is asked for at " \
        "first use"
#define KT_KAKEYS_DETAIL_REENCRYPTABLE               "loaded, and the key file it came from is " \
        "encrypted"
#define KT_KAKEYS_DETAIL_MISSING                     "not loaded - the key file is not reachable " \
        "(absent media, or a path that no longer exists)"
#define KT_KAKEYS_DETAIL_FAILED                      "not loaded - the file is present but would not " \
        "load"
#define KT_KAKEYS_DETAIL_MISMATCH                    "NOT loaded - the file at this path is not the key " \
        "recorded for it. Either you replaced it, or " \
        "something else did. If you replaced it, use " \
        "\"Accept this key\" below; until then it stays " \
        "refused at every start and every re-plug."
#define KT_KAKEYS_DETAIL_LOADED                      "loaded and ready to use"
#define KT_KAKEYS_FP_SHA256                          "SHA-256:  "
#define KT_KAKEYS_FP_MD5                             "MD5:  "
#define KT_KAKEYS_FP_SHA256_CERT                     "SHA-256 incl. certificate:  "
#define KT_KAKEYS_FP_MD5_CERT                        "MD5 incl. certificate:  "
#define KT_KAKEYS_FP_RECORDED                        "recorded at last save:  "
#define KT_KAKEYS_FP_NONE                            "(none)"
#define KT_KAKEYS_PATHS_UNKNOWN                      "not known - this key was added by another " \
        "program, or by a build that did not record " \
        "it"
#define KT_KAKEYS_FILE_HELLO                         "Windows Hello protected"
#define KT_KAKEYS_FILE_PASSPHRASE                    "passphrase"
#define KT_KAKEYS_FILE_UNPROTECTED                   "UNPROTECTED - no passphrase"
#define KT_KA_PATH_NOT_REACHABLE                     "   (not reachable right now)"
#define KT_KAKEYS_CONFIRM_NO                         "no"
#define KT_KAKEYS_CONFIRM_ASK                        "ask before each use"
#define KT_KAKEYS_CONFIRM_ASK_HELLO                  "ask with Windows Hello"
#define KT_KAKEYS_CANNOT_READ_FILE                   "The key file cannot be read right now, so there is " \
        "nothing to accept. Check the file is reachable and " \
        "try again."
#define KT_KAKEYS_ACCEPT_Q_FMT                       "Accept the key that is in this file NOW, and remember it?\n\n" \
        "    %s\n\n" \
        "Recorded before:  %s\n" \
        "In the file now:  %s\n\n" \
        "Only do this if YOU replaced the key. Accepting means this " \
        "file is loaded now and trusted at every future start."
#define KT_KAKEYS_ACCEPT_HELLO_Q                     "Accept the changed key file and " \
        "trust it from now on?"
#define KT_KAKEYS_ACCEPT_HELLO_FAILED                "The Windows Hello check did not verify, so the " \
        "key was NOT accepted and nothing was changed."
#define KT_KAKEYS_ACCEPT_LOAD_FAILED                 "The key could not be loaded, so nothing was " \
        "changed and the entry stays refused."
#define KT_KAKEYS_PROTECTED_FMT                      "Protected copy written:\n\n    %s\n    %s.hello\n\n%s\n\n" \
        "The original stays where it is:\n\n    %s"
#define KT_KAKEYS_STARTUP_REPLACED                   "The startup list now names the protected " \
        "copy instead of the original."
#define KT_KAKEYS_STARTUP_UNCHANGED                  "The startup list was not changed."
#define KT_KAKEYS_FORGET_Q_FMT                       "Forget this path (%d of %d)?\n\n    %s\n\n%s" \
        "The startup list stops naming this file. The file " \
        "itself is not touched."
#define KT_KAKEYS_FORGET_ONLY_ENTRY                  "THIS IS THE KEY'S ONLY STARTUP ENTRY. Forgetting it " \
        "means the key is no longer loaded at startup (it " \
        "stays loaded now). To take the key out of the agent " \
        "use Remove instead.\n\n"
#define KT_KAKEYS_FORGET_OTHERS                      "The key stays loaded and its other paths stay.\n\n"
#define KT_KAKEYS_LOCATE_TITLE                       "Locate the key file"
#define KT_KAKEYS_LOCATE_DIFFERENT                   "That file holds a DIFFERENT key, not the one recorded " \
        "for this entry - nothing was changed. Add Key loads " \
        "it as a new key; this button only re-points the entry " \
        "at its own key."
#define KT_KAKEYS_LOCATE_UNLOADABLE                  "No key could be loaded from that file, so nothing " \
        "was changed."

/* kageant: the agent log window (windows/pageant.c) */
#define KT_KALOG_COL_TIME                            "Time"
#define KT_KALOG_COL_EVENT                           "Event"
#define KT_KALOG_COL_RESULT                          "Result"
#define KT_KALOG_COL_KEY                             KT_HOST_KEYS_KEY  /* "Key" - the same word as the host-keys column */
#define KT_KALOG_COL_REQUESTER                       "Requester"
#define KT_KALOG_NO_PATH                             "(no log path resolved)"
#define KT_KALOG_ALL_APPS                            "(all apps)"
#define KT_KALOG_FP_REFUSED_NOTE                     "   (key refused: the file is not the key " \
        "recorded for it - fingerprint mismatch)"

/* kageant: the settings dialog (windows/pageant.c) */
#define KT_KASET_TAB_AGENT                           "Agent"
#define KT_KASET_TAB_SECURITY                        KT_SECURITY_SECURITY  /* "Security" */
#define KT_KASET_TAB_MEDIA                           "Removable media"
#define KT_KASET_TAB_LOG                             "Log"
#define KT_KASET_RETRY_NEVER                         KT_PROXYGUI_LOG_NEVER  /* "never" */
#define KT_KASET_RETRY_DRIVE                         "from their stored drive and path"
#define KT_KASET_RETRY_ANYDRIVE                      "from their stored path on any drive"
#define KT_KASET_AUTOENC_OFF                         KT_SCRIPTING_OFF  /* "Off" */
#define KT_KASET_AUTOENC_DEFAULT                     "Default for keys without their own setting"
#define KT_KASET_AUTOENC_ENFORCED                    "Enforced for every key"
#define KT_KASET_HELLO_UNAVAILABLE                   "Confirmations require Windows Hello " \
        "(not set up on this system)"
#define KT_KASET_LOG_DEFAULT_FMT                     "Default: %s"
#define KT_KASET_THEME_SYSTEM                        "Follow the system"
#define KT_KASET_THEME_LIGHT                         "Always light"
#define KT_KASET_THEME_DARK                          "Always dark"
#define KT_KASET_THEME_UNAVAILABLE                   "This Windows has no dark mode for desktop " \
        "windows; the light theme is the only one."

/* kageant: notices and prompts raised by the agent core (kitty/kitty_pageant.c) */
#define KT_KA_WHO_PROGRAM_FMT                        " a program (pid %lu)"
#define KT_KA_HELD_BACK_ONE_FMT                      "Something%s just asked for your keys, and one of them is NOT " \
        "loaded: the file is not the key recorded for it. If that login " \
        "fails, this is why. Click to see it."
#define KT_KA_HELD_BACK_MANY_FMT                     "Something%s just asked for your keys, and %d of them are NOT " \
        "loaded: the files are not the keys recorded for them. If a login " \
        "fails, this is why. Click to see them."
#define KT_KA_VERIFY_MISMATCH_ONE                    "A key file was NOT loaded: it is not the key recorded for " \
        "that path. Click to see which - its State in the key list " \
        "reads \"mismatch\"."
#define KT_KA_VERIFY_MISMATCH_MANY_FMT               "%d key files were NOT loaded: they are not the keys recorded " \
        "for those paths. Click to see which - their State in the key " \
        "list reads \"mismatch\"."
#define KT_KA_VERIFY_UNCHECKED_ONE                   "A key was loaded with no fingerprint on record, so nothing " \
        "could be checked. Its fingerprint is recorded now and it " \
        "will be checked from here on."
#define KT_KA_VERIFY_UNCHECKED_MANY_FMT              "%d keys were loaded with no fingerprint on record, so " \
        "nothing could be checked. Their fingerprints are recorded " \
        "now and they will be checked from here on."
#define KT_KA_VERIFY_NOFP_ONE                        "A file matching a startup key's path appeared on the new " \
        "drive, but that key has no fingerprint on record to check " \
        "it against, so it was NOT loaded. Load the key once from " \
        "its recorded path (or Add Key) to record one."
#define KT_KA_VERIFY_NOFP_MANY_FMT                   "%d files matching startup keys' paths appeared on the new " \
        "drive, but those keys have no fingerprints on record to " \
        "check them against, so they were NOT loaded. Load each key " \
        "once from its recorded path (or Add Key) to record one."
#define KT_KA_RETRY_NOTHING                          "No key is waiting to be loaded. Every " \
        "remembered key is either loaded already or " \
        "not in the startup list."
#define KT_KA_RETRY_LOADED_FMT                       "%d key%s loaded.%s "
#define KT_KA_RETRY_UNCHECKED_CLAUSE                 " There was no fingerprint on record for some of them," \
        " so nothing could be checked this once - what loaded" \
        " is the baseline from here on."
#define KT_KA_RETRY_REFUSED_FMT                      "%d refused: the file is not the key recorded for that " \
        "path. "
#define KT_KA_RETRY_ABSENT_FMT                       "%d still not there. "
#define KT_KA_RETRY_BROKEN_FMT                       "%d could not be read as a key. "
#define KT_KA_RETRY_CLICK                            "Click to see which."
#define KT_KA_PORTABLE_KEY_Q_FMT                     "This key is outside the portable install folder:\n\n" \
        "    %s\n\n" \
        "Copy it into the portable keys folder so it travels with this " \
        "install, or reference it where it is (it will then load only on " \
        "this machine)?%s\n\n" \
        "Yes = Copy into %s\\keys\n" \
        "No = Reference where it is\n" \
        "Cancel = Do not add it to the startup list"
#define KT_KA_PORTABLE_KEY_NOPASS_WARN               "\n\nWARNING: this key has no passphrase - copying it onto " \
        "portable media lets anyone holding the media use it."
#define KT_KA_PORTABLE_COPY_FAILED                   "Could not copy the key into the portable " \
        "folder; it will be referenced at its current location " \
        "instead."
#define KT_KA_AUTOSTART_RUN_DESC_FMT                 "%s  ->  %s\n(%s\\...\\CurrentVersion\\Run)"
#define KT_KA_AUTOSTART_SHORTCUT_DESC_FMT            "%s  ->  %s\n(%s Startup folder)"
#define KT_KA_AUTOSTART_ALL_USERS                    "all-users"
#define KT_KA_AUTOSTART_YOUR                         "your"
#define KT_KA_STARTUP_MISSING_RETRY_FMT              "%d startup key%s not reachable right now. They will be " \
        "loaded as soon as the drive they are on is back."
#define KT_KA_STARTUP_MISSING_FMT                    "%d startup key%s could not be found and %s skipped."
#define KT_KA_BY_PROGRAM_PID_FMT                     " by %s (pid %lu)"
#define KT_KA_BY_PID_FMT                             " by pid %lu"
#define KT_KA_ALL_KEYS_REMOVED_FMT                   "All keys were removed from the agent%s."
#define KT_KA_KEY_ADDED_TEXT                         "A key was added to the agent"
#define KT_KA_KEY_REMOVED_TEXT                       "A key was removed from the agent"
#define KT_KA_NO_COMMENT                             "(no comment)"
#define KT_KA_UNNAMED_KEY                            "(unnamed key)"
#define KT_KA_HELLO_ALLOW_USE_FMT                    "Allow this use of the SSH key \"%s\"?"
#define KT_KA_USE_DENIED_UNAVAILABLE                 "A key use was denied: it requires a Windows Hello check, " \
        "and Hello is not available in this session (no Hello " \
        "credential, policy, or a remote desktop). The request was " \
        "REFUSED - it is never downgraded to a plain click."
#define KT_KA_USE_DENIED_ERROR                       "A key use was denied: the Windows Hello check could not " \
        "be carried out."
#define KT_KA_CONFIRM_USE_FMT                        "A remote session is requesting to authenticate with the SSH key:" \
        "\n\n    %s\n\n" \
        "Yes - allow this one use.\n" \
        "No - deny this one use.\n" \
        "Cancel - deny this AND stop asking: all further requests are " \
        "denied silently until you open the kageant key list."
#define KT_KA_CONFIRM_BLOCKED_TEXT                   "Key-use confirmations are now being denied silently. " \
        "Click this notice, the tray \"Resume\" item, or the " \
        "key list's Resume button to allow them again."
#define KT_KA_KEY_USED_FMT                           "A key was used to authenticate:\n%s%s%s"

/* In-memory key protection missing: the lead sentence differs per app,
 * the reason clauses are shared (kitty_pageant.c, windows/puttygen.c) */
#define KT_KA_PROTKEY_WARN_FMT                       "Windows' CryptProtectMemory is not working in this " \
        "process, so private keys are held in PLAIN memory while " \
        "loaded. %s"
#define KT_KGEN_PROTKEY_WARN_FMT                     "Windows' CryptProtectMemory is not available on this " \
        "system, so generated and loaded private keys are held " \
        "in PLAIN memory while KiTTYgen runs. %s"
#define KT_PROTKEY_ABSENT_REASON                     "On this version of Windows the protection does not " \
        "exist."
#define KT_PROTKEY_HOOKED_REASON                     "On a normal Windows this never happens - something is " \
        "stripping or hooking the crypt API, which is itself " \
        "worth investigating."

/* Windows Hello: the shared printout window (kitty/kitty_hello_ui.c) */
#define KT_HELLO_UI_TITLE_FMT                        "%s - the key's %s"
#define KT_HELLO_UI_RECOVERY_CODE                    "recovery code"
#define KT_HELLO_UI_PRINTED_SECRET                   "printed secret"
#define KT_HELLO_UI_NOTE_CODE_FMT                    "This is the key's RECOVERY CODE. It is shown ONCE - %s does " \
        "not keep it.\r\n\r\n" \
        "It opens the key only TOGETHER with the .hello file, in KiTTY " \
        "tools. Keep the printout AND back up the .hello file: without " \
        "the file the code is worthless, and the key file's own " \
        "passphrase is written nowhere."
#define KT_HELLO_UI_NOTE_SECRET_FMT                  "This is the protected key's passphrase. It is shown ONCE - %s " \
        "does not keep it.\r\n\r\n" \
        "Print it or store it in a password manager. It opens the key " \
        "in any PuTTY-compatible tool, on any machine, with or without " \
        "Windows Hello, and it is the last resort if Windows Hello and " \
        "the recovery doors are all lost."
#define KT_HELLO_UI_NOT_STORED_Q                     "Have you stored the printout? It will " \
        "NEVER be shown again."
#define KT_HELLO_UI_NOT_STORED_CAP_FMT               "%s - printout not stored?"

/* Windows Hello: the terminal's anchor-card context line (kitty_hello_terminal.c) */
#define KT_HELLO_CONTEXT_LINE_FMT                    "%s  -  key %s"

/* Windows Hello: protect / enrol failure reasons, shown after
 * "The key was not protected:" and "Windows Hello was not added:"
 * (kitty/kitty_hello_keys.c) */
#define KT_HELLO_ERR_NEED_RECOVERY                   "a recovery passphrase is required: the key's " \
        "own passphrase is empty, so it cannot serve as " \
        "one"
#define KT_HELLO_ERR_EXISTS_FMT                      "%s already exists - not overwriting it"
#define KT_HELLO_ERR_UNAVAILABLE_HERE                "Windows Hello key protection is not available " \
        "on this machine"
#define KT_HELLO_ERR_LOAD_FMT                        "could not load %s: %s"
#define KT_HELLO_ERR_WRONG_PASSPHRASE                "wrong passphrase"
#define KT_HELLO_ERR_RANDOM                          "the system random generator failed"
#define KT_HELLO_ERR_OOM                             "out of memory"
#define KT_HELLO_ERR_CODE_DOOR                       "could not add the recovery-code door"
#define KT_HELLO_ERR_CANCELLED                       "the Windows Hello prompt was cancelled"
#define KT_HELLO_ERR_UNAVAILABLE                     "Windows Hello key protection is not available"
#define KT_HELLO_ERR_WRAP_FAILED                     "wrapping the secret failed"
#define KT_HELLO_ERR_WRITE_FMT                       "could not write %s"
#define KT_HELLO_ERR_NOT_SECRET                      "that is not this key's secret"
#define KT_HELLO_ERR_NO_SIDECAR                      "the key has no readable .hello sidecar"
#define KT_HELLO_ERR_SIDECAR_REWRITE                 "could not rewrite the .hello sidecar"
#define KT_HELLO_ERR_PASSKEY_UNAVAILABLE             "Windows Hello (passkey) protection is not " \
        "available on this machine"
#define KT_HELLO_ERR_ENROL_FAILED                    "adding this account's Hello door failed"

/* kittygen: captions and the window title (windows/puttygen.c) */
#define KT_CAP_KGEN_TITLE                            "KiTTY Key Generator"
#define KT_CAP_KGEN_NOTICE                           "KiTTYgen Notice"
#define KT_CAP_KGEN_CMDLINE_ERROR                    "KiTTYgen command line error"
#define KT_CAP_KGEN_SAVE_PARAMS_INVALID              "Save parameters invalid"
#define KT_CAP_KGEN_REMOVE_DOOR                      "KiTTYgen - remove a door"
#define KT_KGEN_NOTICE_UNPROTECTED                   "KiTTYgen: keys will not be memory-protected"

/* kittygen: menu items and main-window controls */
#define KT_KGEN_MENU_NEW                             "&New (clear)"
#define KT_KGEN_MENU_HELLO_DOORS                     "Windows Hello &doors..."
#define KT_KGEN_ADD_CONFIRMATION                     "Add confirmation"
#define KT_KGEN_CONFIRMATION_TIP                     "Tip: include the word \"confirmation\" in the " \
        "comment so kageant asks before each use."
#define KT_KGEN_HELLO_CHECKBOX                       "Protect with Windows &Hello " \
        "(passphrase = recovery passphrase)"
#define KT_KGEN_SIDEBOUND_CHECKBOX                   "Printout is a recover&y code bound to the " \
        ".hello file, not the key's passphrase"

/* kittygen: the passphrase prompt and the Windows Hello doors dialog */
#define KT_KGEN_PASS_PROMPT_HELLO                    "Enter the recovery passphrase, the recovery code, or the " \
        "printed secret:"
#define KT_KGEN_DOORS_NO_SIDECAR                     "(no readable .hello sidecar)"
#define KT_KGEN_DOORS_HELLO_FMT                      "Windows Hello: %s%s"
#define KT_KGEN_DOORS_UNTAGGED                       "(untagged)"
#define KT_KGEN_DOORS_THIS_COMPUTER                  "  - this computer"
#define KT_KGEN_DOORS_KCM                            "Windows Hello (KeyCredentialManager)"
#define KT_KGEN_DOORS_RECOVERY_CODE                  "Recovery code (printout, bound to this file)"
#define KT_KGEN_DOORS_RECOVERY_PASS                  "Recovery passphrase"
#define KT_KGEN_DOORS_PRINTED                        "Printed secret (the key's passphrase itself)"
#define KT_KGEN_DOORS_ONLY_HELLO                     "Only a machine's Windows Hello entry can " \
        "be removed here. The recovery passphrase is the " \
        "safety net, and the printed secret is the file's " \
        "own passphrase."
#define KT_KGEN_DOORS_REMOVE_Q                       "Remove this Windows Hello entry? The " \
        "machine it belongs to can then open the key " \
        "only with the recovery passphrase or the " \
        "printed secret."
#define KT_KGEN_DOORS_REWRITE_FAILED                 "Could not rewrite the .hello sidecar."
#define KT_KGEN_DOORS_LAST_DOOR                      "Refused: a sidecar never loses its last door. " \
        "Disarm the key instead (save it without " \
        "protection)."
#define KT_KGEN_DOORS_ALREADY                        "This computer is already enrolled."
#define KT_KGEN_DOORS_NO_DOOR                        "That opened no door - it is neither the " \
        "recovery passphrase nor the printed secret."
#define KT_KGEN_HELLO_NOT_ADDED_FMT                  "Windows Hello was not added:\n\n%s"
#define KT_KGEN_LOAD_HELLO_FIRST                     "Load a Windows Hello protected key first " \
        "(a .ppk with a .hello file beside it)."

/* kittygen: saving a key */
#define KT_KGEN_DECRYPT_FAILED                       "Unable to decrypt the in-memory private key"
#define KT_KGEN_HELLO_NEEDS_PPK                      "Windows Hello protection needs the " \
        "PuTTY PPK format (SSH-2). Save or export " \
        "without it, or untick the box."
#define KT_KGEN_RANDOM_FAILED                        "The system random generator " \
        "failed."
#define KT_KGEN_HELLO_CANCELLED_NOT_SAVED            "The Windows Hello prompt was " \
        "cancelled - the key was NOT saved."
#define KT_KGEN_HELLO_FAILED_NOT_SAVED               "Windows Hello protection failed - " \
        "the key was NOT saved."
#define KT_KGEN_PPK_PARAMS_INVALID                   "PPK parameters invalid: "
#define KT_KGEN_SIDECAR_WRITE_FAILED                 "Could not write the .hello " \
        "sidecar; the key file was removed."
#define KT_KGEN_STILL_PROTECTED_FMT                  "Note: this key is still Windows Hello " \
        "protected at\n\n    %s\n\nThe agent keeps " \
        "asking Windows Hello for that file (its " \
        ".hello sidecar still exists). Delete it, " \
        "or save over it without protection, to " \
        "disarm it there too."

/* ---- Batch 4: terminal window, command line, terminal core, dialogs ---- */

/* windows/window.c: notices, confirmations and Event Log lines */
#define KT_TWIN_OLDSESS_TITLE                        "KiTTY is also showing your old sessions"
#define KT_TWIN_OLDSESS_TEXT                         "This KiTTY had no saved sessions of its own, so its session " \
        "list also holds the ones an older KiTTY or PuTTY left in the " \
        "registry - they can be opened, edited and deleted from it. " \
        "That answer is now recorded and will not change on its own. " \
        "Click here to see it, or to switch the old sessions off."
#define KT_CAP_CONNECTION_FAILED                     "Connection failed"
#define KT_TWIN_WORKPLACE_FAILED_Q                   "%s\n\n" \
        "Workplace proxy mode is on, so this connection was made " \
        "through the proxy \"%s\" rather than through this session's " \
        "own settings.\n\n" \
        "Switch workplace proxy mode off? Connections would then use " \
        "each session's own proxy settings again."
#define KT_TWIN_WORKPLACE_NOTICE_TITLE               "Workplace proxy mode is on"
#define KT_TWIN_WORKPLACE_NOTICE_TEXT                "This connection went through \"%s\" because workplace proxy " \
        "mode is on, and it did not come up. If you have left the place " \
        "that proxy belongs to, click here to switch the mode off."
#define KT_TWIN_CLOSED_BY_HOST_INLINE                "\r\n\x1b[1;33m%s:\x1b[0m Connection closed by remote host\r\n"
#define KT_CAP_BLOCK_CLIP_WRITES                     "KiTTY - block this server's clipboard writes?"
#define KT_TWIN_BLOCK_CLIP_WRITES_Q                  "A server has been changing your clipboard " \
        "repeatedly.\n\n" \
        "Stop it changing your clipboard at all for the " \
        "rest of this session?\n\n" \
        "You can turn it back on under " \
        "Window > Selection, \"Remote clipboard writes\"."
#define KT_TWIN_SCRIPT_DISABLED                      "RuTTY scripting is disabled" \
        " ([KiTTY] scriptmode=no in kitty.ini)."
#define KT_TWIN_SCRIPT_FILE_TITLE                    "Send script file..."
#define KT_TWIN_SCRIPT_FILE_FILTER                   "Script files (*.ksh,*.sh)|*.ksh;*.sh|All files (*.*)|*.*|"
#define KT_CAP_PASTE                                 "KiTTY paste"
#define KT_TWIN_PASTE_LIMIT_Q                        "The clipboard holds %lu characters, more than" \
        " the configured pastesize limit of %d.\n\n" \
        "Paste it anyway?"
/* Event Log lines (window.c) */
#define KT_TWIN_LOG_CONNECT_FAILED_RECONNECT         "Unable to connect, trying to reconnect..."
#define KT_TWIN_LOG_LOST_RECONNECT                   "Lost connection, trying to reconnect..."
#define KT_TWIN_LOG_NO_BACKEND_RECONNECT             "No backend connection, reconnecting..."
#define KT_TWIN_LOG_WAKEUP_RECONNECT                 "Woken up from suspend, trying to reconnect..."
#define KT_TWIN_LOG_SUSPEND_DISCONNECT               "Suspend detected, disconnecting cleanly..."
#define KT_TWIN_LOG_KEY_RECONNECT                    "No connection on key pressed, trying to reconnect..."
#define KT_TWIN_LOG_SESSION_RESTARTED                "----- Session restarted -----"
#define KT_TWIN_LOG_ENDED_LOG_OPEN                   "Session ended; window kept open while " \
        "the Event Log is open (it closes when you close the " \
        "log)"
#define KT_TWIN_LOG_CLOSED_LOG_OPEN                  "Connection closed; window kept open while the " \
        "Event Log is open (it closes when you close the log)"
#define KT_TWIN_LOG_RUTTY_WAITING                    "Rutty script (Session > Scripting) is waiting for " \
        "the login script (Connection > Data) to finish"
#define KT_TWIN_LOG_RUTTY_STARTING                   "Login script (Connection > Data) has not finished; starting " \
        "the rutty script (Session > Scripting) anyway - if the " \
        "automation misbehaves, that is why"
#define KT_TWIN_LOG_SCRIPT_STOPPED                   "script stopped"
#define KT_TWIN_LOG_BC_REFUSED_NOGROUP               "broadcast refused: sender did not identify " \
        "its group (pre-0.85 format)"
#define KT_TWIN_LOG_BC_REFUSED_DISABLED              "broadcast refused: disabled on this " \
        "install ([KiTTY] sendcmdmode=no in kitty.ini)"
#define KT_TWIN_LOG_BC_REFUSED_OTHER_INSTALL         "broadcast refused: from another KiTTY " \
        "install (group '%s', ours is '%s')"
#define KT_TWIN_LOG_BC_REFUSED_SESSION               "broadcast refused: this session does not " \
        "accept broadcasts (Session > Scripting, or the " \
        "Tools > Accept broadcast toggle)"
#define KT_TWIN_LOG_BC_ACCEPTED                      "broadcast accepted (%d bytes), typing it " \
        "into this session"
#define KT_TWIN_LOG_BC_WINDOW_ON                     "broadcasts accepted for this window"
#define KT_TWIN_LOG_BC_WINDOW_OFF                    "broadcasts refused for this window"
#define KT_TWIN_LOG_CLIP_WRITES_BLOCKED              "Remote clipboard writes blocked for " \
        "this session at the user's request"

/* Terminal window title: state suffixes and markers (window.c, dialog.c) */
#define KT_TITLE_DISCONNECTED                        "\xe2\x9a\xa0 %s (disconnected)"
#define KT_TITLE_INACTIVE                            "%s (inactive)"
#define KT_TITLE_SIZE                                " [%dx%d]"
#define KT_TITLE_PROTECTED                           " (PROTECTED)"
#define KT_TITLE_ONTOP                               " (ONTOP)"
#define KT_TITLE_RESTRICTED                          " (RESTRICTED)"
#define KT_TITLE_WORKPLACE_LEAD                      L"⇄ workplace proxy"
#define KT_TITLE_QUICK_CONNECT                       "%s - quick connect"
#define KT_TITLE_QUICK_CONNECT_STAMPED               "%.*s - quick connect%s"

/* The KiTTY system menu (window.c) */
#define KT_SYSMENU_INHERIT_NEW_SESSION               "&Inherit New Session..."
#define KT_SYSMENU_CLOSE_RESTART                     "Close+&Restart"
#define KT_SYSMENU_TRANSPARENCY_UP                   "Transparency &+"
#define KT_SYSMENU_TRANSPARENCY_DOWN                 "Transparency &-"
#define KT_SYSMENU_FONT_UP                           "Font &Up"
#define KT_SYSMENU_FONT_DOWN                         "Font &Down"
#define KT_SYSMENU_INVERT_COLOURS                    "Invert co&lours"
#define KT_SYSMENU_BLACK_ON_WHITE                    "&Black on white"
#define KT_SYSMENU_ALWAYS_ON_TOP                     "Always On &Top"
#define KT_SYSMENU_ROLLUP                            "Roll-u&p"
#define KT_SYSMENU_SEND_TO_TRAY                      "Send to tra&y"
#define KT_SYSMENU_PROTECT                           "Prote&ct"
#define KT_SYSMENU_WINDOW                            "&Window"
#define KT_SYSMENU_PORT_FORWARDINGS                  "Port forwar&dings"
#define KT_SYSMENU_START_WINSCP                      "Start Win&SCP"
#define KT_SYSMENU_SEND_FILE_PSCP                    "Send file (&pscp)"
#define KT_SYSMENU_OPEN_MNOTEPAD                     "Open &mNotepad"
#define KT_SYSMENU_OPEN_MNOTEPAD_CLIP                "Open mNotepad with clip&board"
#define KT_SYSMENU_SCRIPT_SEND                       "Send &recorded script"
#define KT_SYSMENU_SCRIPT_STOP                       "S&top script"
#define KT_SYSMENU_SCRIPT_FILE                       "Send scr&ipt file"
#define KT_SYSMENU_ZMODEM_RECEIVE                    "&ZModem Receive"
#define KT_SYSMENU_ZMODEM_UPLOAD                     "ZModem &Upload"
#define KT_SYSMENU_ZMODEM_ABORT                      "ZModem &Abort"
#define KT_SYSMENU_PRINT_CLIPBOARD                   "Print clip&board"
#define KT_SYSMENU_OPEN_LOG_FILE                     "&Open log file"
#define KT_SYSMENU_CLEAR_LOG_FILE                    "Clear log fil&e"
#define KT_SYSMENU_NEW_LOG_FILE                      "Start a new log file &now"
#define KT_SYSMENU_EXPORT_SETTINGS                   "Export &current settings"
#define KT_SYSMENU_SHORTCUTS                         "Shortcut&s"
#define KT_SYSMENU_HYPERLINKS                        "Hyper&links"
#define KT_SYSMENU_ACCEPT_BROADCAST                  "Accept &broadcast (Session > Scripting)"
#define KT_SYSMENU_TOOLS                             "&Tools"
#define KT_SYSMENU_CHECK_UPDATES                     "Check for &updates..."
#define KT_SYSMENU_NO_SESSIONS                       "(No sessions)"

/* windows/putty.c: command-line diagnostics and the do-and-exit switches */
#define KT_CLI_BAD_ARG                               "bad argument \"%s\" to option \"%s\""
#define KT_CLI_OPTION_NEEDS_DIR                      "option \"%s\" requires a directory argument"
#define KT_CLI_OPTION_NEEDS_FILE                     "option \"%s\" requires a file argument"
#define KT_CLI_CONFMAP_INVALID                       "Serialised configuration data " \
        "was invalid"
#define KT_CLI_BUNDLEPW_OPEN_FAILED                  "unable to open bundle-password file '%s'"
#define KT_CLI_BUNDLEPW_READ_FAILED                  "unable to read a password from file '%s'"
#define KT_CLI_EDIT_FILE_NOT_FOUND                   "Unable to find requested file"
#define KT_CAP_CLI                                   "KiTTY command line"
#define KT_CLI_HELP_FMT                              "%s %s\r\n" \
        "\r\nUsage: kitty.exe [options] [user@]host[:port]" \
        "\r\n       kitty.exe [options] -load <saved session>" \
        "\r\n       kitty.exe ssh://[user@]host[:port]" \
        "\r\n       kitty.exe kitty://<saved session>" \
        "\r\n%s"
#define KT_CLI_EXPORT_NEEDS_PROTECTION               "Say how the exported sessions should be protected:\n\n" \
        "  -bundlepwfile <file>   password (first line of the file);\n" \
        "                         the bundle then imports on any PC\n" \
        "  -bundlethispc          no password; the bundle imports only\n" \
        "                         with this Windows account on this PC\n\n" \
        "Nothing was exported."
#define KT_CLI_EXPORT_DONE                           "Exported %d session(s), %d failed, to:\n%s%s"
#define KT_CLI_EXPORT_THISPC_NOTE                    "\n\nThese sessions can only be imported with this Windows " \
        "account on this PC."
#define KT_CLI_IMPORT_NEEDS_PASSWORD                 "These exported sessions are password-protected. Supply the " \
        "import password with:\n\n" \
        "  -bundlepwfile <file>   (the password on the first line)\n\n" \
        "Nothing was imported."
#define KT_CLI_IMPORT_DONE                           "Imported %d session(s), %d prox(ies), %d failed, from:\n%s"
#define KT_CAP_BACKUP                                "KiTTY backup"
#define KT_CLI_BACKUP_DONE                           "Backup written beside:\n%s"
#define KT_CLI_BACKUP_NO_TARGET                      "(no backup target)"
#define KT_CAP_PORTABLE_COPY                         KT_INIMIG_OUT_GROUP  /* the panel's group box carries the same words */
#define KT_CLI_PORTABLECOPY_NEEDS_PROTECTION         "Say how the copy's passwords should be protected:\n\n" \
        "  -bundlepwfile <file>   its master password (first line of " \
        "the file)\n" \
        "  -bundlethispc          no password; readable by this Windows " \
        "account on this PC only\n\n" \
        "Nothing was copied."
#define KT_CAP_TAKE_FOLDER                           KT_INIMIG_IN_GROUP   /* likewise */
#define KT_CLI_TAKEFOLDER_NEEDS_PASSWORD             "This folder store has a master password. Supply it with:\n\n" \
        "  -bundlepwfile <file>   (the password on the first line)\n\n" \
        "Nothing was taken."

/* cmdline.c: -masterpwfile */
#define KT_CLI_MPWFILE_CONFLICT                      "-masterpwfile conflicts with PortablePasswordProtection=dpapi" \
        " in kitty.ini: that store protects passwords with Windows" \
        " DPAPI and has no master password. Remove one of the two."
#define KT_CLI_MPWFILE_OPEN_FAILED                   "unable to open master-password file '%s'"
#define KT_CLI_MPWFILE_READ_FAILED                   "unable to read a master password from file '%s'"

/* logging.c: Event Log lines */
#define KT_LOG_ROTATION_SKIPPED                      "Log rotation skipped: the log file name does not " \
        "change with time (use &T, or &Y&M&D, in it) - rotating " \
        "into the same name would overwrite the log"
#define KT_LOG_TIMESTAMP_EMPTY                       "Log timestamp: that strftime pattern produces nothing, " \
        "so lines are not being stamped - check the format in " \
        "Session > Logging"

/* terminal/terminal.c: remote clipboard (OSC 52, OSC 5522, far2l) */
#define KT_CLIP_WHAT_FAR2L                           "A far2l clipboard payload"
#define KT_CLIP_WHAT_OSC52                           "A remote clipboard write (OSC 52)"
#define KT_CLIP_WHAT_OSC5522                         "A clipboard request (OSC 5522)"
#define KT_CLIP_FAR2L_ALLOW_Q                        "Allow far2l clipboard sync?"
#define KT_CLIP_WRITE_ALLOW_Q                        "The server wants to put text on your clipboard.\n\n" \
        "Allow it for the rest of this session?"
/* balloon notices */
#define KT_CLIP_NOTICE_TOO_MUCH                      "Too much data arrived for the clipboard in one go, so it " \
        "was not copied. Nothing was pasted in part.\n" \
        "Click here for the Event Log, which lists every one of " \
        "these (this notice is rate-limited)."
#define KT_CLIP_NOTICE_READ_NOTITLE                  "A server has read your clipboard.\n" \
        "Shown here because this window has no title bar to mark."
#define KT_CLIP_NOTICE_WRITE_NOTITLE                 "A server has changed your clipboard.\n" \
        "Shown here because this window has no title bar to mark."
#define KT_CLIP_NOTICE_WRITE_REPEATED                "A server is repeatedly changing your clipboard. The extra " \
        "changes are being ignored.\n" \
        "Click here to stop this server changing it at all."
#define KT_CLIP_NOTICE_READ_EXPIRED                  "Permission to read the clipboard has expired."
#define KT_CLIP_NOTICE_READ_REFUSED                  "A server asked to read your clipboard. " \
        "It was refused.\n" \
        "Click here for the Event Log (this notice is " \
        "rate-limited)."
#define KT_CLIP_NOTICE_READ_GRANTED                  "This server may now read your clipboard. " \
        "The title bar shows it while that lasts."
/* Event Log lines */
#define KT_CLIP_LOG_DROPPED_MORE                     "%s was too large and was dropped; %d further " \
        "payload%s also dropped"
#define KT_CLIP_LOG_DROPPED                          "%s was too large and was dropped"
#define KT_CLIP_LOG_WRITE_RATE_MORE                  "Remote clipboard write ignored: more than the " \
        "permitted number in one second; %d further write%s " \
        "also ignored"
#define KT_CLIP_LOG_WRITE_RATE                       "Remote clipboard write ignored: more than the " \
        "permitted number in one second"
#define KT_CLIP_LOG_WRITE_EMPTY                      "Remote clipboard write ignored: empty payload"
#define KT_CLIP_LOG_WRITE_NO_FOCUS                   "Remote clipboard write ignored: " \
        "the window does not have focus"
#define KT_CLIP_LOG_READ_REFUSED_MORE                "Clipboard read refused (%s); %d further " \
        "request%s also refused"
#define KT_CLIP_LOG_READ_REFUSED                     "Clipboard read refused (%s)"
#define KT_CLIP_LOG_READ_SENT                        "Clipboard sent to the server on request " \
        "(%d character%s; %d read%s served in this window)"
#define KT_CLIP_LOG_READ_SENT_5522                   "Clipboard sent to the server on request over OSC 5522 " \
        "(%d character%s%s%s; %d read%s served in this window)"
#define KT_CLIP_LOG_PROGRAM_CALLS_ITSELF             ", program calls itself "
#define KT_CLIP_LOG_APPROVAL_WITHDRAWN               "Clipboard approval withdrawn: the limit on " \
        "reads served in this window was reached"
#define KT_CLIP_LOG_PERMISSION_WITHDRAWN             "Clipboard permission withdrawn: the limit on " \
        "reads served in this window was reached"
#define KT_CLIP_LOG_DENY_SAVED                       "Clipboard reads set to Deny for this " \
        "host, and saved in the session"
#define KT_CLIP_LOG_DENY_UNSAVED                     "Clipboard reads refused for the rest of " \
        "this session; no saved session to store it in"
/* the "(why)" of a refused read, as logged */
#define KT_CLIP_WHY_DENY_SETTING                     "reads are set to Deny"
#define KT_CLIP_WHY_NO_FOCUS                         "window not focused"
#define KT_CLIP_WHY_REFUSED_EARLIER                  "refused earlier, and that still applies"
#define KT_CLIP_WHY_PROGRAM_TOO_SOON                 "approved program asked again sooner than " \
        "the minimum gap allows"
#define KT_CLIP_WHY_PROGRAM_LIMIT                    "approved program reached the read limit"
#define KT_CLIP_WHY_TOO_SOON                         "asked again sooner than the minimum gap " \
        "allows"
#define KT_CLIP_WHY_DIALOG_OPEN                      "a clipboard dialog is already open"
#define KT_CLIP_WHY_ASKING_REPEATEDLY                "asking repeatedly; not prompting again yet"
#define KT_CLIP_WHY_UNAVAILABLE                      "the clipboard was not available (another " \
        "program has it open); not asking this time"
#define KT_CLIP_WHY_USER_NO                          "the user said no"

/* windows/dialog.c: the configuration box, the About box, inline confirmations */
#define KT_DLG_UNNAMED_SETTING                       "(unnamed setting)"
#define KT_DLG_INVALID_SETTINGS                      "Session \"%s\" contains %d setting%s this version cannot " \
        "represent:\n\n%s%s\n\nThe default%s been used instead. Saving " \
        "the session will store the corrected value%s."
#define KT_DLG_LOADED_SESSION                        "Currently loaded session: %s"
#define KT_DLG_QUICK_CONNECT_ACTIVE                  "Quick Connect Mode active"
#define KT_DLG_TAB_SESSION                           "Session"
#define KT_DLG_TAB_APPLICATION                       "Application"
/* About box. KT_DLG_ABOUT_TESTBUILD expands KITTY_TEST_BUILD_LABEL (a build
 * define, used only under #ifdef KITTY_TEST_BUILD_LABEL) and KT_DLG_ABOUT_PUTTY
 * expands SHORT_COPYRIGHT_DETAILS (version.h): both are resolved where the
 * macro is used, not here. */
#define KT_DLG_ABOUT_NETDEBUG                        "\r\n*** NETDEBUG BUILD - event log is teed to " \
        "%USERPROFILE%\\kitty_netdebug.log ***"
#define KT_DLG_ABOUT_TESTBUILD                       "\r\n*** TEST BUILD: " KITTY_TEST_BUILD_LABEL " ***"
#define KT_DLG_ABOUT_RESTRICTED                      "\r\n\r\nRunning with a restricted process ACL: other programs " \
        "under your account cannot open this process."
#define KT_DLG_ABOUT_FMT                             "%s\r\n\r\n%s%s%s%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s\r\n\r\n%s" \
        "\r\n\r\n%s"
#define KT_DLG_ABOUT_PORT                            "This PuTTY 0.85 port \xc2\xa9 KAPPER NETWORK-COMMUNICATIONS GmbH " \
        "\xe2\x80\x94 https://github.com/hknet/KiTTY"
#define KT_DLG_ABOUT_KITTY                           "KiTTY \xc2\xa9 2007-2013 Cyril Dupont \xe2\x80\x94 https://www.9bis.net/kitty/"
#define KT_DLG_ABOUT_PUTTY                           "Based on PuTTY \xc2\xa9 " SHORT_COPYRIGHT_DETAILS ". All rights reserved."
#define KT_DLG_ABOUT_FAR2L                           "far2l terminal extensions from putty4far2l " \
        "(Ivan Sorokin, unxed, Ivan Shatsky); far2l \xe2\x80\x94 elfmz."
#define KT_DLG_ABOUT_RUTTY                           "Session scripting from the RuTTY patch \xc2\xa9 2013-2014 " \
        "Ernst Dijk."
/* Inline (typed) SSH confirmations, written into the terminal */
#define KT_DLG_CONNECT_ONCE                          "Connecting once; the key was not cached.\r\n"
#define KT_DLG_ABANDONED                             "Connection abandoned.\r\n"
#define KT_DLG_ABANDONED_NL                          "\r\nConnection abandoned.\r\n"
#define KT_DLG_KEY_REPLACED                          "\r\nStored host key replaced. Continuing.\r\n"
#define KT_DLG_KEY_KEPT_ONCE                         "\nKeeping the previously stored key; continuing this once " \
        "(you will be asked again next time).\n"
#define KT_DLG_HOSTKEY_LIVE_ABANDONED                "Host key confirmation cannot be shown during an active " \
        "session. Connection abandoned.\r\n"
#define KT_DLG_HOSTKEY_NEW_PROMPT                    "Are you sure you want to continue connecting " \
        "(type \"yes\" to accept and cache the key, \"once\" to connect " \
        "without caching, anything else to cancel)? "
#define KT_DLG_WEAKALG_LIVE_ABANDONED                "Weak-algorithm confirmation cannot be shown during an active " \
        "session. Connection abandoned.\r\n"
#define KT_DLG_WEAKKEY_LIVE_ABANDONED                "Weak-key confirmation cannot be shown during an active " \
        "session. Connection abandoned.\r\n"
#define KT_DLG_WEAK_ACCEPT_PROMPT                    "To accept the risk and continue, type \"yes\" " \
        "(anything else cancels): "
/* Changed host key, the two-step flow. KCH_HL / KCH_RST are the bold-red
 * SGR escapes dialog.c defines just above its use of these; they expand there. */
#define KT_DLG_CHANGED_ACK_INTRO                     "\nThe host key for this server has " KCH_HL "CHANGED" KCH_RST " since it was " \
        "last cached. This can mean the server was legitimately rebuilt - or that the " \
        "connection is being intercepted (a man-in-the-middle attack).\n"
#define KT_DLG_CHANGED_ACK_PROMPT                    "Type \"yes\" to accept the new key for THIS connection, or anything else " \
        "to abandon: "
#define KT_DLG_CHANGED_REPLACE_INTRO                 "\nReplace the stored host key with this new one for future connections?\n" \
        KCH_HL "SECURITY WARNING" KCH_RST ": KiTTY cannot confirm that this new key " \
        "genuinely belongs to the server. A plain SSH host key is trusted on first " \
        "use, with no authority to verify it against. Replace the stored key ONLY if " \
        "you are certain, by some independent means (e.g. a fingerprint obtained " \
        "out-of-band), that the new key is genuine.\n"
#define KT_DLG_CHANGED_REPLACE_PROMPT                "Type \"confirmed\" to replace the stored key, \"no\" or Enter to keep the " \
        "old key, or Ctrl-C to abandon: "
#define KT_DLG_CHANGED_RETRY_INTRO                   "\nPlease answer with a whole word: \"confirmed\" to replace the stored key, " \
        "or \"no\" (or Enter) to keep the old key and connect once. A plain \"yes\" " \
        "is intentionally not enough to replace a changed key.\n"

/* ---- Batch 2: configuration box, settings, store moves ---- */

/* Shared by the Proxy panel (kitty_config.c) and Named Proxies (kitty_proxy_gui.c) */
#define KT_PROXY_TYPE_SOCKS4                         "SOCKS 4"
#define KT_PROXY_TYPE_SOCKS5                         "SOCKS 5"

/* kitty_config.c: Session panel */
#define KT_SESSION_PROXY_LABEL_IDLE                  KT_SESSION_PROXY_OVERRIDE_OPTIONS  /* the droplist's caption while no override is armed */
#define KT_CFG_SESSION_TITLE_FMT                     "Basic options for your %s session"
#define KT_CFG_SESSION_SAVE_CURRENT                  "Save the current session settings"
#define KT_CFG_SESSION_LOAD_SAVE_DELETE              "Load, save or delete a stored session"
#define KT_CFG_BTN_APPLY                             "Apply"
#define KT_CFG_BTN_OPEN                              "Open"
#define KT_CFG_PROTO_OTHER                           "Other:"
#define KT_CFG_SERIAL_LINE                           "Serial line"
#define KT_CFG_SPEED                                 "Speed"
#define KT_CFG_RENAME                                "Rename"
#define KT_CFG_COMMENT_SELECT                        "Select a session to see its comment"
#define KT_CFG_COMMENT_NONE                          "(no comment stored for this session)"
#define KT_CFG_ROOT_FOLDER_CANT_DELETE               "root folder can't be deleted"
#define KT_CFG_SESSION_UPDATE_FAILED                 "Could not update session \"%s\":\n%s"
#define KT_CFG_FOLDER_NAME_RESERVED_ROOT             "That name is reserved for the root session list."
#define KT_CFG_FOLDER_NAME_RESERVED                  "That name is reserved."
#define KT_CFG_FOLDER_EXISTS                         "A folder of that name already exists."
#define KT_CFG_FOLDER_DELETE_ONE                     "\"%s\" contains one session.\n\n" \
        "Delete the folder and move the session to the root list?\n" \
        "The session itself is kept."
#define KT_CFG_FOLDER_DELETE_MANY                    "\"%s\" contains %d sessions.\n\n" \
        "Delete the folder and move the sessions to the root list?\n" \
        "The sessions themselves are kept."
#define KT_CFG_DEFAULT_CANT_DELETE_NOSAVE            "\"%s\" cannot be deleted - it is the template every new " \
        "session starts from.\n\n" \
        "It can normally be hidden from this list, but %s, so that " \
        "setting cannot be saved right now."
#define KT_CFG_DEFAULT_READONLY                      "KiTTY is running read-only"
#define KT_CFG_DEFAULT_NO_CONF                       "this KiTTY is running without a " \
        "configuration file"
#define KT_CFG_DEFAULT_HIDE_Q                        "\"%s\" cannot be deleted - it is the template every new session " \
        "starts from.\n\n" \
        "It can be hidden from this list instead. The template itself " \
        "keeps working; it simply stops taking up a row.\n\n" \
        "Note it is also the way into quick connect - loading it once " \
        "puts the caret in Host Name so you can type an address instead " \
        "of picking a session. With the row hidden you would reach that " \
        "by setting [ConfigBox] loadlastsession=no instead.\n\n" \
        "Hide it?\n\n" \
        "To show it again later you have to edit the configuration file " \
        "by hand and set:\n" \
        "    [ConfigBox]\n" \
        "    defaultsettings=yes\n\n" \
        "Configuration file:\n%s"
#define KT_CFG_DEFAULT_WRITE_FAILED                  "Could not write to the configuration file:\n%s\n\n" \
        "\"%s\" is still shown."
#define KT_CAP_OVERWRITE_SESSION                     "Overwrite saved session?"
#define KT_CFG_OVERWRITE_HIGHLIGHT_Q                 "Replace the saved session \"%s\"?\n\n" \
        "The name box is empty, so the highlighted entry in " \
        "the session list is the target. Its settings are " \
        "about to be overwritten with the ones currently in " \
        "this dialog - host name, port, protocol and " \
        "everything else.\n\n" \
        "Type a name in the box to save under a different " \
        "one."
#define KT_CFG_OVERWRITE_UNLOADED_Q                  "Replace the saved session \"%s\"?\n\n" \
        "You did not load it, so its settings are about to " \
        "be overwritten with the ones currently in this " \
        "dialog - host name, port, protocol and everything " \
        "else.\n\n" \
        "Clicking a name in the session list only fills in " \
        "the name; it does not load that session. Use Load " \
        "first if you meant to edit it."

/* kitty_config.c: Session/Startup - the launcher hotkey */
#define KT_CFG_HOTKEY_ENTER                          "Enter a hotkey such as Ctrl+Alt+K or Ctrl+Shift+F12."
#define KT_CFG_HOTKEY_FREE_BUT_SAVED                 "This hotkey is currently available system-wide, but it is " \
        "already assigned to the saved session%s: %s.\n\n" \
        "A hotkey works for only one session; the launcher gives " \
        "it to the first one it finds."
#define KT_CFG_HOTKEY_FREE                           "This hotkey is currently available.\n\nNote: it is only registered while KiTTY Launcher is running."
#define KT_CFG_HOTKEY_IN_USE_SAVED                   "This hotkey is already in use - it is assigned to the saved " \
        "session%s: %s.\n\n" \
        "A hotkey works for only one session; the launcher gives it " \
        "to the first one it finds."
#define KT_CFG_HOTKEY_IN_USE_SYSTEM                  "This hotkey is already in use or reserved by Windows/another app.\n\nWindows does not expose which application owns a global hotkey."
#define KT_CFG_HOTKEY_SLOTS_FULL                     "All %d launcher hotkey slots are already in " \
        "use, so the hotkey of this session has been " \
        "switched off.\n\nDisable another session's " \
        "hotkey first, then enable this one again."
#define KT_CFG_HOTKEY_ALSO_ASSIGNED                  "The hotkey \"%s\" is also assigned to: %s.\n\n" \
        "A hotkey works for only one session; the " \
        "launcher gives it to the first one it finds. " \
        "Edit the others to resolve this."

/* kitty_config.c: Session/Logging, Scripting, Broadcast */
#define KT_CFG_LOG_CLEAR_TIMESTAMP                   "Clear the timestamp"
#define KT_CFG_LOG_SSH_PACKETS                       "SSH packets"
#define KT_CFG_LOG_SSH_PACKETS_RAW                   "SSH packets and raw data"
#define KT_CFG_SCRIPT_NOLF                           "no LF"
#define KT_CFG_SCRIPT_CR                             "CR"
#define KT_CFG_SCRIPT_REC                            "Rec"
#define KT_CFG_BROADCAST_KEY_CUSTOM                  "Custom key for this session - Clear restores the default."
#define KT_CFG_BROADCAST_KEY_INI                     "Default key, set as sendcmdgroup in your kitty.ini file."

/* kitty_config.c: panel titles built with the application name */
#define KT_CFG_WINDOW_TITLE_FMT                      "Options controlling %s's window"
#define KT_CFG_APPEARANCE_TITLE_FMT                  "Configure the appearance of %s's window"
#define KT_CFG_BEHAVIOUR_TITLE_FMT                   "Configure the behaviour of %s's window"
#define KT_CFG_LINEDRAW_TITLE_FMT                    "Adjust how %s handles line drawing characters"
#define KT_CFG_PRECISE_COLOURS_TITLE_FMT             "Adjust the precise colours %s displays"

/* kitty_config.c: Window/Hyperlinks, Window/Selection */
#define KT_CAP_RESET_URL_REGEX                       "Reset the URL regular expression?"
#define KT_CFG_RESET_URL_REGEX_Q                     "The custom regular expression is REPLACED by " \
        "KiTTY's default pattern.\n\n" \
        "Whatever the field holds now is lost."
#define KT_CFG_AUTOCOPY_SELECTED_TEXT_TO             "Auto-copy selected text to "   /* the clipboard name follows */
#define KT_CFG_MOUSE_PASTE_ACTION                    "Mouse paste action:"
#define KT_CFG_CTRL_SHIFT_INS                        "{Ctrl,Shift} + Ins:"
#define KT_CFG_CTRL_SHIFT_CV                         "Ctrl + Shift + {C,V}:"
#define KT_CFG_CLIP_NO_ACTION                        "No action"

/* kitty_config.c: Connection, Connection/Data */
#define KT_CFG_LOGHOST_SSH                           "Logical name of remote host (e.g. for SSH key lookup):"
#define KT_CFG_LOGHOST                               "Logical name of remote host:"
#define KT_CFG_USE_SYSTEM_USERNAME                   "Use system username (%s)"

/* kitty_config.c: Connection/Proxy - the session's own proxy and the pre-set loader */
#define KT_CFG_PROXY_TYPE_HTTP_CONNECT               "HTTP CONNECT"
#define KT_CFG_PROXY_TYPE_SSH_TCPIP                  "SSH to proxy and use port forwarding"
#define KT_CFG_PROXY_TYPE_SSH_EXEC                   "SSH to proxy and execute a command"
#define KT_CFG_PROXY_TYPE_SSH_SUBSYSTEM              "SSH to proxy and invoke a subsystem"
#define KT_CFG_PROXY_TYPE_LOCAL                      "Local (run a subprogram to connect)"
#define KT_CFG_PROXY_TYPE_TELNET                     "'Telnet' (send an ad-hoc command)"
#define KT_CAP_LOAD_NAMED_PROXY                      "Load named proxy settings?"
#define KT_CFG_PXLOAD_Q                              "Load the named proxy \"%s\" into this configuration window?\n\n" \
        "It REPLACES this session's own proxy settings - type, host, port, " \
        "exclude list, DNS setting, and the proxy USERNAME AND PASSWORD. " \
        "If \"%s\" has no password stored, the one this session currently " \
        "holds is cleared.\n\n" \
        "Nothing is written to the saved session until you press Save."
#define KT_CFG_PXLOAD_SAVEONEXIT_WARN                "This session has \"Save settings on exit\" enabled, so this WILL be " \
        "saved over your stored proxy settings when the session ends, even if " \
        "you never press Save."

/* kitty_config.c: Application/Workplace Proxy */
#define KT_CFG_WPMODE_1H                             "1 hour"
#define KT_CFG_WPMODE_2H                             "2 hours"
#define KT_CFG_WPMODE_4H                             "4 hours"
#define KT_CFG_WPMODE_8H                             "8 hours"
#define KT_CFG_WPMODE_12H                            "12 hours"
#define KT_CFG_WPMODE_LAUNCHER_EXIT                  "Only when the launcher exits"
#define KT_CFG_WPMODE_SWITCH_OFF_LEFT                "Switch off now (%s left)"
#define KT_CFG_WPMODE_SWITCH_OFF                     "Switch off now"
#define KT_CFG_WPMODE_TAG_IN_USE                     "  (in use)"
#define KT_CFG_WPMODE_TAG_LAST_USED                  "  (last used)"
#define KT_CFG_WPMODE_OFF_FAILED                     "The launcher did not switch workplace proxy mode " \
        "off. Closing the launcher also switches it off."
#define KT_CFG_WPMODE_ON_FAILED                      "Could not switch workplace proxy mode on: the " \
        "launcher, which holds the mode, did not start."

/* kitty_config.c: Connection/SSH sub-panels (bug droplists, TTY modes, tunnels, host keys, credentials) */
#define KT_CFG_SSHBUG_ON                             "On"
#define KT_CFG_TTYMODE_AUTO                          "(auto)"
#define KT_CFG_TTYMODE_DONT_SEND                     "(don't send)"
#define KT_CFG_PORTFWD_NEED_SOURCE                   "You need to specify a source port number"
#define KT_CFG_PORTFWD_NEED_DEST                     "You need to specify a destination address\n" \
        "in the form \"host.name:port\""
#define KT_CFG_PORTFWD_EXISTS                        "Specified forwarding already exists"
#define KT_CFG_HOSTKEY_NEED_KEY                      "You need to specify a host key or " \
        "fingerprint"
#define KT_CFG_HOSTKEY_INVALID                       "Host key is not in a valid format"
#define KT_CFG_HOSTKEY_LISTED                        "Specified host key is already listed"
#define KT_CAP_KEY_FINGERPRINT_PIN                   "KiTTY key fingerprint pin"
#define KT_CFG_PIN_CHOOSE_KEY_FIRST                  "Choose a private key file first - the pin records THAT " \
        "file's fingerprint."
#define KT_CFG_PIN_READ_FAILED                       "Unable to read the key file's public half:\n\n%s"
#define KT_CFG_PIN_RECORDED                          "Recorded for this session:\n\n%s\n\n" \
        "Connections will now refuse the key file if its " \
        "fingerprint changes. Clear the field to switch the check " \
        "off - and remember to SAVE the session."
#define KT_CFG_LOGINSCRIPT_NO_BOX                    "The login script box is not available."
#define KT_CFG_LOGINSCRIPT_SELECT                    "Select a login script file"
#define KT_CFG_LOGINSCRIPT_OPEN_FAILED               "That file could not be opened."

/* kitty_config.c: Connection/Serial */
#define KT_CFG_PARITY_ODD                            "Odd"
#define KT_CFG_PARITY_EVEN                           "Even"
#define KT_CFG_PARITY_MARK                           "Mark"
#define KT_CFG_PARITY_SPACE                          "Space"
#define KT_CFG_FLOW_XONXOFF                          "XON/XOFF"
#define KT_CFG_FLOW_RTSCTS                           "RTS/CTS"
#define KT_CFG_FLOW_DSRDTR                           "DSR/DTR"

/* kitty_config.c: Application/Config Window droplists */
#define KT_CFG_THEME_FOLLOW_WINDOWS                  "Follow Windows"
#define KT_CFG_THEME_LIGHT                           "Light"
#define KT_CFG_THEME_DARK                            "Dark"
#define KT_CFG_PROXYCHOOSER_ONCE_DEFINED             "Only once a named proxy exists"
#define KT_CFG_TREE_EVERYTHING                       "Everything"
#define KT_CFG_TREE_TOP_ONLY                         "Top categories only"
#define KT_CFG_TREE_TWO_LEVELS                       "Two levels"
#define KT_CFG_TREE_THREE_LEVELS                     "Three levels"
#define KT_CFG_DBLCLICK_OPEN_CLOSE                   "Open Terminal and close Config"
#define KT_CFG_DBLCLICK_START_NEW                    "Start Terminal in new window"

/* kitty_proxy_gui.c: Application/Named Proxies */
#define KT_CAP_NAMED_PROXY                           "KiTTY named proxy"
#define KT_PROXYGUI_TYPE_HTTP                        "HTTP"
#define KT_PROXYGUI_TYPE_TELNET                      "Telnet"
#define KT_PROXYGUI_TYPE_LOCAL                       "Local (command)"
#define KT_PROXYGUI_TYPE_SSH_TCPIP                   "SSH jump host (port forwarding)"
#define KT_PROXYGUI_TYPE_SSH_EXEC                    "SSH jump host (execute a command)"
#define KT_PROXYGUI_TYPE_SSH_SUBSYSTEM               "SSH jump host (invoke a subsystem)"
#define KT_PROXYGUI_DNS_AUTO                         "auto"
#define KT_PROXYGUI_DNS_LOCAL                        "local"
#define KT_PROXYGUI_DNS_PROXY                        "proxy"
#define KT_PROXYGUI_LOG_NEVER                        "never"
#define KT_PROXYGUI_LOG_ALWAYS                       "always"
#define KT_PROXYGUI_LOG_CONNECT_ONLY                 "connect only"
#define KT_PROXYGUI_HOSTIS_GLOBAL                    "as globally configured (see Defaults)"
#define KT_PROXYGUI_HOSTIS_HOSTNAME                  "a hostname or IP-address"
#define KT_PROXYGUI_HOSTIS_SESSION                   "possibly the name of a saved session (PuTTY's old rule)"
#define KT_PROXYGUI_EDITING                          "Editing this definition. Nothing is stored until Save."
#define KT_PROXYGUI_NAME_FIRST                       "Give the proxy a name first."
#define KT_PROXYGUI_DEFINED                          "Proxy defined.\r\n\r\nThe proxy-override droplist appears in the " \
        "Session panel the next time a configuration window is opened."
#define KT_PROXYGUI_SAVED                            "Saved."
#define KT_PROXYGUI_DELETE_Q                         "Delete the named proxy \"%s\"?"
#define KT_PROXYGUI_LAST_REMOVED                     "The last named proxy was removed.\r\n\r\nThe proxy-override " \
        "droplist disappears from the Session panel the next time a " \
        "configuration window is opened."
#define KT_PROXYGUI_DELETED                          "Deleted."
#define KT_PROXYGUI_UNSAVED_Q                        "This named proxy has changes that have not been saved.\r\n\r\n" \
        "Leave the panel and discard them?"

/* kitty_mpw_gui.c: the master-password dialog */
#define KT_MPW_PROMPT_SET                            "Set a master password. It encrypts the passwords saved in your " \
        "portable session files, and they stay usable when you copy the " \
        "files to another PC.\r\n\r\n" \
        "If you lose the master password, the protected passwords CANNOT " \
        "be recovered.\r\n\r\n" \
        "If you cancel, saved passwords are protected with Windows DPAPI " \
        "instead: only this Windows account on this machine can read them " \
        "(a roaming domain profile may also work on other machines).\r\n\r\n" \
        "For unattended/automation setups: -masterpwfile <file> supplies " \
        "the master password without a prompt, and kitty.ini " \
        "PortablePasswordProtection can settle the choice permanently - " \
        "'dpapi' to always use DPAPI and never ask again, 'legacy' for " \
        "unprotected storage (see kitty.ini.example)."
#define KT_MPW_PROMPT_UNLOCK                         "Enter your master password to unlock the saved session password."
#define KT_MPW_EMPTY                                 "Please enter a master password."
#define KT_MPW_MISMATCH                              "The two master passwords do not match."

/* kitty_inputbox.c: the text-input / command console box */
#define KT_INPUTBOX_TITLE_PORTABLE                   "Text input (portable mode) - /help = KiTTY commands"
#define KT_INPUTBOX_TITLE                            "Text input - /help = KiTTY commands"
#define KT_INPUTBOX_TITLE_SUFFIX                     " - Text input"
#define KT_CAP_LOAD_WARNING                          "Load Warning"
#define KT_CAP_SAVE_WARNING                          "Save Warning"
#define KT_INPUTBOX_LOAD_NOTES_Q                     "Are you sure you want to load Notes\nand erase this edit box ?"
#define KT_INPUTBOX_SAVE_NOTES_Q                     "Are you sure you want to save Edit box\ninto Notes registry ?"

/* kitty_storemove.c: portable copy out, folder store in (captions = the panel's group titles) */
#define KT_STOREMOVE_TITLE_OUT                       KT_INIMIG_OUT_GROUP
#define KT_STOREMOVE_TITLE_IN                        KT_INIMIG_IN_GROUP
#define KT_STOREMOVE_COPY_DONE                       "Everything is in\n%s\n\n" \
        "Run kitty.exe from there: that copy uses the files in this " \
        "folder and not the registry. This KiTTY and its settings are " \
        "unchanged.\n\n" \
        "%d session%s, %d prox%s, %d host key%s, %d setting%s and " \
        "%d program file%s copied."
#define KT_STOREMOVE_COPY_MPW                        "\n\nThe copy unlocks with the master password " \
        "you just entered."
#define KT_STOREMOVE_COPY_DPAPI                      "\n\nIts passwords are readable by this Windows " \
        "account on this PC only."
#define KT_STOREMOVE_COPY_FAILED                     "\n\n%d item%s could not be written."
#define KT_STOREMOVE_NO_TEMP                         "No temporary folder could be created; nothing " \
        "was taken."
#define KT_STOREMOVE_TAKEN                           "Taken from\n%s\n\n" \
        "%d session%s, %d prox%s, %d host key%s, %d setting%s."
#define KT_STOREMOVE_TAKEN_KEPT                      "\n%d kept as already present."
#define KT_STOREMOVE_TAKEN_FAILED                    "\n%d item%s could not be taken."
#define KT_STOREMOVE_TAKEN_REPROTECTED               "\n\nSaved passwords were re-protected for the store " \
        "in use. Settings apply after a restart."
#define KT_STOREMOVE_NOT_WRITABLE                    "Nothing can be written to\n%s\n\nChoose another folder."
#define KT_STOREMOVE_ALREADY_STORE_Q                 "%s\n\nalready holds a KiTTY store. Files of the same name " \
        "are overwritten, others stay.\n\nContinue?"
/* prefix only: the caller appends the ini file name and the full stop */
#define KT_STOREMOVE_NO_STORE_PREFIX                 "%s\n\nholds no KiTTY folder store: neither a Sessions " \
        "folder nor a "
#define KT_STOREMOVE_TAKE_Q                          "Sessions, named proxies, host keys and settings of the folder " \
        "are taken into the registry. The folder stays as it is.\n" \
        "Whatever the folder does not hold stays in the registry as it " \
        "is.\n\n" \
        "Yes  -  entries of the same name are overwritten\n" \
        "No  -  entries of the same name are kept\n" \
        "Cancel  -  do nothing"

/* kitty_settings_load.c */
#define KT_CAP_KTX_ENCRYPTED_DEPRECATED              "KiTTY - encrypted configuration files are deprecated"
#define KT_SETTINGS_KTX_ENCRYPTED_DEPRECATED         "This file was written by an older KiTTY with \"encrypted configuration " \
        "files\" switched on. It has been read normally.\n\n" \
        "That option is gone. It scrambled the file with a key built into every " \
        "copy of KiTTY, so anyone with KiTTY could unscramble it - it protected " \
        "nothing. KiTTY still READS these files, but no longer writes them, and " \
        "anything you export from now on will be plain.\n\n" \
        "Saved passwords are unaffected: those are protected properly, with " \
        "Windows DPAPI or your master password."
#define KT_SETTINGS_FILE_NOT_FOUND                   "File %s not found !"

/* kitty_store.c: the folder store's error boxes */
#define KT_STORE_ERR_PREFIX                          "Error: "
#define KT_STORE_ERR_DIRECTORY                       "Directory: "
#define KT_STORE_ERR_CODE                            "Error code: "
#define KT_STORE_MKDIR_FAILED_BANG                   "Unable to create directory !"
#define KT_STORE_SESSDIR_FAILED                      "Unable to create sessions directory !"
#define KT_STORE_EXPAND_ENV_FAILED                   "Unable to ExpandEnvironmentStrings for session path"
#define KT_STORE_EXPAND_ENV_USER_FAILED              "Unable to ExpandEnvironmentStringsForUser for session path"
#define KT_STORE_CONF_READ_FAILED                    "Unable to read configuration file, falling back to defaults"
#define KT_STORE_SESSION_READ_FAILED                 "Unable to read session file"

/* ---- Batch 6: resource scripts ---- */
/* Dialog templates in windows/kitty.rc, pageant.rc, puttygen.rc and
 * putty-common.rc2 read their text from here. windres runs the C preprocessor
 * over the .rc, so a macro stands in for the literal - but ONLY a macro whose
 * value is ONE string token: the resource compiler does not concatenate
 * adjacent literals, so nothing in this section may be split across lines.
 * Named KT_RC_<dialog>_<what> after the IDD_ (or the numeric id's role). */

/* kitty.rc: IDD_KITTYABOUT */
#define KT_RC_KITTYABOUT_CAPTION                     "About KiTTY"
#define KT_RC_KITTYABOUT_BLURB                       "KiTTY is a fork of PuTTY, continued by kapper.net.\nInspired by the KiTTY by Cyril Dupont (9bis.com) - thanks to all contributors."
#define KT_RC_KITTYABOUT_WEBPAGE                     "Visit github.com/hknet/KiTTY"

/* kitty.rc: IDD_MASTERPW (the caption is IDD_MPWMOVED's too) */
#define KT_RC_MASTERPW_CAPTION                       "KiTTY master password"
#define KT_RC_MASTERPW_LABEL                         "Master password:"
#define KT_RC_MASTERPW_CONFIRM_LABEL                 "Confirm master password:"

/* kitty.rc: IDD_MIGRATEWARN */
#define KT_RC_MIGRATEWARN_CAPTION                    "KiTTY password protection"
#define KT_RC_MIGRATEWARN_TEXT                       "This session file contains a password stored in the old unprotected format. Saving will re-encrypt it in the new protected format, which old KiTTY releases (0.76 and earlier) cannot read. Do not copy the file back to an old release afterwards."
#define KT_RC_MIGRATEWARN_QUESTION                   "Re-encrypt the stored password now? (No keeps it in the old form.)"
#define KT_RC_MIGRATEWARN_NOASK                      "Don't show this warning again"

/* kitty.rc: IDD_EXPORTPW (MODEDPAPI is IDD_STOREMOVEPW's too) */
#define KT_RC_EXPORTPW_CAPTION                       "Export sessions"
#define KT_RC_EXPORTPW_INTRO                         "Choose how the exported session files are protected. This password belongs to the exported files only: it is not your master password, and nothing about the sessions saved here is changed."
#define KT_RC_EXPORTPW_MODEPW                        "Protect with a &password (can be imported on any PC)"
#define KT_RC_EXPORTPW_PASS_LABEL                    "Password (at least 5 characters):"
#define KT_RC_EXPORTPW_MODEDPAPI                     "Protect for &this PC only (no password)"
#define KT_RC_EXPORTPW_DPAPIWARN                     "The exported files can then only be imported with THIS Windows account on THIS PC. Copying them to another PC will not work."
#define KT_RC_EXPORTPW_EXPORT                        "&Export"

/* kitty.rc: IDD_STOREMOVEPW (caption = KT_INIMIG_OUT_GROUP) */
#define KT_RC_STOREMOVEPW_INTRO                      "Choose how the passwords saved in the copy are protected. Nothing about this KiTTY changes."
#define KT_RC_STOREMOVEPW_MODEPW                     "Protect with a &master password (the copy asks for it once per start, works on any PC)"
#define KT_RC_STOREMOVEPW_PASS_LABEL                 "Master password (at least 5 characters):"
#define KT_RC_STOREMOVEPW_DPAPIWARN                  "The copy can then read its passwords only under THIS Windows account on THIS PC. A moved or synced copy cannot."

/* kitty.rc: IDD_EXPORTDONE (caption = KT_CAP_SESSION_EXPORT) */
#define KT_RC_EXPORTDONE_PW_LABEL                    "Export password:"

/* kitty.rc: IDD_IMPORTPW */
#define KT_RC_IMPORTPW_CAPTION                       "Import sessions"
#define KT_RC_IMPORTPW_PASS_LABEL                    "Import password:"
#define KT_RC_IMPORTPW_IMPORT                        "&Import"

/* kitty.rc: IDD_MPWMOVED (OPEN_FOLDER is pageant.rc IDD_HELLOPROTECT's too) */
#define KT_RC_MPWMOVED_COPY_PATH                     "&Copy path"
#define KT_RC_MPWMOVED_OPEN_FOLDER                   "&Open folder"

/* kitty.rc: IDD_UPDATEBOX (caption = KT_CAP_UPDATE) */
#define KT_RC_UPDATEBOX_NOTES                        "&Release notes"
#define KT_RC_UPDATEBOX_UPDATE                       "&Update now"
#define KT_RC_UPDATEBOX_LATER                        "&Later"

/* kitty.rc: IDD_INPUTBOX, IDD_INPUTBOXMULTI, IDD_INPUTBOXPW (one caption) */
#define KT_RC_INPUTBOX_CAPTION                       "KiTTY text input"
#define KT_RC_INPUTBOX_PROMPT                        "Text to send to the terminal:"
#define KT_RC_INPUTBOXMULTI_PROMPT                   "Text to send (Shift+Return):"
#define KT_RC_INPUTBOXPW_PROMPT                      "Password to send to the terminal:"

/* kitty.rc: IDD_TITLEVARS */
#define KT_RC_TITLEVARS_CAPTION                      "Window title placeholders"
#define KT_RC_TITLEVARS_INTRO                        "Double-click a placeholder (or select one and press Copy) to put it on the clipboard, then paste it into the Window title field."

/* kitty.rc: IDD_OSC52READ (Deny = KT_REMOTE_CLIPBOARD_DENY) */
#define KT_RC_OSC52READ_CAPTION                      "KiTTY - a server wants to read your clipboard"
#define KT_RC_OSC52READ_VIEW                         "&View..."
#define KT_RC_OSC52READ_APPLY_LABEL                  "Apply this decision to:"
#define KT_RC_OSC52READ_ONCE                         "just this &request"
#define KT_RC_OSC52READ_SESSION                      "the rest of this terminal &session"
#define KT_RC_OSC52READ_ALWAYSDENY                   "Always &deny for this host (saved in the session)"

/* kitty.rc: IDD_HELPBOX */
#define KT_RC_HELPBOX_CAPTION                        "KiTTY internal commands"

/* pageant.rc: IDD_LOAD_PASSPHRASE */
#define KT_RC_LOAD_PASSPHRASE_CAPTION                "kageant: Loading Encrypted Key"
#define KT_RC_LOAD_PASSPHRASE_PROMPT                 "Enter passphrase to load key"

/* pageant.rc: IDD_ONDEMAND_PASSPHRASE (three static lines) */
#define KT_RC_ONDEMAND_PASSPHRASE_CAPTION            "kageant: Decrypting Stored Key"
#define KT_RC_ONDEMAND_PASSPHRASE_LINE1              "A client of kageant wants to use the following encrypted key:"
#define KT_RC_ONDEMAND_PASSPHRASE_LINE2              "If you intended this, click in this box to make sure it has"
#define KT_RC_ONDEMAND_PASSPHRASE_LINE3              "input focus, then enter the passphrase to decrypt the key."

/* pageant.rc: IDD_KEYLIST (caption = KT_CAP_KA_KEY_LIST) */
#define KT_RC_KEYLIST_ADDKEY_ENC                     "Add Key (&encrypted)"
#define KT_RC_KEYLIST_MOVEUP                         "Move &Up"
#define KT_RC_KEYLIST_MOVEDOWN                       "Move D&own"
#define KT_RC_KEYLIST_CONFIRM_LABEL                  "Confirm key use:"
#define KT_RC_KEYLIST_CONFIRM_YES                    "Ever&y use"
#define KT_RC_KEYLIST_CONFIRM_AUTO                   "By key co&mment"
#define KT_RC_KEYLIST_RESUMECONFIRM                  "Stop rejec&ting keys"
#define KT_RC_KEYLIST_NEWKEY                         "Ne&w key..."
#define KT_RC_KEYLIST_STOPAGENT                      "Stop a&gent"
#define KT_RC_KEYLIST_FPTYPE_LABEL                   "&Fingerprint type:"
#define KT_RC_KEYLIST_SHOWUNAVAIL                    "Show unavailable keys"
#define KT_RC_KEYLIST_RETRY                          "Retry unavailable &keys"

/* pageant.rc: IDD_KEYDETAILS (caption = KT_CAP_KA_KEY_DETAILS) */
#define KT_RC_KEYDETAILS_KEY                         "Key:"
#define KT_RC_KEYDETAILS_STATE                       "State:"
#define KT_RC_KEYDETAILS_FINGERPRINTS                "Fingerprints:"
#define KT_RC_KEYDETAILS_COMMENT                     "Comment:"
#define KT_RC_KEYDETAILS_LOADED_FROM                 "Loaded from:"
#define KT_RC_KEYDETAILS_LOCATE                      "&Locate..."
#define KT_RC_KEYDETAILS_LIFETIME                    "Lifetime:"
#define KT_RC_KEYDETAILS_DEFER                       "Load this key &deferred at startup (passphrase at first use)"
#define KT_RC_KEYDETAILS_CONFIRM_LABEL               "Confir&m each use:"
#define KT_RC_KEYDETAILS_AUTOENC_LABEL               "Re-encr&ypt after idle:"
#define KT_RC_KEYDETAILS_ACCEPT                      "&Accept this key"
#define KT_RC_KEYDETAILS_PROTECT                     "&Protect with Windows Hello..."
#define KT_RC_KEYDETAILS_FORGET                      "For&get a path..."

/* pageant.rc: IDD_KEYSETTINGS (caption = KT_CAP_KA_SETTINGS) */
#define KT_RC_KEYSETTINGS_NOTICE_LABEL               "Notice &display (seconds):"
#define KT_RC_KEYSETTINGS_NOTICE_HINT                "blank = each notice's own default"
#define KT_RC_KEYSETTINGS_THEME_LABEL                "&Colour theme:"
#define KT_RC_KEYSETTINGS_THEME_HINT                 "Dark needs Windows 10 1809 or newer; anything older stays light."
#define KT_RC_KEYSETTINGS_LOCKDOWN                   "&Lock down: refuse ALL key add/remove over IPC"
#define KT_RC_KEYSETTINGS_BLOCKADD                   "Block &adding keys over IPC"
#define KT_RC_KEYSETTINGS_BLOCKREMOVE                "Block re&moving keys over IPC"
#define KT_RC_KEYSETTINGS_HELLO                      "Confirmations require Windows &Hello"
#define KT_RC_KEYSETTINGS_TTL_LABEL                  "&Passphrase cache (seconds):"
#define KT_RC_KEYSETTINGS_TTL_HINT                   "0 = do not cache; blank = the default 60; maximum 300 (5 min). Keys that share a passphrase then load with one prompt."
#define KT_RC_KEYSETTINGS_HELLOTTL_LABEL             "Windows Hello cach&e (seconds):"
#define KT_RC_KEYSETTINGS_HELLOTTL_HINT              "One Hello covers a batch; 0 = every unlock asks; blank = the default 60. Maximum 300."
#define KT_RC_KEYSETTINGS_AUTOENC_LABEL              "Re-encr&ypt keys after idle:"
#define KT_RC_KEYSETTINGS_AUTOENC_HINT               "Seconds, or 10m / 2h / 1d; \"use\" = right after each use. 30 s to 7 d."
#define KT_RC_KEYSETTINGS_RETRY_LABEL                "&Retry startup keys to load, when a drive appears:"
#define KT_RC_KEYSETTINGS_UNLOAD                     "&Unload a key when its media is removed"
#define KT_RC_KEYSETTINGS_QUIET                      "Do not &warn about missing startup keys"
#define KT_RC_KEYSETTINGS_AGENTLOG                   "Wr&ite the agent log"
#define KT_RC_KEYSETTINGS_LOGPATH_LABEL              "&File (blank = default):"
#define KT_RC_KEYSETTINGS_LOGKB_LABEL                "Rota&te at (KB):"
#define KT_RC_KEYSETTINGS_LOGKEEP_LABEL              "&Generations:"
#define KT_RC_KEYSETTINGS_LOGDAYS_LABEL              "E&xpunge rotated logs after (days, 0 = keep):"
#define KT_RC_KEYSETTINGS_LOGNOTE                    "The log is evidence, not proof - see the manual."

/* pageant.rc: IDD_AUDITVIEW (caption = KT_CAP_KA_AGENT_LOG), IDD_AUDITDETAIL */
#define KT_RC_AUDITVIEW_FILTER_LABEL                 "&Filter:"
#define KT_RC_AUDITVIEW_APP_LABEL                    "&App:"
#define KT_RC_AUDITVIEW_ENABLE                       "&Logging"
#define KT_RC_AUDITDETAIL_CAPTION                    "kageant - agent log record"

/* pageant.rc: IDD_ABOUT (the two buttons are puttygen.rc's About box's too) */
#define KT_RC_ABOUT_CAPTION                          "About kageant"
#define KT_RC_ABOUT_LICENCE                          "View &Licence"
#define KT_RC_ABOUT_WEBSITE                          "Visit &Web Site"

/* pageant.rc IDD_LICENCE, puttygen.rc 214, putty-common.rc2 IDD_LICENCEBOX */
#define KT_RC_LICENCE_CAPTION                        "KiTTY Licence"

/* pageant.rc: IDD_HELLOPROTECT (KEYFILE is puttygen.rc IDD_KGHELLODOORS' too) */
#define KT_RC_HELLOPROTECT_CAPTION                   "kageant - protect a key with Windows Hello"
#define KT_RC_HELLOPROTECT_KEYFILE                   "Key file:"
#define KT_RC_HELLOPROTECT_DEST                      "Protected copy:"
#define KT_RC_HELLOPROTECT_BROWSE                    "&Browse..."
#define KT_RC_HELLOPROTECT_SRCPASS                   "Current passphrase:"
#define KT_RC_HELLOPROTECT_USESRCPASS                "&Use the current passphrase as the recovery passphrase"
#define KT_RC_HELLOPROTECT_RECPASS                   "Recovery passphrase:"
#define KT_RC_HELLOPROTECT_RECPASS2                  "Confirm:"
#define KT_RC_HELLOPROTECT_HELLOONLY                 "&No recovery passphrase - Windows Hello and the printed secret only"
#define KT_RC_HELLOPROTECT_REPLACE                   "&Replace this key's startup entry with the protected copy"
#define KT_RC_HELLOPROTECT_SIDEBOUND                 "Printout is a recover&y code bound to the .hello file (not"
#define KT_RC_HELLOPROTECT_SIDEBOUND2                "the key's passphrase - it opens nothing without the file)"

/* puttygen.rc: 210 = passphrase prompt (KGPASS), 213 = About (KGABOUT),
 * 215 = private key file parameters (KGPPKPARAMS), 216 = certificate info
 * (KGCERTINFO); 201's caption = KT_CAP_KGEN_TITLE */
#define KT_RC_KGPASS_CAPTION                         "KiTTYgen: Enter Passphrase"
#define KT_RC_KGPASS_PROMPT                          "Enter passphrase for key"
#define KT_RC_KGABOUT_CAPTION                        "About KiTTYgen"
#define KT_RC_KGPPKPARAMS_CAPTION                    "KiTTYgen: Private Key File Parameters"
#define KT_RC_KGPPKPARAMS_PPKVER                     "PPK file version:"
#define KT_RC_KGPPKPARAMS_KDF                        "Key derivation function:"
#define KT_RC_KGPPKPARAMS_MEM                        "Memory for passphrase hash:"
#define KT_RC_KGPPKPARAMS_TIME                       "Time to use for passphrase hash:"
#define KT_RC_KGPPKPARAMS_PARALLEL                   "Parallelism for passphrase hash:"
#define KT_RC_KGCERTINFO_CAPTION                     "KiTTYgen: certificate information"

/* puttygen.rc: IDD_KGHELLODOORS */
#define KT_RC_KGHELLODOORS_CAPTION                   "KiTTYgen - Windows Hello doors"
#define KT_RC_KGHELLODOORS_INTRO                     "Everything that opens this key:"
#define KT_RC_KGHELLODOORS_REMOVE                    "&Remove selected entry"
#define KT_RC_KGHELLODOORS_ADD                       "&Add this computer..."
#define KT_RC_KGHELLODOORS_NOTE1                     "Removing needs no unlock; adding asks for the recovery"
#define KT_RC_KGHELLODOORS_NOTE2                     "passphrase or the printed secret, then Windows Hello."

#endif /* KITTY_TEXT_H */
