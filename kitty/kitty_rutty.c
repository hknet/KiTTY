/*
 * KiTTY rutty scripting for PuTTY 0.84 (no-global, window.c-side).
 *
 * Original feature: rutty (Ernst Dijk) script engine, carried by KiTTY.  It
 * sends a script file line by line, and - in "wait for prompt" mode - waits for
 * a configured pattern (waitfor) in the INCOMING terminal data before sending
 * the next line, and aborts if a "halton" pattern appears.
 *
 * In rutty/KiTTY the incoming-data matching (script_remote) was driven from
 * terminal.c's term_data.  Per the 0.84 port rule (no terminal.c edits) we hook
 * it from windows/window.c win_seat_output() instead - the same interception
 * point ZModem uses (kitty_zmodem.c) - but OBSERVE-only: the data still flows on
 * to term_data().
 *
 * This is a minimal port: it keeps the rutty matching/line-stepping core
 * (script_cond_set / script_cond_chk / find/get/chk line) verbatim, drops the
 * recording + AHK + menu UI sides, and sends via backend_send (no global ldisc),
 * which is sufficient for the send + waitfor/halton functionality.
 *
 * Public API (called from window.c):
 *   int  kitty_script_active(void);
 *   int  kitty_script_send_file(Conf *conf, Backend *backend, Filename *fn);
 *   void kitty_script_remote(const void *data, size_t len);  // observe-only
 *   void kitty_script_stop(void);
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"

#define script_line_size 4096
#define script_cond_size 256

/* script cr/lf translation */
enum { SCRIPT_OFF, SCRIPT_NOLF, SCRIPT_CR, SCRIPT_REC };

typedef struct {
    int line_delay;            /* ticks */
    int char_delay;            /* ticks */
    char cond_char;            /* condition/remark start character */
    int cond_use;              /* use condition from file */
    int enable;                /* wait for host response */
    int except;                /* except firstline */
    int timeout;               /* ticks */
    int crlf;                  /* cr/lf translation */
    char waitfor[script_cond_size];
    int waitfor_c;
    char halton[script_cond_size];
    int halton_c;

    char waitfor2[script_cond_size];
    int waitfor2_c;
    int runs;
    int send;

    char *filebuffer;
    char *nextnextline;
    char *filebuffer_end;
    unsigned long latest;

    char *nextline;
    int nextline_c;
    int nextline_cc;
    char remotedata[script_line_size];
    int remotedata_c;

    Backend *backend;          /* no-global: send target */
} ScriptData;

static ScriptData the_script;
static int script_inited = 0;

/* ---- timer callbacks (forward) ---- */
static void script_sendline(void *ctx, unsigned long now);
static void script_sendchar(void *ctx, unsigned long now);
static void script_timeout(void *ctx, unsigned long now);
static int  script_chkline(ScriptData *s);
static void script_getline(ScriptData *s);

/* ---- minimal send helper (backend, not ldisc) ---- */
static void script_emit(ScriptData *s, const char *buf, int len)
{
    if (s->backend && len > 0)
        backend_send(s->backend, buf, (size_t)len);
}

/* copy condition from settings to scriptdata (rutty script_cond_set verbatim) */
static void script_cond_set(char *cond, int *p, const char *in, int sz)
{
    int i = 0;
    (*p) = 0;

    while (sz > 0 && (in[sz-1] == '\n' || in[sz-1] == '\r'))
        sz--;

    if (sz == 0) {
        cond[*p] = '\0';
    } else if (in[0] != '"') {
        if (sz > (script_cond_size - 1))
            i = sz - (script_cond_size - 1);
        cond[(*p)++] = '\0';
        while (i < sz)
            cond[(*p)++] = in[i++];
    } else {
        if (sz > script_cond_size)
            sz = script_cond_size;
        i++;                               /* skip starting " */
        while (i < sz) {
            cond[(*p)++] = '\0';
            while (i < sz && in[i] != '"')
                cond[(*p)++] = in[i++];
            i++;
            while (i < sz && in[i] == ' ')
                i++;
            while (i < sz && in[i] == '"')
                i++;
        }
    }
}

/* compare received 'data' with condition list 'ref' (rutty verbatim) */
static int script_cond_chk(char *ref, int rc, char *data, int dc)
{
    int rcc = rc;
    int dcc = dc;

    while (rcc > 0 && dcc > 0) {
        do {
            rcc--;
            dcc--;
        } while (rcc >= 0 && dcc >= 0 && ref[rcc] != '\0' && ref[rcc] == data[dcc]);

        if (ref[rcc] == '\0')
            return true;

        dcc = dc;
        while (rcc > 0 && ref[--rcc] != '\0')
            ;
    }
    return false;
}

