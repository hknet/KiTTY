/*
 * Pageant client code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "putty.h"
#include "pageant.h" /* for AGENT_MAX_MSGLEN */

#include "security-api.h"
#include "cryptoapi.h"

#include "../kitty/kitty_authenticode.h"   /* KiTTY: serving-agent check hook */
#include "../kitty/kitty_oldwin_reg.h"   /* XP: pipe-server pid via oldwin */

/* KiTTY: installed by kitty.exe only (NULL in console tools). Called with the
 * serving agent's pid after a query is answered - observational, never gates
 * the query. The static below keeps it to one real check per process. */
void (*agent_serving_check_hook)(unsigned long server_pid, int transport) = NULL;
static bool agent_serving_checked = false;

static bool wm_copydata_agent_exists(void)
{
    HWND hwnd;
    hwnd = FindWindow("Pageant", "Pageant");
    if (!hwnd)
        return false;
    else
        return true;
}

static void wm_copydata_agent_query(strbuf *query, void **out, int *outlen)
{
    HWND hwnd;
    char *mapname;
    HANDLE filemap;
    unsigned char *p, *ret;
    int id, retlen;
    COPYDATASTRUCT cds;
    SECURITY_ATTRIBUTES sa, *psa;
    PSECURITY_DESCRIPTOR psd = NULL;
    PSID usersid = NULL;

    *out = NULL;
    *outlen = 0;

    if (query->len > AGENT_MAX_MSGLEN)
        return;                        /* query too large */

    hwnd = FindWindow("Pageant", "Pageant");
    if (!hwnd)
        return;                        /* *out == NULL, so failure */
    mapname = dupprintf("PageantRequest%08x", (unsigned)GetCurrentThreadId());

    psa = NULL;
    if (got_advapi()) {
        /*
         * Make the file mapping we create for communication with
         * Pageant owned by the user SID rather than the default. This
         * should make communication between processes with slightly
         * different contexts more reliable: in particular, command
         * prompts launched as administrator should still be able to
         * run PSFTPs which refer back to the owning user's
         * unprivileged Pageant.
         */
        usersid = get_user_sid();

        if (usersid) {
            psd = (PSECURITY_DESCRIPTOR)
                LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
            if (psd) {
                if (p_InitializeSecurityDescriptor(
                        psd, SECURITY_DESCRIPTOR_REVISION) &&
                    p_SetSecurityDescriptorOwner(psd, usersid, false)) {
                    sa.nLength = sizeof(sa);
                    sa.bInheritHandle = true;
                    sa.lpSecurityDescriptor = psd;
                    psa = &sa;
                } else {
                    LocalFree(psd);
                    psd = NULL;
                }
            }
        }
    }

    filemap = CreateFileMapping(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE,
                                0, AGENT_MAX_MSGLEN, mapname);
    if (filemap == NULL || filemap == INVALID_HANDLE_VALUE) {
        sfree(mapname);
        return;                        /* *out == NULL, so failure */
    }
    p = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, 0);
    strbuf_finalise_agent_query(query);
    memcpy(p, query->s, query->len);
    cds.dwData = AGENT_COPYDATA_ID;
    cds.cbData = 1 + strlen(mapname);
    cds.lpData = mapname;

    /*
     * The user either passed a null callback (indicating that the
     * query is required to be synchronous) or CreateThread failed.
     * Either way, we need a synchronous request.
     */
    id = SendMessage(hwnd, WM_COPYDATA, (WPARAM) NULL, (LPARAM) &cds);
    if (id > 0) {
        /* KiTTY: who answered? (best effort - the sending window's process) */
        if (agent_serving_check_hook && !agent_serving_checked) {
            DWORD spid = 0;
            GetWindowThreadProcessId(hwnd, &spid);
            agent_serving_checked = true;
            agent_serving_check_hook((unsigned long)spid,
                                     KITTY_AGENT_TRANSPORT_WMCOPYDATA);
        }
        uint32_t length_field = GET_32BIT_MSB_FIRST(p);
        if (length_field > 0 && length_field <= AGENT_MAX_MSGLEN - 4) {
            retlen = length_field + 4;
            ret = snewn(retlen, unsigned char);
            memcpy(ret, p, retlen);
            *out = ret;
            *outlen = retlen;
        } else {
            /*
             * If we get here, we received an out-of-range length
             * field, either without space for a message type code or
             * overflowing the FileMapping.
             *
             * Treat this as if Pageant didn't answer at all - which
             * actually means we do nothing, and just don't fill in
             * out and outlen.
             */
        }
    }
    UnmapViewOfFile(p);
    CloseHandle(filemap);
    sfree(mapname);
    if (psd)
        LocalFree(psd);
}

Socket *agent_connect(Plug *plug)
{
    char *pipename = agent_named_pipe_name();
    Socket *s = new_named_pipe_client(pipename, plug);
    sfree(pipename);
    return s;
}

