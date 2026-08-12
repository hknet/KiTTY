/*
 * Session logging.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include <time.h>
#include <assert.h>

#include "putty.h"

/* log session to file stuff ... */
struct LogContext {
    FILE *lgfp;
    enum { L_CLOSED, L_OPENING, L_OPEN, L_ERROR } state;
    bufchain queue;
    Filename *currlogfilename;
    LogPolicy *lp;
    Conf *conf;
    int logtype;                       /* cached out of conf */
#ifdef MOD_PERSO
    /* KiTTY: LogTimestamp writes the configured stamp at the start of every
     * logged line. Per CONTEXT, not a file-scope flag: several sessions can be
     * logging at once and a shared one would stamp whichever session happened
     * to end the previous line. */
    bool at_line_start;
    /* LogTimestamp pattern already reported as unusable - report once per log,
     * not once per line. */
    bool ts_warned;
#endif
};

static Filename *xlatlognam(const Filename *s,
                            const char *hostname, int port,
                            const struct tm *tm);

#ifdef MOD_PERSO
#include <sys/time.h>                  /* gettimeofday, for the %f expansion */

/*
 * KiTTY: expand the LogTimestamp pattern (Session > Logging).
 *
 * strftime does the work; we only pre-expand %f (milliseconds), which strftime
 * has no format for and which the KiTTY this port replaces supported. %% is
 * passed through untouched so an escaped percent cannot be mistaken for %f.
 *
 * Returns the length written, or 0 if there is nothing to write - an empty
 * pattern means the feature is off, which is the default.
 */
static size_t kitty_log_timestamp_text(const char *fmt, char *out, size_t outlen)
{
    char expanded[512];
    size_t o = 0;
    struct tm tm;
    long ms = 0;

    if (!fmt || !*fmt || outlen < 2)
        return 0;

    {
        struct timeval tv;
        /* gettimeofday, not C11's timespec_get: this builds as gnu99, where
         * timespec_get and TIME_UTC do not exist. A failure here costs only
         * the milliseconds, so there is nothing to report. */
        if (gettimeofday(&tv, NULL) == 0)
            ms = tv.tv_usec / 1000L;
    }

    for (const char *p = fmt; *p && o + 4 < sizeof(expanded); p++) {
        if (p[0] == '%' && p[1] == 'f') {
            o += snprintf(expanded + o, sizeof(expanded) - o, "%03ld", ms);
            p++;
        } else if (p[0] == '%' && p[1] == '%') {
            expanded[o++] = *p++;      /* copy both, leave %% to strftime */
            expanded[o++] = *p;
        } else {
            expanded[o++] = *p;
        }
    }
    expanded[o] = '\0';

    tm = ltime();
    return strftime(out, outlen, expanded, &tm);
}
#endif

/*
 * Internal wrapper function which must be called for _all_ output
 * to the log file. It takes care of opening the log file if it
 * isn't open, buffering data if it's in the process of being
 * opened asynchronously, etc.
 */
static void logwrite(LogContext *ctx, ptrlen data)
{
    /*
     * In state L_CLOSED, we call logfopen, which will set the state
     * to one of L_OPENING, L_OPEN or L_ERROR. Hence we process all of
     * those three _after_ processing L_CLOSED.
     */
    if (ctx->state == L_CLOSED)
        logfopen(ctx);

    if (ctx->state == L_OPENING) {
        bufchain_add(&ctx->queue, data.ptr, data.len);
    } else if (ctx->state == L_OPEN) {
        assert(ctx->lgfp);
        if (fwrite(data.ptr, 1, data.len, ctx->lgfp) < data.len) {
            logfclose(ctx);
            ctx->state = L_ERROR;
            lp_eventlog(ctx->lp, "Disabled writing session log "
                        "due to error while writing");
        }
    }                                  /* else L_ERROR, so ignore the write */
}

/*
 * Convenience wrapper on logwrite() which printf-formats the
 * string.
 */
static PRINTF_LIKE(2, 3) void logprintf(LogContext *ctx, const char *fmt, ...)
{
    va_list ap;
    char *data;

    va_start(ap, fmt);
    data = dupvprintf(fmt, ap);
    va_end(ap);

    logwrite(ctx, ptrlen_from_asciz(data));
    sfree(data);
}

/*
 * Flush any open log file.
 */
