/*
 * kitty_ctx3008.h - OSC 3008 hierarchical context signalling (UAPI.15): the
 * platform-free half. The far end says which program has control and how
 * programs nest - a shell, a command, run0 taking root, a container, a VM.
 * Parsing, the limits and the context stack; no Win32 and no PuTTY types, so
 * test/test_ctx3008.c covers all of it without a window.
 *
 * OSC 3008 ; start=<id> [;field=value]... ST opens a context: it becomes the
 *     active one, the context active before it its parent. A start for an id
 *     already open updates that context instead: its fields are reset to what
 *     this start says, and every context opened inside it ends.
 * OSC 3008 ; end=<id> [;exit=..;status=..;signal=..] ST ends that context
 *     and every context opened inside it. An id not open is ignored.
 *
 * Lenient, as the specification asks: an unknown or malformed field is
 * ignored and the others kept. A start beyond CTX_DEPTH_MAX is dropped, the
 * contexts already open kept. Values are UTF-8 with ';' sent as \x3b and '\'
 * as \x5c; a field holding a control character, or invalid UTF-8, is
 * ignored. Everything here is what the host CLAIMS: information, never a
 * security signal.
 */
#ifndef KITTY_CTX3008_H
#define KITTY_CTX3008_H

#include <stdbool.h>
#include <stddef.h>

#define CTX_DEPTH_MAX   32       /* contexts open at once */
#define CTX_ID_MAX      64       /* the specification's limit */
#define CTX_FIELD_MAX   255      /* bytes, decoded */
#define CTX_SEQ_MAX     4096     /* the whole string; longer = ignored */
#define CTX_SIGNAL_MAX  31

/* type= */
enum {
    CTX_TYPE_NONE,               /* not sent, or a name not known */
    CTX_TYPE_BOOT, CTX_TYPE_CONTAINER, CTX_TYPE_VM, CTX_TYPE_ELEVATE,
    CTX_TYPE_CHPRIV, CTX_TYPE_SUBCONTEXT, CTX_TYPE_REMOTE, CTX_TYPE_SHELL,
    CTX_TYPE_COMMAND, CTX_TYPE_APP, CTX_TYPE_SERVICE, CTX_TYPE_SESSION,
    CTX_TYPE_COUNT
};

/* the start fields other than type= */
enum {
    CTX_F_USER, CTX_F_HOSTNAME, CTX_F_MACHINEID, CTX_F_BOOTID, CTX_F_PID,
    CTX_F_PIDFDID, CTX_F_COMM, CTX_F_CWD, CTX_F_CMDLINE, CTX_F_VM,
    CTX_F_CONTAINER, CTX_F_TARGETUSER, CTX_F_TARGETHOST, CTX_F_SESSIONID,
    CTX_F_COUNT
};

/* exit= of an end */
enum {
    CTX_EXIT_NONE, CTX_EXIT_SUCCESS, CTX_EXIT_FAILURE, CTX_EXIT_CRASH,
    CTX_EXIT_INTERRUPT
};

typedef struct {
    char id[CTX_ID_MAX + 1];
    int type;                    /* CTX_TYPE_* */
    char *field[CTX_F_COUNT];    /* decoded UTF-8; NULL = not sent */
} CtxContext;

typedef struct CtxStack {          /* terminal.h holds a struct CtxStack * */
    CtxContext ctx[CTX_DEPTH_MAX];   /* [0] outermost, [depth-1] active */
    int depth;
    unsigned long dropped;           /* starts ignored at the depth cap */
} CtxStack;

/* What ctx_apply did with one string. */
#define CTX_IGNORED  0           /* nothing changed */
#define CTX_STARTED  1           /* a new context, now the active one */
#define CTX_UPDATED  2           /* an open one restarted (its children ended) */
#define CTX_ENDED    3           /* an open one ended (its children too) */
#define CTX_DROPPED  4           /* a new context over CTX_DEPTH_MAX */

typedef struct {
    int what;                    /* CTX_* above */
    int children;                /* contexts ended along with it */
    /* CTX_ENDED: the context that ended, now the caller's (ctx_event_free),
     * and what its end said */
    CtxContext ended;
    int exit;                    /* CTX_EXIT_* */
    int status;                  /* 0..255; -1 = not sent */
    char signal[CTX_SIGNAL_MAX + 1];   /* "" = not sent */
} CtxEvent;

void ctx_init(CtxStack *s);
/* Every context gone (a new connection, the terminal freed). */
void ctx_clear(CtxStack *s);
/* One OSC 3008 string, the part after "3008;". ev may be NULL. */
int ctx_apply(CtxStack *s, const char *str, size_t len, CtxEvent *ev);
void ctx_event_free(CtxEvent *ev);

/* The active context, NULL when none is open. */
const CtxContext *ctx_active(const CtxStack *s);
/* The user the active context runs as, as far as the chain says: walking
 * from the outermost context in, a context's targetuser= (where its type
 * has one) or else its user= is the user from there on; an elevate context
 * without targetuser= is root (run0's default). NULL when nothing says. */
const char *ctx_effective_user(const CtxStack *s);
/* ctx_effective_user is root. */
bool ctx_elevated(const CtxStack *s);
/* Who the active context runs as, for the window's background tint:
 * CTX_AS_ROOT when ctx_effective_user is root; CTX_AS_SWITCHED when it is
 * another user that a switch put there - an elevate context's user
 * (run0 -u) or targetuser=, and the contexts further in that keep that
 * user; CTX_AS_PLAIN otherwise (nothing said, or a login as that user, or a
 * context further in with a user= of its own). */
typedef enum { CTX_AS_PLAIN, CTX_AS_ROOT, CTX_AS_SWITCHED } CtxAs;
CtxAs ctx_runs_as(const CtxStack *s);

const char *ctx_type_name(int type);         /* "" for CTX_TYPE_NONE */
const char *ctx_field_name(int field);
const char *ctx_exit_name(int code);         /* "" for CTX_EXIT_NONE */

#endif