static int script_findline(ScriptData *s)
{
    if (s->filebuffer == NULL)
        return false;
    if (s->nextnextline >= s->filebuffer_end)
        return false;

    s->nextline = s->nextnextline;
    s->nextline_c = 0;
    while (s->nextnextline < s->filebuffer_end && s->nextnextline[0] != '\n') {
        s->nextnextline++;
        s->nextline_c++;
    }
    if (s->nextnextline < s->filebuffer_end) {
        s->nextnextline++;
        s->nextline_c++;
    }
    return true;
}

static void script_getline(ScriptData *s)
{
    int neof;
    int i;

    if (!s->runs || s->filebuffer == NULL)
        return;

    do {
        do
            neof = script_findline(s);
        while (neof && ((!s->cond_use && s->nextline[0] == s->cond_char) ||
                        (s->cond_use && s->nextline[0] == s->cond_char &&
                         s->nextline[1] == s->cond_char)));
        if (!neof) {
            s->nextline_c = 0;
            s->nextline_cc = 0;
            return;
        }

        i = s->nextline_c;
        switch (s->crlf) {
          case SCRIPT_OFF:
            break;
          case SCRIPT_NOLF:
            if (s->nextline[i-1] == '\n')
                i--;
            break;
          case SCRIPT_CR:
            if (s->nextline[i-1] == '\n')
                i--;
            if (i > 0 && s->nextline[i-1] == '\r')
                i--;
            s->nextline[i++] = '\r';
            break;
          default:
            break;
        }
    } while (i == 0);                      /* skip empty lines */
    s->nextline_c = i;
    s->nextline_cc = 0;
}

static int script_chkline(ScriptData *s)
{
    if (s->nextline_c > 0 && s->nextline[0] == s->cond_char) {
        script_cond_set(s->waitfor2, &s->waitfor2_c,
                        &s->nextline[1], s->nextline_c - 1);
        script_getline(s);
        return true;
    } else {
        s->waitfor2_c = -1;
        s->waitfor2[0] = '\0';
    }
    return false;
}

static void script_stop_internal(ScriptData *s)
{
    s->runs = false;
    expire_timer_context(s);
    s->latest = 0;
    if (s->filebuffer != NULL) {
        sfree(s->filebuffer);
        s->filebuffer = NULL;
    }
}

void kitty_script_stop(void)
{
    if (script_inited)
        script_stop_internal(&the_script);
}

static void script_setsend(ScriptData *s)
{
    s->latest = 0;
    if (s->nextline_c == 0) {
        script_stop_internal(s);
        logevent(NULL, "... finished sending script");
        return;
    }
    if (script_chkline(s)) {               /* new condition - restart timeout */
        s->send = false;
        s->latest = schedule_timer(s->timeout, script_timeout, s);
    } else {                               /* data - set line delay timer */
        s->send = true;
        schedule_timer(s->line_delay, script_sendline, s);
    }
}

static void script_sendline(void *ctx, unsigned long now)
{
    ScriptData *s = (ScriptData *)ctx;
    if (!s->runs)
        return;
    if (s->nextline_c == 0) {
        script_stop_internal(s);
        logevent(NULL, " ...finished sending script");
        return;
    }
    if (s->char_delay > 1) {
        schedule_timer(s->char_delay, script_sendchar, s);
        return;
    }
    if (s->char_delay == 0) {
        script_emit(s, s->nextline, s->nextline_c);
    } else {
        int i;
        for (i = 0; i < s->nextline_c; i++)
            script_emit(s, &s->nextline[i], 1);
    }
    script_getline(s);
    script_chkline(s);
    if (s->enable) {
        s->send = false;
        s->latest = schedule_timer(s->timeout, script_timeout, s);
    } else {
        schedule_timer(s->line_delay, script_sendline, s);
    }
}

static void script_sendchar(void *ctx, unsigned long now)
{
    ScriptData *s = (ScriptData *)ctx;
    if (!s->runs)
        return;
    if (s->nextline_c == 0) {
        script_stop_internal(s);
        logevent(NULL, "....finished sending script");
        return;
    }
    if (s->nextline_cc < s->nextline_c)
        script_emit(s, &s->nextline[s->nextline_cc++], 1);
    if (s->nextline_cc < s->nextline_c) {
        schedule_timer(s->char_delay, script_sendchar, s);
        return;
    }
    script_getline(s);
    script_chkline(s);
    if (s->enable) {
        s->send = false;
        s->latest = schedule_timer(s->timeout, script_timeout, s);
    } else {
        schedule_timer(s->line_delay, script_sendline, s);
    }
}

static void script_timeout(void *ctx, unsigned long now)
{
    ScriptData *s = (ScriptData *)ctx;
    if (labs((long)(now - s->latest)) < 50) {
        script_stop_internal(s);
        logevent(NULL, "script timeout !");
    }
}

