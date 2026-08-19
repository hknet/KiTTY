/*
 * kitty_perf.h - stage timers for a build that is asked "where did the time go".
 *
 * OFF unless the build is configured with -DKITTY_PERF=ON, and then it compiles
 * to nothing at all: no cost, no symbols, nothing to forget to turn off.
 *
 * Why it exists: 3.55 MB of ordinary output takes ~5 s to reach a KiTTY window
 * on Windows, while the SAME terminal core built natively swallows it in 0.96 s.
 * Nothing observable from outside explained the difference - not the read size,
 * the line shape, the grid, or the hyperlink scan - so the remaining question is
 * which stage inside the process owns those seconds. That is what these buckets
 * answer.
 *
 * Usage, per stage:   KP_T0;  ...work...;  KP_T1(KP_TERMOUT);
 * Totals are written to %TEMP%\kitty_perf.txt when the process exits.
 */
#ifndef KITTY_PERF_H
#define KITTY_PERF_H

#ifdef KITTY_PERF

enum {
    KP_RECV,        /* recv() off the socket */
    KP_PLUG,        /* plug_receive: backend -> seat -> term_data */
    KP_TERMOUT,     /* term_out: the terminal core doing the actual work */
    KP_UPDATE,      /* term_update: deciding what changed */
    KP_PAINT,       /* the window painting it */
    KP_TIMER,       /* KillTimer+SetTimer: reprogramming the Windows timer */
    KP_NBUCKETS
};

void kp_add(int bucket, long long start_qpc, long long start_cpu);
long long kp_now(void);      /* QueryPerformanceCounter: wall clock */
long long kp_cpu(void);      /* this THREAD's user+kernel time, 100ns units */

#define KP_T0 long long kp_t0 = kp_now(); long long kp_c0 = kp_cpu()
#define KP_T1(b) kp_add((b), kp_t0, kp_c0)

#else  /* !KITTY_PERF */

#define KP_T0 do {} while (0)
#define KP_T1(b) do {} while (0)

#endif /* KITTY_PERF */
#endif /* KITTY_PERF_H */
