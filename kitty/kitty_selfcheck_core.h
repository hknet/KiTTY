/*
 * kitty_selfcheck_core.h - the one definition of the integrity stamp and of
 * the hash rule. The program checks itself at startup (kitty_selfcheck.c)
 * against a stamp written after packaging. This header is the single
 * implementation of "which bytes are hashed", so the write side and the read
 * side cannot drift apart.
 *
 * Everything here is plain C over byte offsets: no PE structs, no Windows
 * headers, no heap. The caller supplies a reader callback; the hash state is
 * PuTTY's own ssh_sha256, and the callers own the fixed-size buffers.
 *
 * THE STAMP is a fixed-size block that lives in one of two places:
 *
 *   version 1  in the PE section `.ktstamp` (the name is exactly eight
 *              characters, the most a PE section name can carry).
 *   version 2  appended to the PE OVERLAY - the bytes after the last
 *              section's raw data - because a section does not survive UPX:
 *              UPX packs the original sections into its own, so a `.ktstamp`
 *              written before packing is compressed away, and one written
 *              after sits outside what its loader expects. The overlay is
 *              copied through unchanged, which is also where Authenticode
 *              puts the certificate table.
 *
 * The block is the same 256 bytes either way; only the version field differs,
 * and one reader serves both:
 *
 *   offset  size  field
 *        0     8  magic       "KTSTAMP\0"
 *        8     4  version     little-endian; 0 = reserved, never stamped
 *       12     4  flags       little-endian; reserved, zero
 *       16     8  length      little-endian; the file length that was hashed
 *       24    32  sha256      over bytes [0, length) with the ranges below zeroed
 *       56    64  signature   raw Ed25519 R || S over bytes [0, 56) of the stamp
 *      120   136  reserved    zero
 *
 * Bytes [0, 56) - magic, version, flags, length, sha256 - are the signed
 * message, so the signature binds the length to the hash: a re-stamped file
 * cannot borrow another one's signature.
 *
 * THE HASH RULE: SHA-256 over the file from byte 0 to `length`, with three
 * byte ranges read as zeros, because Authenticode signing rewrites them after
 * the stamp is written and the stamp must survive that:
 *   1. the PE optional header CheckSum field (4 bytes),
 *   2. the Security data-directory entry (8 bytes, offset + size of the
 *      certificate table),
 *   3. the stamp itself (KT_STAMP_SIZE bytes, at the section's raw offset for
 *      version 1, at its overlay offset for version 2).
 * Signing also appends the certificate table beyond `length`; that is not
 * hashed, because it lies past the stamped length. For version 2 the block is
 * the last thing before that table, so `length` is exactly the end of the
 * block - which is what lets the reader find it without a section to name it.
 */
#ifndef KITTY_SELFCHECK_CORE_H
#define KITTY_SELFCHECK_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ssh.h"       /* ssh_sha256, ssh_hash_new/put_data/final */

#define KT_STAMP_SECTION   ".ktstamp"
#define KT_STAMP_MAGIC     "KTSTAMP"          /* 7 chars + the NUL = 8 bytes */
#define KT_STAMP_MAGIC_LEN 8
#define KT_STAMP_VERSION   1                 /* v1: in the .ktstamp section */
#define KT_STAMP_VERSION_OVERLAY 2           /* v2: appended to the overlay */
#define KT_STAMP_SIZE      256
#define KT_STAMP_SIGNED_LEN 56
#define KT_STAMP_OFF_VERSION 8
#define KT_STAMP_OFF_FLAGS   12
#define KT_STAMP_OFF_LENGTH  16
#define KT_STAMP_OFF_SHA256  24
#define KT_STAMP_OFF_SIG     56
#define KT_SHA256_LEN        32
#define KT_ED25519_SIG_LEN   64
#define KT_ED25519_PUB_LEN   32

/* The chunk the file is streamed through. On the stack in both users. */
#define KT_STAMP_CHUNK       16384

/*
 * The marker a test-only fault build carries (KITTY_SELFCHECK_FAULT). The
 * stamp tool refuses any file that contains it, so a binary with the fault
 * switch can never be stamped by mistake and go out as a release.
 */
#define KT_SELFCHECK_FAULT_MARKER "KTSELFCHECK-FAULT-BUILD"

/*
 * Where the interesting bytes of one PE file are. All offsets are file
 * offsets; a value of 0 for stamp_off means "no .ktstamp section".
 */
