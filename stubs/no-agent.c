#include "putty.h"

bool agent_exists(void) { return false; }
Socket *agent_connect(Plug *plug) {
    return new_error_socket_fmt(
        plug, "no actual networking in this application");
}
void agent_cancel_query(agent_pending_query *pq) {}
agent_pending_query *agent_query(
    strbuf *query, void **out, int *outlen,
    void (*callback)(void *, void *, int), void *callback_ctx) {return NULL;}
/* KiTTY: the held agent connection (putty.h) */
agent_connection *agent_conn_open(void) { return NULL; }
agent_pending_query *agent_conn_query(
    agent_connection *conn, strbuf *query, void **out, int *outlen,
    void (*callback)(void *, void *, int), void *callback_ctx)
{ *out = NULL; *outlen = 0; return NULL; }
bool agent_conn_alive(agent_connection *conn) { return false; }
void agent_conn_close(agent_connection *conn) {}
