/*
 * kitty_scriptbridge.h - the script bridge (kitty_scriptbridge.c): another
 * program drives this terminal's script engine over WM_COPYDATA.
 */
#ifndef KITTY_SCRIPTBRIDGE_H
#define KITTY_SCRIPTBRIDGE_H

#include <windows.h>
#include "putty.h"

/* [KiTTY] scriptbridge=no|inbound|prompt */
enum { KSB_LEVEL_NO = 0, KSB_LEVEL_INBOUND = 1, KSB_LEVEL_PROMPT = 2 };

/* The wire: dwData of WM_COPYDATA. 1-4 are the broadcast's (window.c). */
#define KSB_ID_SEND     5       /* in:  "<key>\0<script>" */
#define KSB_ID_ENABLE   6       /* in:  "<key>\0" "1" = bind wParam, "0" = unbind */
#define KSB_ID_SET      7       /* in:  "<key>\0<pattern>", empty = cleared */
#define KSB_ID_PROMPT   8       /* out: the matched pattern, wParam = sender */
#define KSB_ID_LAST     10      /* 9, 10: session output, not built */

#define KSB_INBOUND_MAX     65536   /* a message, all of it: larger is refused */
#define KSB_PATTERN_MAX     255     /* the prompt pattern */
#define KSB_QUEUE_MAX       262144  /* _prompt messages waiting to be sent */
#define KSB_SEND_TIMEOUT_MS 1000    /* the target's time to answer one */

int  kitty_sbridge_level(void);
void kitty_sbridge_set_level(int level);
void kitty_sbridge_set_level_text(const char *value);   /* the kitty.ini word */

/* The installation's script bridge key: [KiTTY] scriptbridgekey, else one
 * derived from the installation's location ("sb-<hash>", never the
 * broadcast's "auto-<hash>"). A session's own key (ScriptBridgeKey) wins
 * over it. The set call follows a change of the store in this process. */
const char *kitty_sbridge_install_key(void);
int  kitty_sbridge_install_key_stored(void);   /* 1: from the store */
void kitty_sbridge_set_install_key(const char *k);
char *kitty_sbridge_new_key_dup(void);         /* a random key (sfree it), NULL: none */

/* One message of a bridge id, with what the terminal knows about itself. */
struct kitty_sbridge_msg {
    const COPYDATASTRUCT *cds;
    HWND self;                  /* this terminal's window */
    HWND sender;                /* wParam: the window _enable binds */
    Conf *conf;                 /* the session: script settings and the arm */
    Backend *backend;           /* NULL while no session runs */
    LogContext *logctx;         /* the Terminal Window's Event Log */
    const char *group;          /* the key this session listens for */
    int armed;                  /* AcceptScriptBridge */
};
int  kitty_sbridge_is_id(ULONG_PTR id);
LRESULT kitty_sbridge_copydata(const struct kitty_sbridge_msg *msg);

/* The prompt observer (win_seat_output): bound to a target with a pattern. */
int  kitty_sbridge_bound(void);
void kitty_sbridge_remote(const void *data, size_t len);
/* Ends a binding with the reason in the Event Log; nothing when unbound. */
void kitty_sbridge_unbind(const char *why);

#endif /* KITTY_SCRIPTBRIDGE_H */