void logflush(LogContext *ctx)
{
    if (ctx->logtype > 0)
        if (ctx->state == L_OPEN)
            fflush(ctx->lgfp);
}

LogPolicy *log_get_policy(LogContext *ctx)
{
    return ctx->lp;
}

static void logfopen_callback(void *vctx, int mode)
{
    LogContext *ctx = (LogContext *)vctx;
    char buf[256], *event;
    struct tm tm;
    const char *fmode;
    bool shout = false;

    if (mode == 0) {
        ctx->state = L_ERROR;          /* disable logging */
    } else {
        fmode = (mode == 1 ? "ab" : "wb");
        ctx->lgfp = f_open(ctx->currlogfilename, fmode, false);
        if (ctx->lgfp) {
            ctx->state = L_OPEN;
            /* KiTTY: deliberately NOT re-arming the LogTimestamp line latch
             * here. Output that arrives while the file is still opening is
             * queued, so by this point we may be in the MIDDLE of a line;
             * re-arming stamped the next character and split the first line
             * as "<stamp>a<stamp>lpha". log_init() arms it once, which is all
             * the first line needs. */
        } else {
            ctx->state = L_ERROR;
            shout = true;
        }
    }

    if (ctx->state == L_OPEN && conf_get_bool(ctx->conf, CONF_logheader)) {
        /* Write header line into log file. */
        tm = ltime();
        strftime(buf, 24, "%Y.%m.%d %H:%M:%S", &tm);
        logprintf(ctx, "=~=~=~=~=~=~=~=~=~=~=~= PuTTY log %s"
                  " =~=~=~=~=~=~=~=~=~=~=~=\r\n", buf);
    }

    event = dupprintf("%s session log (%s mode) to file: %s",
                      ctx->state == L_ERROR ?
                      (mode == 0 ? "Disabled writing" : "Error writing") :
                      (mode == 1 ? "Appending" : "Writing new"),
                      (ctx->logtype == LGTYP_ASCII ? "ASCII" :
                       ctx->logtype == LGTYP_DEBUG ? "raw" :
                       ctx->logtype == LGTYP_PACKETS ? "SSH packets" :
                       ctx->logtype == LGTYP_SSHRAW ? "SSH raw data" :
                       "unknown"),
                      filename_to_str(ctx->currlogfilename));
    lp_eventlog(ctx->lp, event);
    if (shout) {
        /*
         * If we failed to open the log file due to filesystem error
         * (as opposed to user action such as clicking Cancel in the
         * askappend box), we should log it more prominently.
         */
        lp_logging_error(ctx->lp, event);
    }
    sfree(event);

    /*
     * Having either succeeded or failed in opening the log file,
     * we should write any queued data out.
     */
    assert(ctx->state != L_OPENING);   /* make _sure_ it won't be requeued */
    while (bufchain_size(&ctx->queue)) {
        ptrlen data = bufchain_prefix(&ctx->queue);
        logwrite(ctx, data);
        bufchain_consume(&ctx->queue, data.len);
    }
    logflush(ctx);
}

/*
 * Open the log file. Takes care of detecting an already-existing
 * file and asking the user whether they want to append, overwrite
 * or cancel logging.
 */
void logfopen(LogContext *ctx)
{
    struct tm tm;
    int mode;

    /* Prevent repeat calls */
    if (ctx->state != L_CLOSED)
        return;

    if (!ctx->logtype)
        return;

    tm = ltime();

    /* substitute special codes in file name */
    if (ctx->currlogfilename)
        filename_free(ctx->currlogfilename);
    ctx->currlogfilename =
        xlatlognam(conf_get_filename(ctx->conf, CONF_logfilename),
                   conf_dest(ctx->conf),    /* hostname or serial line */
                   conf_get_int(ctx->conf, CONF_port), &tm);

    if (open_for_write_would_lose_data(ctx->currlogfilename)) {
        int logxfovr = conf_get_int(ctx->conf, CONF_logxfovr);
        if (logxfovr != LGXF_ASK) {
            mode = ((logxfovr == LGXF_OVR) ? 2 : 1);
        } else
            mode = lp_askappend(ctx->lp, ctx->currlogfilename,
                                logfopen_callback, ctx);
    } else
        mode = 2;                      /* create == overwrite */

    if (mode < 0)
        ctx->state = L_OPENING;
    else
        logfopen_callback(ctx, mode);  /* open the file */
}

