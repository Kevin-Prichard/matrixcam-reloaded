#ifndef CELLAVG_COMMON_H
#define CELLAVG_COMMON_H
#include <signal.h>
#include <stdint.h>

/* Cleared by the SIGINT/SIGTERM handler to begin an orderly shutdown;
 * checked by every long-running loop in the capture/pipeline/display code. */
extern volatile sig_atomic_t running;

uint64_t now_us(void);

/* Installs the SIGINT/SIGTERM handler that clears `running`. */
void install_signal_handlers(void);

#endif
