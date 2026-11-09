/*
 * kitty_random.h - random bytes from Windows, for every program of the
 * suite: RtlGenRandom (SystemFunction036), the documented-stable export
 * underneath CryptGenRandom, present since XP, in advapi32.
 *
 * PuTTY's own pool is not usable everywhere: kageant stubs random_read()
 * with a fatal error, and in kitty.exe nothing need have seeded it when a
 * key, a token or a file name is made. A header only, so it adds no file to
 * the build lists of the programs that use it.
 *
 * Not BCryptGenRandom or ProcessPrng: neither exists on XP, which the suite
 * still runs on. On Windows 11 (25H2) advapi32's
 * SystemFunction036 forwards to cryptbase, which delay-loads ProcessPrng
 * from bcryptPrimitives.dll - the same generator, one hop further.
 * CryptGenRandom (PuTTY's windows/noise.c) ends there too, through rsaenh.
 */
#ifndef KITTY_RANDOM_H
#define KITTY_RANDOM_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

BOOLEAN NTAPI SystemFunction036(PVOID buf, ULONG len);

/* true: buf holds len random bytes. false: Windows gave none - the caller
 * decides; never fill in something guessable and go on as if it worked. */
static inline bool kitty_os_random(void *buf, size_t len)
{
    return SystemFunction036(buf, (ULONG)len) != 0;
}

#endif /* KITTY_RANDOM_H */
