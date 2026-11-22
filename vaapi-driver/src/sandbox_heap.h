#ifndef DMD_SANDBOX_HEAP_H
#define DMD_SANDBOX_HEAP_H

/* Optional Firefox launch adapter. It opens only the system DMA heap in the
 * RDD process, before Firefox enables its normal seccomp/broker sandbox.
 * Other applications and launches without the adapter keep the usual opens.
 * A returned descriptor is owned by the caller; the adapter retains its own.
 */
extern int dmd_firefox_system_heap_dup(void)
    __attribute__((weak, visibility("default")));

static inline int dmd_sandbox_system_heap_dup(void)
{
    return dmd_firefox_system_heap_dup ? dmd_firefox_system_heap_dup() : -1;
}

#endif
