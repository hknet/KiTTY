/*
 * kitty_crashlog.c - when the process crashes, say WHERE, in a form that
 * survives the trip back from an air-gapped XP VM.
 *
 * Motivation: the first XP runtime crash arrived as "msvcrt.dll offset
 * 37742" - an address inside somebody else's DLL, useless for finding our
 * bug, and every clarifying question costs a VM round-trip. This filter
 * writes kitty_crash.log NEXT TO THE EXE with the exception code, the
 * faulting address as module+offset, and a scan of the stack for values
 * that point into loaded modules - a poor man's backtrace that needs no
 * dbghelp, no symbols and no frame pointers (the release build omits
 * those). Offsets into our own exe resolve to source lines with addr2line.
 *
 * Registered from a constructor, so every binary linking `utils` carries it
 * with no per-target wiring. It chains to any previously installed filter.
 * Diagnostic yield only; it changes no behaviour on the happy path.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static LPTOP_LEVEL_EXCEPTION_FILTER kcl_prev;

extern const char ver[];     /* version string, in utils like this file */

/* Module names go into the log as BASE NAME only. The log is meant to be
 * attached to public issue reports, and a full path carries the Windows
 * user name whenever the exe runs from a profile directory. The base name
 * plus offset is what identifies the module anyway. */
static void kcl_module_of(void *addr, char *name, size_t namesz,
                          ULONG_PTR *offset)
{
    MEMORY_BASIC_INFORMATION mbi;
    name[0] = '\0';
    *offset = 0;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) &&
        mbi.State == MEM_COMMIT && mbi.AllocationBase) {
        if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, name,
                               (DWORD)namesz)) {
            char *base = name, *p;
            /* pre-Vista does not NUL-terminate on truncation */
            name[namesz - 1] = '\0';
            for (p = name; *p; p++)
                if (*p == '\\' || *p == '/')
                    base = p + 1;
            if (base != name)
                memmove(name, base, strlen(base) + 1);
            *offset = (ULONG_PTR)addr - (ULONG_PTR)mbi.AllocationBase;
        } else
            name[0] = '\0';
    }
}

static LONG WINAPI kcl_filter(EXCEPTION_POINTERS *ep)
{
    char path[MAX_PATH + 32], mod[MAX_PATH], exe[64];
    ULONG_PTR off;
    FILE *fp;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    /* remember which program this is, base name only, before the path is
     * repurposed - one temp-directory log can collect several programs */
    {
        DWORD b = n;
        while (b > 0 && path[b - 1] != '\\' && path[b - 1] != '/')
            b--;
        lstrcpynA(exe, path + b, sizeof(exe));
    }
    /* beside the exe: replace the file name with kitty_crash.log */
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    lstrcpyA(path + n, "kitty_crash.log");

    fp = fopen(path, "a");
    if (!fp) {
        /* Beside the exe is read-only when running from a mounted ISO -
         * exactly the air-gapped delivery this exists for. */
        char tmp[MAX_PATH + 32];
        DWORD t = GetTempPathA(MAX_PATH, tmp);
        if (t && t < MAX_PATH) {
            lstrcpyA(tmp + t, "kitty_crash.log");
            fp = fopen(tmp, "a");
        }
    }
    if (fp) {
        void *ip =
#if defined _M_IX86 || defined __i386__
            (void *)ep->ContextRecord->Eip;
        void *sp = (void *)ep->ContextRecord->Esp;
#else
            (void *)ep->ContextRecord->Rip;
        void *sp = (void *)ep->ContextRecord->Rsp;
#endif
        /* One self-contained header per crash: which program, which build,
         * which Windows - the log is meant to be attached to an issue
         * report, so it has to say what crashed without a follow-up. */
        {
            OSVERSIONINFOA osv;
            memset(&osv, 0, sizeof(osv));
            osv.dwOSVersionInfoSize = sizeof(osv);
            GetVersionExA(&osv);
            fprintf(fp, "=== %s %s (%u-bit) on Windows %lu.%lu build %lu\n",
                    exe, ver, (unsigned)(sizeof(void *) * 8),
                    (unsigned long)osv.dwMajorVersion,
                    (unsigned long)osv.dwMinorVersion,
                    (unsigned long)osv.dwBuildNumber);
        }
        fprintf(fp, "=== crash: exception 0x%08lx\n",
                (unsigned long)ep->ExceptionRecord->ExceptionCode);
        kcl_module_of(ip, mod, sizeof(mod), &off);
        fprintf(fp, "at %s+0x%lx\n", mod[0] ? mod : "?",
                (unsigned long)off);
        /* Stack scan: every aligned value on the top of the stack that
         * points into a committed module image MIGHT be a return address.
         * Noisy, but the true call chain is in there, in order. */
        {
            ULONG_PTR *p = (ULONG_PTR *)sp;
            int printed = 0;
            for (int i = 0; i < 2048 && printed < 40; i++) {
                ULONG_PTR v;
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(p + i, &mbi, sizeof(mbi)) ||
                    mbi.State != MEM_COMMIT)
                    break;
                v = p[i];
                if (v < 0x10000)
                    continue;
                kcl_module_of((void *)v, mod, sizeof(mod), &off);
                if (mod[0] && off) {
                    /* only image-backed regions, or it is data, not code */
                    fprintf(fp, "  stack[%d] %s+0x%lx\n", i, mod,
                            (unsigned long)off);
                    printed++;
                }
            }
        }
        fflush(fp);
        fclose(fp);
    }
    return kcl_prev ? kcl_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static void __attribute__((constructor)) kcl_install(void)
{
    kcl_prev = SetUnhandledExceptionFilter(kcl_filter);
}