static bool named_pipe_agent_exists(void)
{
    char *pipename = agent_named_pipe_name();
    WIN32_FIND_DATA data;
    HANDLE ffh = FindFirstFile(pipename, &data);
    sfree(pipename);
    if (ffh == INVALID_HANDLE_VALUE)
        return false;
    FindClose(ffh);
    return true;
}

bool agent_exists(void)
{
    return named_pipe_agent_exists() || wm_copydata_agent_exists();
}

struct agent_pending_query {
    struct handle *handle;
    HANDLE os_handle;
    strbuf *response;
    void (*callback)(void *, void *, int);
    void *callback_ctx;
    agent_connection *conn;   /* KiTTY: a query on a held connection */
};

/* KiTTY: the held agent connection (putty.h) */
struct agent_connection {
    Socket *s;
    Plug plug;
    strbuf *in;                  /* bytes of the answer being read */
    agent_pending_query *pq;     /* the query waiting for it */
    bool dead;
};

/* KiTTY: which process is serving this pipe? Observational only. */
static void agent_serving_check_pipe(HANDLE pipehandle)
{
    if (agent_serving_check_hook && !agent_serving_checked) {
        ULONG spid = 0;
        if (GetNamedPipeServerProcessId(pipehandle, &spid)) {
            agent_serving_checked = true;
            agent_serving_check_hook((unsigned long)spid,
                                     KITTY_AGENT_TRANSPORT_PIPE);
        }
    }
}

static int named_pipe_agent_accumulate_response(
    strbuf *sb, const void *data, size_t len)
{
    put_data(sb, data, len);
    if (sb->len >= 4) {
        uint32_t length_field = GET_32BIT_MSB_FIRST(sb->u);
        if (length_field > AGENT_MAX_MSGLEN)
            return -1; /* badly formatted message */

        int overall_length = length_field + 4;
        if (sb->len >= overall_length)
            return overall_length;
    }

    return 0; /* not done yet */
}

static size_t named_pipe_agent_gotdata(
    struct handle *h, const void *data, size_t len, int err)
{
    agent_pending_query *pq = handle_get_privdata(h);

    if (err || len == 0) {
        pq->callback(pq->callback_ctx, NULL, 0);
        agent_cancel_query(pq);
        return 0;
    }

    int status = named_pipe_agent_accumulate_response(pq->response, data, len);
    if (status == -1) {
        pq->callback(pq->callback_ctx, NULL, 0);
        agent_cancel_query(pq);
    } else if (status > 0) {
        void *response_buf = strbuf_to_str(pq->response);
        pq->response = NULL;
        pq->callback(pq->callback_ctx, response_buf, status);
        agent_cancel_query(pq);
    }
    return 0;
}

static agent_pending_query *named_pipe_agent_query(
    strbuf *query, void **out, int *outlen,
    void (*callback)(void *, void *, int), void *callback_ctx)
{
    agent_pending_query *pq = NULL;
    char *err = NULL, *pipename = NULL;
    strbuf *sb = NULL;
    HANDLE pipehandle;

    pipename = agent_named_pipe_name();
    pipehandle = connect_to_named_pipe(pipename, &err);
    if (pipehandle == INVALID_HANDLE_VALUE)
        goto failure;

    agent_serving_check_pipe(pipehandle);   /* KiTTY */

    strbuf_finalise_agent_query(query);

    for (DWORD done = 0; done < query->len ;) {
        DWORD nwritten;
        bool ret = WriteFile(pipehandle, query->s + done, query->len - done,
                             &nwritten, NULL);
        if (!ret)
            goto failure;

        done += nwritten;
    }

    if (!callback) {
        int status;

        sb = strbuf_new_nm();
        do {
            char buf[1024];
            DWORD nread;
            bool ret = ReadFile(pipehandle, buf, sizeof(buf), &nread, NULL);
            if (!ret)
                goto failure;
            status = named_pipe_agent_accumulate_response(sb, buf, nread);
        } while (status == 0);

        if (status == -1)
            goto failure;

        *out = strbuf_to_str(sb);
        *outlen = status;
        sb = NULL;
        pq = NULL;
        goto out;
    }

    pq = snew(agent_pending_query);
    pq->handle = handle_input_new(pipehandle, named_pipe_agent_gotdata, pq, 0);
    pq->os_handle = pipehandle;
    pipehandle = INVALID_HANDLE_VALUE;  /* prevent it being closed below */
    pq->response = strbuf_new_nm();
    pq->callback = callback;
    pq->callback_ctx = callback_ctx;
    pq->conn = NULL;
    goto out;

  failure:
    *out = NULL;
    *outlen = 0;
    pq = NULL;

  out:
    sfree(err);
    sfree(pipename);
    if (pipehandle != INVALID_HANDLE_VALUE)
        CloseHandle(pipehandle);
    if (sb)
        strbuf_free(sb);
    return pq;
}

