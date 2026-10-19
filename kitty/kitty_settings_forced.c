/*
 * kitty_settings_forced.c - write a session to a ".ktx" file (the export).
 *
 * KiTTY++'s session saver, save_open_settings in settings.c, writes every key
 * into a list in memory (kitty_open_settings_w_items: nothing reaches the
 * store), and this file adds the two passwords in their export form and
 * writes the list in classic KiTTY's "key\value\" lines, which every reader
 * of .ktx files takes (kitty_settings_load.c via ksf_load, older KiTTY).
 * "Make a portable copy" writes the copy's folder store through here too; its
 * files take the store's own "key=value" lines with LF, as ksf_save writes a
 * new file (kitty_set_bundle_store_form): the first save keeps them as they are.
 * It used to be a hand-written copy of the saver, one line per key, and had
 * fallen 59 keys behind it: an export lost the background image, the
 * transfer and Tools-menu settings and more.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"
#include "kitty.h"
#include "kitty_pwmem.h"   /* passwords wrapped in memory */
#include "kitty_storage.h"
#include "kitty_secretstore.h"
#include "kitty_settings.h"

/*
 * KiTTY: exported .ktx files are no longer written encrypted.
 *
 * The "encryption" was `cryptstring` under MASTER_PASSWORD - a constant compiled
 * into every build and visible in the build file - so it could be undone by
 * anyone holding a copy of KiTTY. It was obfuscation presented as protection.
 *
 * It was also unreachable by accident: no menu item, no documentation, off by
 * default, and switched on only by the undocumented internal command /crypt,
 * which has been removed with it. So nobody was relying on it, and nobody was
 * misled into thinking a file was safe. The project had already published the
 * same reasoning for the neighbouring case - FEATURES.md on backups: "the
 * registry already holds saved passwords in protected form, so a second layer
 * added nothing".
 *
 * The READ path in kitty_settings_load.c deliberately stays: .ktx files written
 * by older builds must keep loading, and KNOWN-ISSUES.md documents that they do.
 * Removing the read side is part of retiring MASTER_PASSWORD itself, which cannot
 * happen until users have re-saved.
 */

static void ktx_write_line(const char *key, const char *val, void *ctx)
{
    char *mv = ksf_munge(val ? val : "");
    fprintf((FILE *)ctx, "%s\\%s\\\n", key, mv ? mv : "");
    if (mv) { smemclr(mv, strlen(mv)); sfree(mv); }
}

/* the folder store's line (ksf_save in kitty_storage.c) */
static void store_write_line(const char *key, const char *val, void *ctx)
{
    char *mv = ksf_munge(val ? val : "");
    fprintf((FILE *)ctx, "%s=%s\n", key, mv ? mv : "");
    if (mv) { smemclr(mv, strlen(mv)); sfree(mv); }
}

/* The session a bundle's summary lists: the file's name without its
 * extension, unescaped as the import does (kitty_ktx_session_name). */
static char *ktx_session_of(const char *filename)
{
    const char *b = strrchr(filename, '\\');
    char *stem = dupstr(b ? b + 1 : filename), *dot = strrchr(stem, '.');
    char *name;
    if (dot && dot != stem) *dot = '\0';
    name = kitty_session_fname_unmunge(stem);
    sfree(stem);
    return name;
}

int save_open_settings_forced(char *filename, Conf *conf) {
    settings_w *w = kitty_open_settings_w_items();
    struct ksf_item *items;
    FILE *fp;
    int ok;
    DWORD err;
    int store_form = kitty_bundle_store_form();
    int pwlocked = 0;               /* a password left out: master password locked */
    char *where = ktx_session_of(filename);   /* for a bundle's summary */

    save_open_settings(w, conf);
    items = kitty_settings_w_take_items(w);

    {
        /* A .ktx export is a portable file: the password gets the envelope a
         * portable session file gets - the bundle's own passphrase while an
         * export bundle is written, else the master password - none written
         * while it is locked (reported below), never a DPAPI value nobody
         * chose - plain only in explicit legacy mode. A bundle wrap that
         * failed leaves the line out (the summary lists it). The Conf holds
         * it wrapped for this process (kitty_pwmem.c). */
        char pw[KITTY_PW_MAX + 1];
        char *blob;
        int skipped = 0;
        kitty_pw_get(conf, CONF_password, pw, sizeof(pw));
        blob = kitty_secret_wrap_export(pw, KSEC_EXPORT_SESSION_PW, where,
                                        &pwlocked, &skipped);
        smemclr(pw, sizeof(pw));
        if (!skipped)
            ksf_list_set(&items, "Password", blob ? blob : "");
        if (blob) { memset(blob, 0, strlen(blob)); free(blob); }
    }
    {
        /* the proxy password in the same envelope. The reader
         * (ktx_decode_password) takes the envelope and, from files written
         * before, the plain value. */
        char pxpw[KITTY_PW_MAX + 1];
        char *blob = NULL;
        kitty_pw_get(conf, CONF_proxy_password, pxpw, sizeof(pxpw));
        int pxlocked = 0, pxskipped = 0;
        if (pxpw[0])
            blob = kitty_secret_wrap_export(pxpw, KSEC_EXPORT_SESSION_PXPW, where,
                                            &pxlocked, &pxskipped);
        smemclr(pxpw, sizeof(pxpw));
        if (!pxskipped)
            ksf_list_set(&items, "ProxyPassword", blob ? blob : "");
        if (blob) { memset(blob, 0, strlen(blob)); free(blob); }
        if (pxlocked) pwlocked = 1;
    }
    sfree(where);
    if (pwlocked)
        ksec_report_locked_save(KSEC_KEPT_EXPORT, filename);

    if ((fp = fopen(filename, store_form ? "wb" : "w")) == NULL) {
        ksf_list_free(items);
        return 0;
    }
    ksf_list_foreach(items, store_form ? store_write_line : ktx_write_line, fp);
    ksf_list_free(items);
    ok = !ferror(fp);
    err = GetLastError();
    if (fclose(fp) != 0 && ok) { ok = 0; err = GetLastError(); }
    if (!ok) SetLastError(err ? err : ERROR_WRITE_FAULT);
    return ok;
}