#ifdef MOD_PERSO
/*
 * KiTTY: log rotation (Session > Logging > "Log rotation delay").
 *
 * Closing the file is the whole mechanism: the next write reopens it, and
 * logfopen() re-substitutes the &-codes in the name against the time of day,
 * so &T (or &D over midnight) yields a new file.
 *
 * It REFUSES to rotate when the name would not change. Reopening the same name
 * with "always overwrite" opens it "wb" and truncates it, so a rotation delay
 * on a fixed filename would silently destroy the log every N seconds - turning
 * a convenience into data loss. Rotation needs a time-varying name; the config
 * dialog says so, and the event log says so here if it is ever asked to.
 */
void logfile_rotate(LogContext *ctx)
{
    struct tm tm;
    Filename *newname;
    bool unchanged;

    if (!ctx || ctx->state != L_OPEN || !ctx->currlogfilename)
        return;

    tm = ltime();
    newname = xlatlognam(conf_get_filename(ctx->conf, CONF_logfilename),
                         conf_dest(ctx->conf),
                         conf_get_int(ctx->conf, CONF_port), &tm);
    unchanged = filename_equal(newname, ctx->currlogfilename);
    filename_free(newname);

    if (unchanged) {
        logevent(ctx, "Log rotation skipped: the log file name does not "
                 "change with time (use &T, or &Y&M&D, in it) - rotating "
                 "into the same name would overwrite the log");
        return;
    }

    logfclose(ctx);                    /* the next write reopens it */
    ctx->at_line_start = true;
}
#endif

void logfclose(LogContext *ctx)
{
    if (ctx->lgfp) {
        fclose(ctx->lgfp);
        ctx->lgfp = NULL;
    }
    ctx->state = L_CLOSED;
}

#ifdef MOD_PERSO
/*
 * KiTTY: write the stamp if we are at the start of a logged line. Session logs
 * only (see logtraffic) - the packet and SSH-raw logs already timestamp every
 * record of their own accord, and stamping those again would corrupt a format
 * other tools parse.
 */
static void kitty_log_timestamp(LogContext *ctx)
{
    char buf[512];
    size_t len;
    const char *fmt;

    if (!ctx->at_line_start)
        return;
    ctx->at_line_start = false;

    fmt = conf_get_str(ctx->conf, CONF_logtimestamp);
    if (!fmt || !*fmt)
        return;                        /* feature off - the default */

    len = kitty_log_timestamp_text(fmt, buf, sizeof(buf));
    if (len) {
        logwrite(ctx, make_ptrlen(buf, len));
        return;
    }

    /*
     * strftime refused the pattern (an unknown %-code, a trailing '%'), or the
     * result did not fit. Nothing is written and the log itself is unharmed -
     * but say so ONCE, because a field that is set and silently does nothing
     * is the very confusion this option was fixed to end.
     */
    if (!ctx->ts_warned) {
        ctx->ts_warned = true;
        logevent(ctx, "Log timestamp: that strftime pattern produces nothing, "
                 "so lines are not being stamped - check the format in "
                 "Session > Logging");
    }
}
#endif

/*
 * Log session traffic.
 */
void logtraffic(LogContext *ctx, unsigned char c, int logmode)
{
    if (ctx->logtype > 0) {
        if (ctx->logtype == logmode) {
#ifdef MOD_PERSO
            /* KiTTY: LogTimestamp. Before the character, so the stamp opens
             * the line; the newline itself stays on the line it ends. */
            kitty_log_timestamp(ctx);
            if (c == '\n')
                ctx->at_line_start = true;
#endif
            logwrite(ctx, make_ptrlen(&c, 1));
        }
    }
}

static void logevent_internal(LogContext *ctx, const char *event)
{
    if (ctx->logtype == LGTYP_PACKETS || ctx->logtype == LGTYP_SSHRAW) {
        logprintf(ctx, "Event Log: %s\r\n", event);
        logflush(ctx);
    }
    lp_eventlog(ctx->lp, event);
}

