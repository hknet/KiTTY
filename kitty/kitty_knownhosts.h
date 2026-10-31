/*
 * kitty_knownhosts.h - OpenSSH's known_hosts as kageant reads it. KiTTY++
 * never writes the file. kageant reads it only while its known_hosts
 * setting is on (Settings, Agent page; [Agent] readknownhosts=yes, default
 * off): kkh_path_hook answers NULL otherwise and nothing is opened. Three uses: the host names of a host key (dest=,
 * fwd=, the confirmation, the "Checked at each login: any matching host" match at login),
 * the rule editor's "Look up host keys" (plain, hashed and @cert-authority
 * entries; a pattern takes plain entries only) and the comparison of a
 * program's rule with the file (kar_origin_text). kkh_count counts its
 * entries for the report shown when the setting is turned on.
 *
 * Plain C, no Win32: the agent library carries it, test/test_agentrestrict.c
 * checks it with a file of its own. Include after putty.h.
 */
#ifndef KITTY_KNOWNHOSTS_H
#define KITTY_KNOWNHOSTS_H

/* The file to read, a new string (sfree it); NULL = not read. kageant sets
 * it from its setting and %USERPROFILE%. NULL hook = never read. */
extern char *(*kkh_path_hook)(void);
/* How often a known_hosts file was opened - every read counts: each
 * kkh_names and kkh_lookup call, one per program rule in Key details and
 * the editor's tree (kar_origin_text). The tests prove that nothing is read
 * while the setting is off. */
extern unsigned long kkh_opened;

/* Every name a plain host key (its blob; a certificate's own key) is
 * stored under: "host", "host:port" for a [host]:port entry. A hashed
 * entry, a marker line, a negated or wildcard name give none. add() once
 * per name found, in file order. */
void kkh_names(ptrlen keyblob, void (*add)(void *ctx, const char *name),
               void *ctx);

/* The host entries of the file: *total counts every line with a host field
 * and a key (comments, empty lines and marker lines such as
 * @cert-authority are not counted), *hashed those whose host field is
 * hashed ("|1|salt|hash", ssh-keygen -H or HashKnownHosts yes). A hashed
 * entry holds no name, so a pattern destination never matches it. false
 * (both 0) when nothing was opened. */
bool kkh_count(size_t *total, size_t *hashed);

/* Every key stored for host and port: plain and hashed entries whose host
 * field takes "host" ("[host]:port" for another port than 22), and
 * @cert-authority lines (is_ca). pattern: host is a pattern (* and ?, case
 * ignored), matched against the plain names on that port - hashed entries,
 * negations, patterns and marker lines give nothing. add() once per line
 * found; the blob is borrowed for the call. */
void kkh_lookup(const char *host, int port, bool pattern,
                void (*add)(void *ctx, ptrlen blob, bool is_ca), void *ctx);

#endif /* KITTY_KNOWNHOSTS_H */
