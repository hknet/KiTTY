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

#define KT_MIG_IMP_GROUP   "Import into this KiTTY"
#define KT_MIG_IMP_INTRO   "An import copies. The old store is left as it is."
#define KT_MIG_IMP_BUTTON  "Import selected Sessions"
#define KT_MIG_IMP_NOSEL   "Nothing was selected."
#define KT_MIG_IMP_DONE    "Imported. The copies are in the session list."
#define KT_MIG_IMP_NONE    "Nothing was imported."

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
#define KT_TITLE_OPTIONS_CONTROLLING_THE_WINDOW_TITLE "Options controlling the window title"
#define KT_TITLE_ADJUST_THE_BEHAVIOUR                "Adjust the behaviour of the window title"
#define KT_TITLE_WINDOW_TITLE                        "Window title:"
#define KT_TITLE_PLACEHOLDERS_H_S                    "Placeholders (%h, %s, ...)"
#define KT_TITLE_SEPARATE_WINDOW_AND_ICON_TITLES     "Host may set window and taskbar titles separately"

/* Window/Behaviour */
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
#define KT_TRANSPARENCY_OPTIONS_CONTROLLING_TRANSPARENCY "Options controlling transparency"
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
#define KT_COPY_OPTIONS_CONTROLLING_COPYING_FROM_TERMINAL "Options controlling copying from terminal to clipboard"
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
#define KT_PROXY_NAMED_PROXIES_PROXY_TEMPLATES       "Named proxies (proxy templates)"
#define KT_PROXY_EDIT_NAMED_PROXIES                  "Edit named proxies..."
#define KT_PROXY_THIS_SESSION_S_OWN_PROXY            "This session's own proxy"
#define KT_PROXY_NAMED_PROXY_PRE_SETS                "Named proxy pre-sets"
#define KT_PROXY_LOAD                                "Load"
#define KT_PROXY_PROXY_TYPE                          "Proxy type:"
#define KT_PROXY_PROXY_HOSTNAME                      "Proxy hostname"
#define KT_PROXY_PORT                                "Port"
#define KT_PROXY_EXCLUDE_HOSTS_IPS                   "Exclude Hosts/IPs"
#define KT_PROXY_CONSIDER_PROXYING_LOCAL_HOST_CONNECTIONS "Consider proxying local host connections"
#define KT_PROXY_DO_DNS_NAME_LOOKUP                  "Do DNS name lookup at proxy end:"
#define KT_PROXY_NO                                  "No"
#define KT_PROXY_YES                                 "Yes"
#define KT_PROXY_USERNAME                            "Username"
#define KT_PROXY_PASSWORD                            "Password"
#define KT_PROXY_COMMAND_TO_SEND_TO_PROXY            "Command to send to proxy (for some types)"
#define KT_PROXY_PRINT_PROXY_DIAGNOSTICS             "Print proxy diagnostics " \
        "in the terminal window"
#define KT_PROXY_ONLY_UNTIL_SESSION_STARTS           "Only until session starts"

/* Application/Workplace proxy */
#define KT_WORKPLACE_PROXY_WORKPLACE_PROXY_MODE_APPLICATION_WIDE "Workplace proxy mode (application-wide)"
#define KT_WORKPLACE_PROXY_WHILE_IT_IS_ON_EVERY      "While on, EVERY connection uses the proxy below. No " \
        "session is changed."
#define KT_WORKPLACE_PROXY_PROXY_FOR_EVERYTHING      "Proxy for everything:"
#define KT_WORKPLACE_PROXY_SWITCH_OFF_AFTER          "Switch off after:"
#define KT_WORKPLACE_PROXY_SWITCH                    "Switch on"
#define KT_WORKPLACE_PROXY_NEEDS_NAMED               "Named proxies need to be configured first."

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

