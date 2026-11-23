/* (ps5, 2026-11-10) How many of the thread library's objects the title makes, and who makes them. Runs 8 to 11 ended at the
   first 3D scene in a mutex lock that failed or never came back, and EglantinePS5 met the same on this console ("mutex lock
   failed: Cannot allocate memory", its headless/striped_wait.h): a mutex, a condition or a read-write lock is an object of
   the thread library, and a process can hold only so many. The eboot's --wrap of the six init and destroy calls
   (nx2/toolchain_ps5.py link_title) counts the ones made by name, with the 32 call sites that hold the most; the ones made
   by a static initializer's first lock (std::mutex, PTHREAD_MUTEX_INITIALIZER) are not seen here.
   hos_ps5_pthread_counts: one line for the scheduling report and for a failed lock's report. */
#ifdef __PROSPERO__
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int __real_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int __real_pthread_mutex_destroy(pthread_mutex_t *m);
int __real_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int __real_pthread_cond_destroy(pthread_cond_t *c);
int __real_pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int __real_pthread_rwlock_destroy(pthread_rwlock_t *l);

static long made[3], gone[3], failed[3];
static int first_error[3];
#define SITES 4096
static struct { uintptr_t ra; long live; } sites[SITES];   /* (open addressing by the caller's return address; never emptied) */

static void site(uintptr_t ra, long d) {
    unsigned h = (unsigned)((ra * 0x9E3779B97F4A7C15ULL) >> 52) % SITES;
    for (int i = 0; i < SITES; i++, h = (h + 1) % SITES) {
        uintptr_t seen = __atomic_load_n(&sites[h].ra, __ATOMIC_RELAXED);
        if (!seen) {
            uintptr_t none = 0;
            if (__atomic_compare_exchange_n(&sites[h].ra, &none, ra, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) seen = ra;
            else seen = none;
        }
        if (seen == ra) {
            __atomic_add_fetch(&sites[h].live, d, __ATOMIC_RELAXED);
            return;
        }
    }
}
/* (a destroy does not know who made the object: only makes are counted by site; the totals say what is live) */
#define MADE(k, r, ra) do { if (r) { if (!__atomic_fetch_add(&failed[k], 1, __ATOMIC_RELAXED)) first_error[k] = (r); } \
                            else { __atomic_add_fetch(&made[k], 1, __ATOMIC_RELAXED); site((uintptr_t)(ra), 1); } } while (0)
int __wrap_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    const int r = __real_pthread_mutex_init(m, a);
    MADE(0, r, __builtin_return_address(0));
    return r;
}
int __wrap_pthread_mutex_destroy(pthread_mutex_t *m) {
    const int r = __real_pthread_mutex_destroy(m);
    if (!r) __atomic_add_fetch(&gone[0], 1, __ATOMIC_RELAXED);
    return r;
}
int __wrap_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    const int r = __real_pthread_cond_init(c, a);
    MADE(1, r, __builtin_return_address(0));
    return r;
}
int __wrap_pthread_cond_destroy(pthread_cond_t *c) {
    const int r = __real_pthread_cond_destroy(c);
    if (!r) __atomic_add_fetch(&gone[1], 1, __ATOMIC_RELAXED);
    return r;
}
int __wrap_pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) {
    const int r = __real_pthread_rwlock_init(l, a);
    MADE(2, r, __builtin_return_address(0));
    return r;
}
int __wrap_pthread_rwlock_destroy(pthread_rwlock_t *l) {
    const int r = __real_pthread_rwlock_destroy(l);
    if (!r) __atomic_add_fetch(&gone[2], 1, __ATOMIC_RELAXED);
    return r;
}

void hos_ps5_pthread_counts(char *buf, size_t cap) {
    int n = snprintf(buf, cap, "made by name and not destroyed: mutexes %ld (%ld made, %ld refused, first error %d), conditions %ld (%ld made, "
                               "%ld refused, first error %d), read-write locks %ld (%ld made, %ld refused, first error %d); the makers "
                               "(return address: made):",
                     made[0] - gone[0], made[0], failed[0], first_error[0], made[1] - gone[1], made[1], failed[1], first_error[1],
                     made[2] - gone[2], made[2], failed[2], first_error[2]);
    /* the 8 sites with the most made */
    int used[8];
    for (int k = 0; k < 8 && n < (int)cap - 40; k++) {
        int best = -1;
        for (int i = 0; i < SITES; i++) {
            if (!sites[i].ra) continue;
            int taken = 0;
            for (int j = 0; j < k; j++) taken |= used[j] == i;
            if (!taken && (best < 0 || sites[i].live > sites[best].live)) best = i;
        }
        if (best < 0) break;
        used[k] = best;
        n += snprintf(buf + n, cap - (size_t)n, " %#lx: %ld;", (unsigned long)sites[best].ra, sites[best].live);
    }
}
/* how many more mutexes the thread library gives now, up to `most` (made and destroyed again): 0 says it has none left */
long hos_ps5_pthread_room(long most) {
    static pthread_mutex_t probe[4096];
    if (most > 4096) most = 4096;
    long n = 0;
    while (n < most && !__real_pthread_mutex_init(&probe[n], NULL)) n++;
    for (long i = 0; i < n; i++) __real_pthread_mutex_destroy(&probe[i]);
    return n;
}
#endif
