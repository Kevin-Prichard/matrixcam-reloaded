#include "common.h"
#include <time.h>

volatile sig_atomic_t running = 1;

uint64_t now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static void on_sig(int s) { (void)s; running = 0; }

void install_signal_handlers(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
}