void logevent(LogContext *ctx, const char *event)
{
    if (!ctx)
        return;

    /*
     * Replace newlines in Event Log messages with spaces. (Sometimes
     * the same message string is reused for the Event Log and a GUI
     * dialog box; newlines are sometimes appropriate in the latter,
     * but never in the former.)
     */
    if (strchr(event, '\n') || strchr(event, '\r')) {
        char *dup = dupstr(event);
        char *p = dup, *q = dup;
        while (*p) {
            if (*p == '\r' || *p == '\n') {
                do {
                    p++;
                } while (*p == '\r' || *p == '\n');
                *q++ = ' ';
            } else {
                *q++ = *p++;
            }
        }
        *q = '\0';
        logevent_internal(ctx, dup);
        sfree(dup);
    } else {
        logevent_internal(ctx, event);
    }
}

/*
 * Log an SSH packet.
 * If n_blanks != 0, blank or omit some parts.
 * Set of blanking areas must be in increasing order.
 */
void log_packet(LogContext *ctx, int direction, int type,
                const char *texttype, const void *data, size_t len,
                int n_blanks, const struct logblank_t *blanks,
                const unsigned long *seq,
                unsigned downstream_id, const char *additional_log_text)
{
    char dumpdata[128], smalldata[5];
    size_t p = 0, b = 0, omitted = 0;
    int output_pos = 0; /* NZ if pending output in dumpdata */

    if (!(ctx->logtype == LGTYP_SSHRAW ||
          (ctx->logtype == LGTYP_PACKETS && texttype)))
        return;

    /* Packet header. */
    if (texttype) {
        logprintf(ctx, "%s packet ",
                  direction == PKT_INCOMING ? "Incoming" : "Outgoing");

        if (seq)
            logprintf(ctx, "#0x%lx, ", *seq);

        logprintf(ctx, "type %d / 0x%02x (%s)", type, type, texttype);

        if (downstream_id) {
            logprintf(ctx, " on behalf of downstream #%u", downstream_id);
            if (additional_log_text)
                logprintf(ctx, " (%s)", additional_log_text);
        }

        logprintf(ctx, "\r\n");
    } else {
        /*
         * Raw data is logged with a timestamp, so that it's possible
         * to determine whether a mysterious delay occurred at the
         * client or server end. (Timestamping the raw data avoids
         * cluttering the normal case of only logging decrypted SSH
         * messages, and also adds conceptual rigour in the case where
         * an SSH message arrives in several pieces.)
         */
        char buf[256];
        struct tm tm;
        tm = ltime();
        strftime(buf, 24, "%Y-%m-%d %H:%M:%S", &tm);
        logprintf(ctx, "%s raw data at %s\r\n",
                  direction == PKT_INCOMING ? "Incoming" : "Outgoing",
                  buf);
    }

    /*
     * Output a hex/ASCII dump of the packet body, blanking/omitting
     * parts as specified.
     */
    while (p < len) {
        int blktype;

        /* Move to a current entry in the blanking array. */
        while ((b < n_blanks) &&
               (p >= blanks[b].offset + blanks[b].len))
            b++;
        /* Work out what type of blanking to apply to
         * this byte. */
        blktype = PKTLOG_EMIT; /* default */
        if ((b < n_blanks) &&
            (p >= blanks[b].offset) &&
            (p < blanks[b].offset + blanks[b].len))
            blktype = blanks[b].type;

        /* If we're about to stop omitting, it's time to say how
         * much we omitted. */
        if ((blktype != PKTLOG_OMIT) && omitted) {
            logprintf(ctx, "  (%"SIZEu" byte%s omitted)\r\n",
                      omitted, (omitted==1?"":"s"));
            omitted = 0;
        }

        /* (Re-)initialise dumpdata as necessary
         * (start of row, or if we've just stopped omitting) */
        if (!output_pos && !omitted)
            sprintf(dumpdata, "  %08"SIZEx"%*s\r\n",
                    p-(p%16), 1+3*16+2+16, "");

        /* Deal with the current byte. */
        if (blktype == PKTLOG_OMIT) {
            omitted++;
        } else {
            int c;
            if (blktype == PKTLOG_BLANK) {
                c = 'X';
                sprintf(smalldata, "XX");
            } else {  /* PKTLOG_EMIT */
                c = ((const unsigned char *)data)[p];
                sprintf(smalldata, "%02x", c);
            }
            dumpdata[10+2+3*(p%16)] = smalldata[0];
            dumpdata[10+2+3*(p%16)+1] = smalldata[1];
            dumpdata[10+1+3*16+2+(p%16)] = (c >= 0x20 && c < 0x7F ? c : '.');
            output_pos = (p%16) + 1;
        }

        p++;

        /* Flush row if necessary */
        if (((p % 16) == 0) || (p == len) || omitted) {
            if (output_pos) {
                strcpy(dumpdata + 10+1+3*16+2+output_pos, "\r\n");
                logwrite(ctx, ptrlen_from_asciz(dumpdata));
                output_pos = 0;
            }
        }

    }

    /* Tidy up */
    if (omitted)
        logprintf(ctx, "  (%"SIZEu" byte%s omitted)\r\n",
                  omitted, (omitted==1?"":"s"));
    logflush(ctx);
}