/* ---- public: start a script ---- */
int kitty_script_active(void)
{
    return script_inited && the_script.runs;
}

int kitty_script_send_file(Conf *conf, Backend *backend, Filename *scriptfile)
{
    ScriptData *s = &the_script;
    FILE *fp;
    long fsize;
    const char *cc;

    if (script_inited && s->runs)
        return false;                      /* a script is already running */

    memset(s, 0, sizeof(*s));
    script_inited = 1;
    s->backend = backend;

    /* script_init: settings from conf */
    s->line_delay = conf_get_int(conf, CONF_script_line_delay);
    if (s->line_delay < 5) s->line_delay = 5;
    s->line_delay = s->line_delay * TICKSPERSEC / 1000;
    s->char_delay = conf_get_int(conf, CONF_script_char_delay) * TICKSPERSEC / 1000;
    cc = conf_get_str(conf, CONF_script_cond_line);
    s->cond_char = cc[0] ? cc[0] : ':';
    s->enable = conf_get_int(conf, CONF_script_enable);
    s->cond_use = s->enable ? conf_get_int(conf, CONF_script_cond_use) : false;
    s->except = conf_get_int(conf, CONF_script_except);
    s->timeout = conf_get_int(conf, CONF_script_timeout) * TICKSPERSEC;
    {
        const char *w = conf_get_str(conf, CONF_script_waitfor);
        const char *h = conf_get_str(conf, CONF_script_halton);
        script_cond_set(s->waitfor, &s->waitfor_c, w, (int)strlen(w));
        script_cond_set(s->halton, &s->halton_c, h, (int)strlen(h));
    }
    s->crlf = conf_get_int(conf, CONF_script_crlf);
    s->waitfor2[0] = '\0';
    s->waitfor2_c = -1;
    s->remotedata_c = script_cond_size;
    s->remotedata[0] = '\0';

    /* script_sendfile: read whole file */
    fp = f_open(scriptfile, "rb", false);
    if (fp == NULL) {
        logevent(NULL, "script file not found");
        return false;
    }
    s->runs = true;
    fseek(fp, 0L, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0L, SEEK_SET);
    if (fsize <= 0) { fclose(fp); script_stop_internal(s); return false; }
    s->nextnextline = s->filebuffer = snewn(fsize, char);
    s->filebuffer_end = &s->filebuffer[fsize];
    if (fread(s->filebuffer, sizeof(char), fsize, fp) != (size_t)fsize) {
        logevent(NULL, "script file read failed");
        fclose(fp);
        script_stop_internal(s);
        return false;
    }
    fclose(fp);
    logevent(NULL, "sending script to host ...");

    script_getline(s);
    script_chkline(s);

    if (s->enable && !s->except) {         /* wait-for-prompt before first line */
        s->send = false;
        s->latest = schedule_timer(s->timeout, script_timeout, s);
    } else {
        s->send = true;
        schedule_timer(s->line_delay, script_sendline, s);
    }
    return true;
}

/* ---- public: observe incoming host data (waitfor/halton) ---- */
void kitty_script_remote(const void *vdata, size_t len)
{
    ScriptData *s = &the_script;
    const char *data = (const char *)vdata;
    size_t i;

    if (!script_inited || !s->runs)
        return;

    for (i = 0; i < len; i++) {
        if (data[i] == '\n' || data[i] == '\r' || data[i] == '\0') {
            s->remotedata_c = script_cond_size;
            s->remotedata[s->remotedata_c] = '\0';
        } else {
            if (s->remotedata_c >= script_line_size) {
                int j = script_line_size - script_cond_size;
                s->remotedata_c = 0;
                while (s->remotedata_c < script_cond_size)
                    s->remotedata[s->remotedata_c++] = s->remotedata[j++];
            }
            s->remotedata[s->remotedata_c++] = data[i];
        }

        if (s->runs) {
            /* halton */
            if (s->halton_c > 0 &&
                script_cond_chk(s->halton, s->halton_c,
                                s->remotedata, s->remotedata_c)) {
                script_stop_internal(s);
                logevent(NULL, "script halted");
                return;
            }
            /* waitfor (prompt to send the next line) */
            if (s->enable && !s->send) {
                if (s->waitfor2_c >= 0) {
                    if (s->waitfor2_c == 0 ||
                        script_cond_chk(s->waitfor2, s->waitfor2_c,
                                        s->remotedata, s->remotedata_c))
                        script_setsend(s);
                } else if (s->waitfor_c == 0 ||
                           script_cond_chk(s->waitfor, s->waitfor_c,
                                           s->remotedata, s->remotedata_c)) {
                    script_setsend(s);
                }
            }
        }
    }
}
