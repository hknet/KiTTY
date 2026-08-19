/*
 * kitty_perf.c - the accumulator behind kitty_perf.h. Compiled ONLY when the
 * build is configured with -DKITTY_PERF=ON.
 *
 * Deliberately dumb: a counter and a total per stage, no locking (the paths it
 * measures are all on the one UI thread), and a dump at exit. Anything cleverer
 * would risk measuring itself.
 */
#ifdef KITTY_PERF

#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "kitty_perf.h"   /* repo root */

static long long kp_total[KP_NBUCKETS];
static long long kp_cputot[KP_NBUCKETS];
static long long kp_calls[KP_NBUCKETS];
static long long kp_freq;
static long long kp_start_qpc;
static int kp_registered;

static const char *kp_names[KP_NBUCKETS] = {
    "recv (socket)", "plug_receive -> term_data", "term_out (terminal core)",
    "term_update", "paint", "timer reprogramming"
};

long long kp_now(void)
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (long long)li.QuadPart;
}

long long kp_cpu(void)
{
    FILETIME c, e, k, u;
    ULARGE_INTEGER ku, uu;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
        return 0;
    ku.LowPart = k.dwLowDateTime; ku.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (long long)(ku.QuadPart + uu.QuadPart);   /* 100ns units */
}

static void kp_dump(void)
{
    char path[MAX_PATH];
    DWORD n;
    FILE *fp;
    int i;
    long long wall = kp_now() - kp_start_qpc;

    n = GetTempPathA(sizeof(path) - 32, path);
    if (n == 0 || n > sizeof(path) - 32)
        return;
    strcat(path, "kitty_perf.txt");
    fp = fopen(path, "a");
    if (!fp)
        return;
    fprintf(fp, "=== kitty perf (pid %lu) ===\n", (unsigned long)GetCurrentProcessId());
    fprintf(fp, "%-28s %10s %10s %12s %10s\n",
            "stage", "wall s", "cpu s", "calls", "us/call");
    for (i = 0; i < KP_NBUCKETS; i++) {
        double secs = kp_freq ? (double)kp_total[i] / (double)kp_freq : 0.0;
        double cpu  = (double)kp_cputot[i] / 1e7;
        double per  = kp_calls[i] ? secs * 1e6 / (double)kp_calls[i] : 0.0;
        fprintf(fp, "%-28s %10.3f %10.3f %12lld %10.2f\n",
                kp_names[i], secs, cpu, kp_calls[i], per);
    }
    fprintf(fp, "%-28s %10.3f\n", "wall clock since first timed call",
            kp_freq ? (double)wall / (double)kp_freq : 0.0);

    /* WALL time and CPU time are different questions, and the timers above
     * answer only the first. If a stage shows seconds of wall clock but the
     * process burned no CPU, it was WAITING, not computing - and waiting is a
     * different bug with a different fix. Ask the OS. */
    {
        FILETIME ftc, fte, ftk, ftu;
        if (GetProcessTimes(GetCurrentProcess(), &ftc, &fte, &ftk, &ftu)) {
            ULARGE_INTEGER k, u;
            k.LowPart = ftk.dwLowDateTime;  k.HighPart = ftk.dwHighDateTime;
            u.LowPart = ftu.dwLowDateTime;  u.HighPart = ftu.dwHighDateTime;
            fprintf(fp, "%-28s %10.3f\n", "CPU time: user",
                    (double)u.QuadPart / 1e7);
            fprintf(fp, "%-28s %10.3f\n", "CPU time: kernel",
                    (double)k.QuadPart / 1e7);
            /* Kernel time in a program that only chews text means syscalls or
             * page faults. The fault count tells them apart: millions of faults
             * is memory churn (commit/decommit), few faults with high kernel
             * time is real syscalls. */
            {
                PROCESS_MEMORY_COUNTERS pmc;
                memset(&pmc, 0, sizeof(pmc));
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
                    fprintf(fp, "%-28s %10lu\n", "page faults",
                            (unsigned long)pmc.PageFaultCount);
                    fprintf(fp, "%-28s %10.1f MB\n", "peak working set",
                            (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0));
                }
            }
        }
    }
    fprintf(fp, "\n");
    fclose(fp);
}

void kp_add(int bucket, long long start_qpc, long long start_cpu)
{
    LARGE_INTEGER li;
    if (bucket < 0 || bucket >= KP_NBUCKETS)
        return;
    if (!kp_registered) {
        QueryPerformanceFrequency(&li);
        kp_freq = (long long)li.QuadPart;
        kp_start_qpc = start_qpc;
        kp_registered = 1;
        atexit(kp_dump);
    }
    kp_total[bucket] += kp_now() - start_qpc;
    kp_cputot[bucket] += kp_cpu() - start_cpu;
    kp_calls[bucket]++;
}

#endif /* KITTY_PERF */