LogContext *log_init(LogPolicy *lp, Conf *conf)
{
    LogContext *ctx = snew(LogContext);
    ctx->lgfp = NULL;
    ctx->state = L_CLOSED;
    ctx->lp = lp;
    ctx->conf = conf_copy(conf);
    ctx->logtype = conf_get_int(ctx->conf, CONF_logtype);
    ctx->currlogfilename = NULL;
#ifdef MOD_PERSO
    ctx->at_line_start = true;         /* KiTTY: stamp the first line too */
    ctx->ts_warned = false;
#endif
    bufchain_init(&ctx->queue);
    return ctx;
}

void log_free(LogContext *ctx)
{
    logfclose(ctx);
    bufchain_clear(&ctx->queue);
    if (ctx->currlogfilename)
        filename_free(ctx->currlogfilename);
    conf_free(ctx->conf);
    sfree(ctx);
}

void log_reconfig(LogContext *ctx, Conf *conf)
{
    bool reset_logging;

    if (!filename_equal(conf_get_filename(ctx->conf, CONF_logfilename),
                        conf_get_filename(conf, CONF_logfilename)) ||
        conf_get_int(ctx->conf, CONF_logtype) !=
        conf_get_int(conf, CONF_logtype))
        reset_logging = true;
    else
        reset_logging = false;

    if (reset_logging)
        logfclose(ctx);

    conf_free(ctx->conf);
    ctx->conf = conf_copy(conf);

    ctx->logtype = conf_get_int(ctx->conf, CONF_logtype);

    if (reset_logging)
        logfopen(ctx);
}

/*
 * translate format codes into time/date strings
 * and insert them into log file name
 *
 * "&Y":YYYY   "&m":MM   "&d":DD   "&T":hhmmss   "&h":<hostname>   "&&":&
 */
static Filename *xlatlognam(const Filename *src,
                            const char *hostname, int port,
                            const struct tm *tm)
{
    char buf[32];
    const char *bufp;
    int size;
    strbuf *buffer;
    const char *s;
    Filename *ret;

    buffer = strbuf_new();
    s = filename_to_str(src);

    while (*s) {
        bool sanitise = false;
        /* Let (bufp, len) be the string to append. */
        bufp = buf;                    /* don't usually override this */
        if (*s == '&') {
            char c;
            s++;
            size = 0;
            if (*s) switch (c = *s++, tolower((unsigned char)c)) {
              case 'y':
                size = strftime(buf, sizeof(buf), "%Y", tm);
                break;
              case 'm':
                size = strftime(buf, sizeof(buf), "%m", tm);
                break;
              case 'd':
                size = strftime(buf, sizeof(buf), "%d", tm);
                break;
              case 't':
                size = strftime(buf, sizeof(buf), "%H%M%S", tm);
                break;
              case 'h':
                bufp = hostname;
                size = strlen(bufp);
                break;
              case 'p':
                size = sprintf(buf, "%d", port);
                break;
              default:
                buf[0] = '&';
                size = 1;
                if (c != '&')
                    buf[size++] = c;
            }
            /* Never allow path separators - or any other illegal
             * filename character - to come out of any of these
             * auto-format directives. E.g. 'hostname' can contain
             * colons, if it's an IPv6 address, and colons aren't
             * legal in filenames on Windows. */
            sanitise = true;
        } else {
            buf[0] = *s++;
            size = 1;
        }
        while (size-- > 0) {
            char c = *bufp++;
            if (sanitise)
                c = filename_char_sanitise(c);
            put_byte(buffer, c);
        }
    }

    ret = filename_from_str(buffer->s);
    strbuf_free(buffer);
    return ret;
}
