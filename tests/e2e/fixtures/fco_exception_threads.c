// SPDX-License-Identifier: MIT
//
// Linux x86_64 FCO exception-resolver stress workload.  Threads synchronize
// before many external libc/pthread calls so first-miss resolver faults overlap.

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { THREADS = 24, ROUNDS = 2048 };

static pthread_barrier_t start_barrier;

static const char *const words[] = {
    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
    "golf",  "hotel", "india",   "juliet", "kilo", "lima",
};

static void *worker(void *opaque) {
    uintptr_t id = (uintptr_t)opaque;
    unsigned long acc = 0x9e3779b97f4a7c15ULL ^ id;

    int barrier_rc = pthread_barrier_wait(&start_barrier);
    if (barrier_rc != 0 && barrier_rc != PTHREAD_BARRIER_SERIAL_THREAD)
        abort();

    for (unsigned i = 0; i != ROUNDS; ++i) {
        const char *word = words[(i + id) % (sizeof(words) / sizeof(words[0]))];
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "%s:%lu:%u", word,
                         (unsigned long)id, i);
        char *end = NULL;
        long parsed = strtol((i & 1u) ? "37" : "41", &end, 10);
        char *colon = strchr(buf, ':');
        int cmp = memcmp(buf, word, strlen(word));

        if (n <= 0 || end == NULL || *end != '\0' || colon == NULL || cmp != 0)
            abort();

        acc ^= (unsigned long)n;
        acc += (unsigned long)(colon - buf) * 131u;
        acc ^= (unsigned long)parsed + strlen(buf);
        if ((i & 127u) == 0)
            sched_yield();
    }

    return (void *)acc;
}

int main(void) {
    pthread_t threads[THREADS];
    unsigned long digest = 0;

    if (pthread_barrier_init(&start_barrier, NULL, THREADS) != 0)
        return 2;

    for (uintptr_t i = 0; i != THREADS; ++i)
        if (pthread_create(&threads[i], NULL, worker, (void *)i) != 0)
            return 3;

    for (unsigned i = 0; i != THREADS; ++i) {
        void *ret = NULL;
        if (pthread_join(threads[i], &ret) != 0)
            return 4;
        digest ^= (unsigned long)(uintptr_t)ret;
    }

    pthread_barrier_destroy(&start_barrier);
    printf("digest=%lu\n", digest);
    return 0;
}
