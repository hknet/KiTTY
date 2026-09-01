/*
 * kitty_protkey.c - see kitty_protkey.h. Faithful extraction of pageant's
 * protected_skey_from_key / _to_temp_key / _free, made reusable.
 */
#include "putty.h"
#include "ssh.h"
#include "cryptoapi.h"
#include "kitty_protkey.h"

struct KittyProtKey {
    unsigned char *data;              /* CryptProtectMemory'd openssh blob */
    size_t blob_len, protected_len;   /* real length, and padded length */
    const ssh_keyalg *alg;            /* to rebuild with the right algorithm */
};

static bool kpk_protect(void *data, size_t len)
{
#ifdef _WINDOWS
    return got_crypt() && p_CryptProtectMemory(
        data, (DWORD)len, CRYPTPROTECTMEMORY_SAME_PROCESS);
#else
    (void)data; (void)len; return false;
#endif
}

static bool kpk_unprotect(void *data, size_t len)
{
#ifdef _WINDOWS
    return got_crypt() && p_CryptUnprotectMemory(
        data, (DWORD)len, CRYPTPROTECTMEMORY_SAME_PROCESS);
#else
    (void)data; (void)len; return false;
#endif
}

int kitty_protkey_available(void)
{
    static int cached = -1;
    if (cached < 0) {
        /* A real round trip, not just got_crypt(): the DLL loading proves
         * nothing about the calls working, and the whole point of asking is
         * to TELL THE USER when their protection is off. */
        unsigned char buf[CRYPTPROTECTMEMORY_BLOCK_SIZE];
        unsigned char ref[sizeof(buf)];
        memset(buf, 0xA5, sizeof(buf));
        memcpy(ref, buf, sizeof(buf));
        cached = kpk_protect(buf, sizeof(buf)) &&
                 kpk_unprotect(buf, sizeof(buf)) &&
                 memcmp(buf, ref, sizeof(buf)) == 0;
        smemclr(buf, sizeof(buf));
    }
    return cached;
}

int kitty_protkey_absent(void)
{
#ifdef _WINDOWS
    /* The API cannot even be resolved - the honest reading on pre-Vista
     * Windows, where CryptProtectMemory simply does not exist. Distinct
     * from kitty_protkey_available()==0 with the symbol present, which
     * means something is interfering on a Windows that HAS it. */
    return !(got_crypt() && p_CryptProtectMemory && p_CryptUnprotectMemory);
#else
    return 1;
#endif
}

KittyProtKey *kitty_protkey_from_key(ssh_key *key)
{
    strbuf *plain = strbuf_new_nm();
    ssh_key_openssh_blob(key, BinarySink_UPCAST(plain));

    size_t blob_len = plain->len;
    size_t protected_len = ((blob_len + CRYPTPROTECTMEMORY_BLOCK_SIZE - 1) /
                            CRYPTPROTECTMEMORY_BLOCK_SIZE) *
                           CRYPTPROTECTMEMORY_BLOCK_SIZE;
    unsigned char *data = snewn(protected_len, unsigned char);
    memset(data, 0, protected_len);
    memcpy(data, plain->u, blob_len);

    bool ok = kpk_protect(data, protected_len);
    smemclr(plain->u, blob_len);
    strbuf_free(plain);

    if (!ok) {
        smemclr(data, protected_len);
        sfree(data);
        return NULL;
    }

    KittyProtKey *pk = snew(KittyProtKey);
    pk->data = data;
    pk->blob_len = blob_len;
    pk->protected_len = protected_len;
    pk->alg = ssh_key_alg(key);
    return pk;
}

ssh_key *kitty_protkey_to_temp_key(KittyProtKey *pk)
{
    if (!pk)
        return NULL;

    if (!kpk_unprotect(pk->data, pk->protected_len))
        return NULL;

    BinarySource src[1];
    BinarySource_BARE_INIT_PL(src, make_ptrlen(pk->data, pk->blob_len));
    ssh_key *key = ssh_key_new_priv_openssh(pk->alg, src);

    /* Re-encrypt the buffer immediately, whether or not the rebuild worked. */
    if (!kpk_protect(pk->data, pk->protected_len)) {
        smemclr(pk->data, pk->protected_len);
        if (key)
            ssh_key_free(key);
        return NULL;
    }

    return key;
}

void kitty_protkey_free(KittyProtKey *pk)
{
    if (!pk)
        return;
    if (pk->data) {
        smemclr(pk->data, pk->protected_len);
        sfree(pk->data);
    }
    smemclr(pk, sizeof(*pk));
    sfree(pk);
}