void agent_cancel_query(agent_pending_query *pq)
{
    if (pq->conn) {
        /* KiTTY: a query on a held connection. Its answer may still
         * come and would then meet the next query: the connection is
         * no longer used. */
        pq->conn->pq = NULL;
        pq->conn->dead = true;
        sfree(pq);
        return;
    }
    handle_free(pq->handle);
    CloseHandle(pq->os_handle);
    if (pq->response)
        strbuf_free(pq->response);
    sfree(pq);
}

/*
 * KiTTY: the held agent connection (putty.h). A Socket over the agent's
 * named pipe, as agent_connect() makes for agent forwarding, read through
 * a Plug that cuts the stream into the agent's framed answers and hands
 * each to the query waiting for it (agent_conn_receive below).
 */

static void agent_conn_answer(agent_connection *c, void *reply, int len)
{
    agent_pending_query *pq = c->pq;
    void (*callback)(void *, void *, int);
    void *callback_ctx;

    if (!pq) {
        sfree(reply);
        return;
    }
    c->pq = NULL;
    callback = pq->callback;
    callback_ctx = pq->callback_ctx;
    sfree(pq);
    callback(callback_ctx, reply, len);
}

static void agent_conn_receive(Plug *plug, int urgent, const char *data,
                               size_t len)
{
    agent_connection *c = container_of(plug, agent_connection, plug);
    uint32_t length_field;
    unsigned char *reply;
    strbuf *rest;

    if (c->dead)
        return;
    put_data(c->in, data, len);
    if (c->in->len < 4)
        return;
    length_field = GET_32BIT_MSB_FIRST(c->in->u);
    if (length_field > AGENT_MAX_MSGLEN || !c->pq) {
        /* a bad length, or an answer nobody asked for */
        c->dead = true;
        agent_conn_answer(c, NULL, 0);
        return;
    }
    if (c->in->len < (size_t)length_field + 4)
        return;
    reply = snewn(length_field + 4, unsigned char);
    memcpy(reply, c->in->u, length_field + 4);
    rest = strbuf_new_nm();
    put_data(rest, c->in->u + length_field + 4,
             c->in->len - (length_field + 4));
    strbuf_free(c->in);
    c->in = rest;
    agent_conn_answer(c, reply, length_field + 4);
}

static void agent_conn_closing(Plug *plug, PlugCloseType type,
                               const char *error_msg)
{
    agent_connection *c = container_of(plug, agent_connection, plug);
    c->dead = true;
    agent_conn_answer(c, NULL, 0);
}

static const PlugVtable agent_conn_plugvt = {
    .log = nullplug_log,
    .closing = agent_conn_closing,
    .receive = agent_conn_receive,
    .sent = nullplug_sent,
};

agent_connection *agent_conn_open(void)
{
    char *err = NULL;
    char *pipename = agent_named_pipe_name();
    HANDLE pipehandle = connect_to_named_pipe(pipename, &err);
    agent_connection *c;

    sfree(pipename);
    sfree(err);
    if (pipehandle == INVALID_HANDLE_VALUE)
        return NULL;
    agent_serving_check_pipe(pipehandle);

    c = snew(agent_connection);
    memset(c, 0, sizeof(*c));
    c->plug.vt = &agent_conn_plugvt;
    c->in = strbuf_new_nm();
    c->s = make_handle_socket(pipehandle, pipehandle, NULL, NULL, 0,
                              &c->plug, true);
    if (sk_socket_error(c->s)) {
        sk_close(c->s);
        strbuf_free(c->in);
        sfree(c);
        return NULL;
    }
    return c;
}

agent_pending_query *agent_conn_query(
    agent_connection *c, strbuf *query, void **out, int *outlen,
    void (*callback)(void *, void *, int), void *callback_ctx)
{
    agent_pending_query *pq;

    *out = NULL;
    *outlen = 0;
    if (!c || c->dead || c->pq || !callback ||
        query->len > AGENT_MAX_MSGLEN)
        return NULL;

    strbuf_finalise_agent_query(query);
    pq = snew(agent_pending_query);
    memset(pq, 0, sizeof(*pq));
    pq->callback = callback;
    pq->callback_ctx = callback_ctx;
    pq->conn = c;
    c->pq = pq;
    sk_write(c->s, query->s, query->len);
    return pq;
}

bool agent_conn_alive(agent_connection *c)
{
    return c && !c->dead;
}

void agent_conn_close(agent_connection *c)
{
    if (!c)
        return;
    if (c->pq)
        sfree(c->pq);   /* its owner has let it go with the connection */
    sk_close(c->s);
    strbuf_free(c->in);
    sfree(c);
}

agent_pending_query *agent_query(
    strbuf *query, void **out, int *outlen,
    void (*callback)(void *, void *, int), void *callback_ctx)
{
    agent_pending_query *pq = named_pipe_agent_query(
        query, out, outlen, callback, callback_ctx);
    if (pq || *out)
        return pq;

    wm_copydata_agent_query(query, out, outlen);
    return NULL;
}