/* Application/Config window */
#define KT_CONFIG_WINDOW_THIS_WINDOW                 "This window"
#define KT_CONFIG_WINDOW_APPEARANCE                  "Appearance"
#define KT_CONFIG_WINDOW_COLOURS                     "Choose the default Appearance:"
#define KT_CONFIG_WINDOW_ONE_SETTING_FOR_THE_WHOLE   "One setting for the whole suite - kitty, kageant and " \
        "kittygen all read it. Dark needs Windows 10 1809 or newer."
#define KT_CONFIG_WINDOW_CHANGES_APPLY_TO_WINDOWS_OPENED "Changes apply to windows opened afterwards - this " \
        "configuration window keeps the colours it opened with."
#define KT_CONFIG_WINDOW_CATEGORY_TREE_OPENS_SHOWING "Category tree opens showing:"
#define KT_CONFIG_WINDOW_SIZE                        "Size"
#define KT_CONFIG_WINDOW_WINDOW_HEIGHT_IN_PIXELS_BLANK "Window height, in pixels (blank = default):"
#define KT_CONFIG_WINDOW_WINDOW_WIDTH_IN_PIXELS_BLANK "Window width, in pixels (blank = default):"
#define KT_CONFIG_WINDOW_CLOSING_A_TERMINAL_WINDOW   "Terminal window exit"
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
#define KT_EXTERNAL_TOOLS_WHERE_THESE_ARE_INSTALLED  "Where these are installed is a property of this PC, so they " \
        "are kept in kitty.ini and shared by every session."

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

/* Application/Session parameter */
#define KT_SESSION_PARAMETER_THE_SESSION_LIST        "The session list"
#define KT_SESSION_PARAMETER_THE_LIST                "The list"
#define KT_SESSION_PARAMETER_LENGTH_IN_ROWS_7        "Length, in rows (" \
        KITTY_STR(KITTY_CFG_SESSION_ROWS_MIN) " or more):"
#define KT_SESSION_PARAMETER_SHOW_DEFAULT_SETTINGS   "Show \"Default Settings\" in the list"
#define KT_SESSION_PARAMETER_QUICK_CONNECT_NEEDS_IT_LOADING "Quick connect needs it: loading Default Settings is how you " \
        "get back to that mode."
#define KT_SESSION_PARAMETER_SHOW_FOLDERS_AS_ROWS_NOT "Show folders as rows, not a drop-down"
#define KT_SESSION_PARAMETER_SEARCH_THE_LIST_AS_YOU  "Search the list as you type"
#define KT_SESSION_PARAMETER_OPENING                 "Opening"
#define KT_SESSION_PARAMETER_OPEN_ON_THE_LAST_USED   "Open on the last used session (off = quick connect)"
#define KT_SESSION_PARAMETER_QUICK_CONNECT_STARTS_EVERY_KITTY "Quick connect starts every KiTTY on Default Settings with the " \
        "cursor already in Host Name: type an address and press Enter."
#define KT_SESSION_PARAMETER_DOUBLE_CLICK_A_SESSION  "Double-click a session to:"
#define KT_SESSION_PARAMETER_PROXY                   "Proxy"
#define KT_SESSION_PARAMETER_SHOW_THE_PROXY_CHOOSER  "Show the proxy chooser:"
#define KT_SESSION_PARAMETER_NEVER_ALSO_HIDES_THE_EDIT "Never also hides the Edit button. Definitions stay on " \
        "Application > Named proxies."

/* Application/Updates */
#define KT_UPDATES_KEEPING_KITTY_UP_TO_DATE          "Keeping KiTTY up to date"
#define KT_UPDATES_UPDATE_CHECK                      "Update check"
#define KT_UPDATES_CHECK_FOR_UPDATES_WHEN_KITTY      "Check for updates when KiTTY starts"
#define KT_UPDATES_CHECK_FOR_UPDATES_NOW             "Check for updates now"

/* Comment */
#define KT_COMMENT_COMMENT_FOR_THIS_SESSION          "Comment for this session"
#define KT_COMMENT_SESSION_COMMENT                   "Session comment"

/* Application/Named proxies */
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

#endif /* KITTY_TEXT_H */
