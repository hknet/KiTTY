/*
 * kitty_tree_text.h - the Category tree's DISPLAY names, in one place.
 *
 * A panel's path ("Connection/Login/Environment") is an IDENTIFIER: settings
 * code, remembered panels, help mappings and the QA harnesses all key on it,
 * so paths never change language. What the tree SHOWS for each path component
 * is looked up here instead - the first step towards translations: point a
 * row's display column at translated text and the tree follows, while every
 * key stays what it was.
 *
 * A component missing from the table shows itself, so adding a panel works
 * without touching this file - but keep the table complete, because a
 * translator works from it.
 */

typedef struct {
    const char *component;   /* the path component - NEVER translated */
    const char *display;     /* what the tree shows for it */
} KittyTreeLabel;

static const KittyTreeLabel kitty_tree_labels[] = {
    { "Appearance",              "Appearance" },
    { "Application",             "Application" },
    { "Application keypad",      "App Keypad/Cursor" },
    { "Auth",                    "Auth" },
    { "Back.&Image",             "Background & Image" },
    { "Behaviour",               "Behaviour" },
    { "Bell",                    "Bell" },
    { "Broadcast",               "Broadcast" },
    { "Bugs",                    "Bugs" },
    { "Certificate Authorities", "Certificate Authorities" },
    { "Charset translation",     "Charset translation" },
    { "Cipher",                  "Cipher" },
    { "Colours",                 "Colours" },
    { "Comment",                 "Comment" },
    { "Config window",           "Config window" },
    { "Connection",              "Connection" },
    { "Copy",                    "Copy" },
    { "Credentials",             "Credentials" },
    { "Data",                    "Data" },
    { "Environment",             "Environment" },
    { "External tools",          "External tools" },
    { "Features",                "Features" },
    { "GSSAPI",                  "GSSAPI" },
    { "Host keys",               "Host keys" },
    { "Hyperlinks",              "Hyperlinks" },
    { "Icon",                    "Icon" },
    { "KSCP",                    "KSCP" },
    { "Kex",                     "Kex" },
    { "Keyboard",                "Keyboard" },
    { "Limits",                  "Limits" },
    { "Logging",                 "Logging" },
    { "Login",                   "Login" },
    { "Migration",               "Migration" },
    { "More bugs",               "More bugs" },
    { "Notices",                 "Notices" },
    { "Position",                "Position" },
    { "Precise colours",         "Precise colours" },
    { "Proxy",                   "Proxy" },
    { "Remote clipboard",        "Remote clipboard" },
    { "Rlogin",                  "Rlogin" },
    { "SSH",                     "SSH" },
    { "SUPDUP",                  "SUPDUP" },
    { "Scripting",               "Scripting" },
    { "Security",                "Security" },
    { "Selection",               "Selection" },
    { "Serial",                  "Serial" },
    { "Session",                 "Session" },
    { "Session parameter",       "Session parameter" },
    { "Startup",                 "Startup" },
    { "TTY",                     "TTY" },
    { "Telnet",                  "Telnet" },
    { "Terminal",                "Terminal" },
    { "Terminal details",        "Terminal details" },
    { "Title",                   "Title" },
    { "Translation",             "Translation" },
    { "Transparency",            "Transparency" },
    { "Tunnels",                 "Tunnels" },
    { "Updates",                 "Updates" },
    { "WinSCP",                  "WinSCP" },
    { "Window",                  "Window" },
    { "Workplace proxy",         "Workplace proxy" },
    { "X11",                     "X11" },
    { "ZModem",                  "ZModem" },
};

static inline const char *kitty_tree_label(const char *component)
{
    for (size_t i = 0; i < sizeof(kitty_tree_labels)/sizeof(*kitty_tree_labels); i++)
        if (!strcmp(kitty_tree_labels[i].component, component))
            return kitty_tree_labels[i].display;
    return component;
}
