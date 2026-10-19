/* ---------- Generic capture -> analysis pipeline ---------- */
#include "pipeline.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

enum { S_FREE, S_QUEUED, S_BUSY };
typedef struct { uint8_t *buf; size_t cap, len; uint64_t seq; int state; } slot_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_job = PTHREAD_COND_INITIALIZER, cv_res = PTHREAD_COND_INITIALIZER;
static slot_t *slots;
static int nslots;
static uint8_t *latest;
static uint64_t latest_seq, next_seq;
static size_t result_grid_bytes;
static int grid_w, grid_h;
static const frame_analyzer_t *active_analyzer;
static pthread_t *worker_threads;
static int worker_thread_count;
static pipeline_stats_t st;

static void *worker_main(void *arg) {
    (void)arg;
    void *context = active_analyzer->create_context ? active_analyzer->create_context() : NULL;
    uint8_t *out = malloc(result_grid_bytes);

    for (;;) {
        pthread_mutex_lock(&mu);
        slot_t *s = NULL;
        while (running) {
            s = NULL;
            for (int i = 0; i < nslots; i++)
                if (slots[i].state == S_QUEUED && (!s || slots[i].seq < s->seq)) s = &slots[i];
            if (s) break;
            pthread_cond_wait(&cv_job, &mu);
        }
        if (!s) { pthread_mutex_unlock(&mu); break; }
        s->state = S_BUSY;
        pthread_mutex_unlock(&mu);

        uint64_t t0 = now_us();
        int rc = active_analyzer->analyze(context, s->buf, s->len, out);
        uint64_t t1 = now_us();

        pthread_mutex_lock(&mu);
        if (rc == 0) {
            st.proc++; st.proc_us += t1 - t0;
            if (s->seq > latest_seq) {
                memcpy(latest, out, result_grid_bytes);
                latest_seq = s->seq;
                pthread_cond_signal(&cv_res);
            }
        } else st.bad++;
        s->state = S_FREE;
        pthread_mutex_unlock(&mu);
    }
    free(out);
    if (active_analyzer->destroy_context) active_analyzer->destroy_context(context);
    return NULL;
}

int pipeline_start(const frame_analyzer_t *analyzer, int worker_count, int queue_depth,
                    int grid_width, int grid_height) {
    active_analyzer = analyzer;
    grid_w = grid_width; grid_h = grid_height;
    result_grid_bytes = analyzer->result_cell_size * (size_t)grid_w * grid_h;
    nslots = queue_depth;
    slots = calloc(nslots, sizeof(slot_t));
    latest = calloc(1, result_grid_bytes);
    if (!slots || !latest) { perror("pipeline buffers"); return -1; }
    worker_threads = malloc(worker_count * sizeof(pthread_t));
    worker_thread_count = worker_count;
    for (int i = 0; i < worker_count; i++)
        pthread_create(&worker_threads[i], NULL, worker_main, NULL);
    return 0;
}

void pipeline_stop(void) {
    pthread_mutex_lock(&mu);
    pthread_cond_broadcast(&cv_job);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < worker_thread_count; i++) pthread_join(worker_threads[i], NULL);
    free(worker_threads);
    for (int i = 0; i < nslots; i++) free(slots[i].buf);
    free(slots);
    free(latest);
}

void pipeline_submit_frame(const uint8_t *data, size_t len) {
    pthread_mutex_lock(&mu);
    st.cap++; st.bytes += len;
    slot_t *s = NULL;
    for (int i = 0; i < nslots; i++) if (slots[i].state == S_FREE) { s = &slots[i]; break; }
    if (s) {
        if (s->cap < len) { s->buf = realloc(s->buf, len); s->cap = len; }
        memcpy(s->buf, data, len);
        s->len = len; s->seq = ++next_seq;
        s->state = S_QUEUED;
        pthread_cond_signal(&cv_job);
    } else st.drop++;
    pthread_mutex_unlock(&mu);
}

uint64_t pipeline_wait_latest(void *out, uint64_t last_seen) {
    pthread_mutex_lock(&mu);
    if (latest_seq == last_seen) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&cv_res, &mu, &ts);
    }
    uint64_t seq = last_seen;
    if (latest_seq != last_seen) {
        memcpy(out, latest, result_grid_bytes);
        seq = latest_seq;
    }
    pthread_mutex_unlock(&mu);
    return seq;
}

pipeline_stats_t pipeline_take_stats(void) {
    pthread_mutex_lock(&mu);
    pipeline_stats_t s = st;
    memset(&st, 0, sizeof st);
    pthread_mutex_unlock(&mu);
    return s;
}

void pipeline_note_shown(void) {
    pthread_mutex_lock(&mu);
    st.shown++;
    pthread_mutex_unlock(&mu);
}