typedef struct kt_pe_layout {
    uint64_t checksum_off;     /* optional header CheckSum, 4 bytes */
    uint64_t secdir_off;       /* data directory [4], 8 bytes */
    uint64_t stamp_off;        /* file offset of the stamp block, 0 = none */
    uint32_t stamp_raw_size;   /* the .ktstamp section's SizeOfRawData */
    uint64_t certtab_off;      /* VALUE of data directory [4]: the certificate
                                * table's file offset, 0 when unsigned */
    uint32_t certtab_size;     /* its size, 0 when unsigned */
    int      stamp_version;    /* which shape stamp_off points at, 0 = none */
} kt_pe_layout;

/*
 * Reader callback: fill `buf` with `len` bytes from file offset `off`.
 * Returns nonzero on success (all `len` bytes), zero on any failure.
 */
typedef int (*kt_read_fn)(void *ctx, uint64_t off, void *buf, size_t len);

static inline uint16_t kt_le16(const unsigned char *p)
{ return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t kt_le32(const unsigned char *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t kt_le64(const unsigned char *p)
{ return (uint64_t)kt_le32(p) | ((uint64_t)kt_le32(p + 4) << 32); }
static inline void kt_put_le32(unsigned char *p, uint32_t v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static inline void kt_put_le64(unsigned char *p, uint64_t v)
{ kt_put_le32(p, (uint32_t)v); kt_put_le32(p + 4, (uint32_t)(v >> 32)); }

/*
 * Parse the PE headers. `hdr` holds the first `hdrlen` bytes of the file;
 * 4096 is enough for every PE the build produces (the section table of a
 * MinGW image ends well inside the first kilobyte). Returns nonzero when the
 * layout was found, zero when the bytes are not a PE this code understands
 * or the headers do not fit in `hdr`.
 */
static inline int kt_pe_parse(const unsigned char *hdr, size_t hdrlen,
                              kt_pe_layout *out)
{
    uint32_t pe, nsect, optsize, ddir_off, secoff, i;
    uint16_t magic;

    memset(out, 0, sizeof(*out));
    if (hdrlen < 64 || hdr[0] != 'M' || hdr[1] != 'Z')
        return 0;
    pe = kt_le32(hdr + 60);                         /* e_lfanew */
    if (pe + 24 > hdrlen || memcmp(hdr + pe, "PE\0\0", 4) != 0)
        return 0;
    nsect = kt_le16(hdr + pe + 6);
    optsize = kt_le16(hdr + pe + 20);
    if (optsize < 96 || pe + 24 + 96 > hdrlen)
        return 0;
    magic = kt_le16(hdr + pe + 24);
    if (magic == 0x10b)                              /* PE32 */
        ddir_off = 96;
    else if (magic == 0x20b)                         /* PE32+ */
        ddir_off = 112;
    else
        return 0;
    /* CheckSum sits at the same place in both optional-header shapes. */
    out->checksum_off = (uint64_t)pe + 24 + 64;
    /* Data directory entry 4 = Security: 8 bytes each entry. */
    out->secdir_off = (uint64_t)pe + 24 + ddir_off + 4 * 8;
    if (out->secdir_off + 8 > pe + 24 + optsize)
        return 0;                                    /* no such directory */
    secoff = pe + 24 + optsize;
    if ((uint64_t)secoff + (uint64_t)nsect * 40 > hdrlen)
        return 0;
    for (i = 0; i < nsect; i++) {
        const unsigned char *s = hdr + secoff + i * 40;
        if (memcmp(s, KT_STAMP_SECTION, 8) == 0) {
            out->stamp_raw_size = kt_le32(s + 16);   /* SizeOfRawData */
            out->stamp_off = kt_le32(s + 20);        /* PointerToRawData */
            break;
        }
    }
    if (out->stamp_off && out->stamp_raw_size < KT_STAMP_SIZE)
        out->stamp_off = 0;                          /* too small to be ours */
    if (out->stamp_off)
        out->stamp_version = KT_STAMP_VERSION;
    /* The certificate table's own offset and size, needed to know where the
     * overlay ends on a signed file. Read from the directory entry itself,
     * which the hash zeroes but the header still carries here. */
    if (out->secdir_off + 8 <= hdrlen) {
        out->certtab_off = kt_le32(hdr + out->secdir_off);
        out->certtab_size = kt_le32(hdr + out->secdir_off + 4);
    }
    return 1;
}

/*
 * Find the stamp block. The .ktstamp section wins when there is one (v1);
 * otherwise the last KT_STAMP_SIZE bytes before the certificate table - or
 * before end of file on an unsigned image - are tried as a v2 block.
 *
 * The candidate is accepted only when the magic matches, the version is 2 AND
 * the block's own `length` names the byte just past the block. That last test
 * is what makes this safe on a file carrying an unrelated overlay: an
 * installer's appended data cannot satisfy it by accident, and a wrong guess
 * fails here instead of hashing the wrong bytes. The signature still has to
 * verify afterwards.
 *
 * Returns nonzero when a stamp was located; `lay->stamp_off` and
 * `lay->stamp_version` then describe it.
 */
static inline int kt_stamp_locate(kt_read_fn rd, void *ctx, uint64_t filesize,
                                  kt_pe_layout *lay)
{
    unsigned char blk[KT_STAMP_SIZE];
    uint64_t end, cand;

    if (lay->stamp_off)                              /* v1, already found */
        return 1;
    end = lay->certtab_off ? lay->certtab_off : filesize;
    if (end > filesize || end < KT_STAMP_SIZE)
        return 0;
    cand = end - KT_STAMP_SIZE;
    if (!rd(ctx, cand, blk, KT_STAMP_SIZE))
        return 0;
    if (memcmp(blk, KT_STAMP_MAGIC, KT_STAMP_MAGIC_LEN) != 0)
        return 0;
    if (kt_le32(blk + KT_STAMP_OFF_VERSION) != KT_STAMP_VERSION_OVERLAY)
        return 0;
    if (kt_le64(blk + KT_STAMP_OFF_LENGTH) != cand + KT_STAMP_SIZE)
        return 0;
    lay->stamp_off = cand;
    lay->stamp_version = KT_STAMP_VERSION_OVERLAY;
    return 1;
}

/*
 * Zero every byte of `buf` (covering file range [off, off+len)) that falls
 * inside [zoff, zoff+zlen).
 */
static inline void kt_zero_range(unsigned char *buf, uint64_t off, size_t len,
                                 uint64_t zoff, uint64_t zlen)
{
    uint64_t a = off, b = off + len, za = zoff, zb = zoff + zlen;
    if (za < a) za = a;
    if (zb > b) zb = b;
    if (za < zb)
        memset(buf + (za - off), 0, (size_t)(zb - za));
}

/*
 * SHA-256 over [0, length) of the file with the three ranges zeroed. `chunk`
 * is the caller's KT_STAMP_CHUNK-byte buffer. Returns nonzero on success.
 */
static inline int kt_stamp_hash(kt_read_fn rd, void *ctx,
                                const kt_pe_layout *lay, uint64_t length,
                                unsigned char *chunk, unsigned char out[32])
{
    ssh_hash *h = ssh_hash_new(&ssh_sha256);
    uint64_t off = 0;

    while (off < length) {
        size_t n = (size_t)((length - off < KT_STAMP_CHUNK) ?
                            (length - off) : KT_STAMP_CHUNK);
        if (!rd(ctx, off, chunk, n)) {
            ssh_hash_free(h);
            return 0;
        }
        kt_zero_range(chunk, off, n, lay->checksum_off, 4);
        kt_zero_range(chunk, off, n, lay->secdir_off, 8);
        kt_zero_range(chunk, off, n, lay->stamp_off, KT_STAMP_SIZE);
        put_data(h, chunk, n);
        off += n;
    }
    ssh_hash_final(h, out);
    return 1;
}

/*
 * Ed25519 verify of a raw 64-byte signature over `msg`, with a raw 32-byte
 * public key, through PuTTY's own ssh_ecdsa_ed25519 - which expects both
 * wrapped in their SSH wire shapes. Returns nonzero when the signature holds.
 */
static inline int kt_ed25519_verify(const unsigned char pub[32],
                                    const unsigned char sig[64],
                                    const void *msg, size_t msglen)
{
    strbuf *pubblob = strbuf_new();
    strbuf *sigblob = strbuf_new();
    ssh_key *key;
    int ok = 0;

    put_stringz(pubblob, "ssh-ed25519");
    put_string(pubblob, pub, 32);
    put_stringz(sigblob, "ssh-ed25519");
    put_string(sigblob, sig, 64);
    key = ssh_key_new_pub(&ssh_ecdsa_ed25519, ptrlen_from_strbuf(pubblob));
    if (key) {
        ok = ssh_key_verify(key, ptrlen_from_strbuf(sigblob),
                            make_ptrlen(msg, msglen)) ? 1 : 0;
        ssh_key_free(key);
    }
    strbuf_free(sigblob);
    strbuf_free(pubblob);
    return ok;
}

/* Is the stamp block filled at all? Version 0 or an all-zero hash = unfilled. */
static inline int kt_stamp_is_filled(const unsigned char *stamp)
{
    size_t i;
    if (memcmp(stamp, KT_STAMP_MAGIC, KT_STAMP_MAGIC_LEN) != 0)
        return 0;
    if (kt_le32(stamp + KT_STAMP_OFF_VERSION) == 0)
        return 0;
    for (i = 0; i < KT_SHA256_LEN; i++)
        if (stamp[KT_STAMP_OFF_SHA256 + i])
            return 1;
    return 0;
}

#endif /* KITTY_SELFCHECK_CORE_H */
