/* -efifo must never make a session wait on its consumer, and its delivered
 * lines must strictly alternate even when two threads emit at once. Handlers
 * are unset on purpose: blocking dies of SIGALRM, a lost reader of SIGPIPE. */
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../event_fifo.h"

/* More than a pipe's 64KB capacity of events (26 bytes per begin/end
 * pair), so the no-reader case really does hit the drop path. */
#define FLOOD_PAIRS 10000
/* begin/end pairs per racing thread, and the least the drained stream must
 * contain for the alternation check to mean anything. */
#define RACE_EMITS 100000
#define RACE_MIN_DELIVERED 100000

static char path_a[128];
static char path_b[128];
static char path_c[128];
static char path_file[128];

static int delivered_active = -1;
static long delivered_lines = 0;
static char partial[64];
static size_t partial_len = 0;
static long log_drops = 0;
static long log_not_fifo = 0;

/* A dropped or refused event must be observable, not silently swallowed. */
static void
count_logs(void *cls, int level, const char *msg)
{
    (void) cls;
    if (level != LOGGER_ERR) {
        return;
    }
    if (strstr(msg, "dropped")) {
        log_drops++;
    } else if (strstr(msg, "not a fifo")) {
        log_not_fifo++;
    }
}

/* Splits drained bytes into lines and rejects anything but a strict
 * begin/end alternation starting at begin -- a torn or inverted write. */
static void
check_delivered(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n') {
            if (partial_len < sizeof(partial) - 1) {
                partial[partial_len++] = buf[i];
            }
            continue;
        }
        partial[partial_len] = '\0';
        partial_len = 0;
        int active = -1;
        if (strcmp(partial, "session-begin") == 0) {
            active = 1;
        } else if (strcmp(partial, "session-end") != 0) {
            fprintf(stderr, "FAIL: line %ld is garbled: \"%s\"\n", delivered_lines + 1, partial);
            exit(1);
        } else {
            active = 0;
        }
        if (active == delivered_active) {
            fprintf(stderr, "FAIL: line %ld repeats \"%s\": delivered order inverted\n",
                    delivered_lines + 1, partial);
            exit(1);
        }
        if (delivered_active < 0 && active != 1) {
            fprintf(stderr, "FAIL: first delivered line is \"%s\", expected session-begin\n", partial);
            exit(1);
        }
        delivered_active = active;
        delivered_lines++;
    }
}

static void *
emit_race_thread(void *arg)
{
    for (long i = 0; i < RACE_EMITS; i++) {
        event_fifo_session_begin();
        event_fifo_session_end();
    }
    atomic_fetch_add((atomic_int *) arg, 1);
    return NULL;
}

static void
read_expect(int fd, const char *expected)
{
    char buf[256];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        n = 0;
    }
    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "FAIL: read %zd bytes \"%s\", expected \"%s\"\n", n, buf, expected);
        exit(1);
    }
}

int
main(void)
{
    snprintf(path_a, sizeof(path_a), "/tmp/uxplay-efifo-test-%d-a", (int) getpid());
    snprintf(path_b, sizeof(path_b), "/tmp/uxplay-efifo-test-%d-b", (int) getpid());
    snprintf(path_c, sizeof(path_c), "/tmp/uxplay-efifo-test-%d-c", (int) getpid());
    snprintf(path_file, sizeof(path_file), "/tmp/uxplay-efifo-test-%d-file", (int) getpid());
    unlink(path_a);
    unlink(path_b);
    unlink(path_c);
    unlink(path_file);
    alarm(60);

    logger_t *logger = logger_init();
    logger_set_callback(logger, count_logs, NULL);
    logger_set_level(logger, LOGGER_ERR);

    /* A regular file at the path would accept events forever, unbounded and
     * without the write atomicity the protocol needs. */
    int plain = open(path_file, O_WRONLY | O_CREAT, 0600);
    assert(plain >= 0);
    close(plain);
    assert(event_fifo_open(logger, path_file) < 0);
    assert(log_not_fifo == 1);

    /* No consumer has ever attached: open and every emit must still
     * complete, with the events dropped -- loudly, but harmlessly. */
    assert(event_fifo_open(logger, path_a) == 0);
    for (int i = 0; i < FLOOD_PAIRS; i++) {
        event_fifo_session_begin();
        event_fifo_session_end();
    }
    assert(log_drops > 0);

    assert(event_fifo_open(NULL, path_b) == 0);
    int reader = open(path_b, O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);

    /* A repeated transition must collapse, so one real session is exactly
     * one begin and one end on the wire. */
    event_fifo_session_begin();
    event_fifo_session_begin();
    event_fifo_session_end();
    event_fifo_session_end();
    read_expect(reader, "session-begin\nsession-end\n");

    /* Consumer exits mid-session: emits must keep completing, and a
     * replacement consumer must still get later events. */
    event_fifo_session_begin();
    read_expect(reader, "session-begin\n");
    close(reader);
    event_fifo_session_end();
    event_fifo_session_begin();

    reader = open(path_b, O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);
    read_expect(reader, "session-end\nsession-begin\n");
    close(reader);

    /* Two threads racing on the same transitions: the state test and the
     * write must be one critical section, or a pair inverts on the wire. */
    assert(event_fifo_open(NULL, path_c) == 0);
    reader = open(path_c, O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);
    atomic_int emitters_done = 0;
    pthread_t race_tid[2];
    assert(pthread_create(&race_tid[0], NULL, emit_race_thread, &emitters_done) == 0);
    assert(pthread_create(&race_tid[1], NULL, emit_race_thread, &emitters_done) == 0);
    for (;;) {
        /* Sampled before the read, so an empty read after both emitters
         * finished really means the FIFO is drained. */
        int done = (atomic_load(&emitters_done) == 2);
        char buf[4096];
        ssize_t n = read(reader, buf, sizeof(buf));
        if (n > 0) {
            check_delivered(buf, (size_t) n);
        } else if (done) {
            break;
        }
    }
    assert(pthread_join(race_tid[0], NULL) == 0);
    assert(pthread_join(race_tid[1], NULL) == 0);
    close(reader);
    if (delivered_lines < RACE_MIN_DELIVERED) {
        fprintf(stderr, "FAIL: only %ld lines delivered under contention, need %d\n",
                delivered_lines, RACE_MIN_DELIVERED);
        exit(1);
    }

    event_fifo_close();
    logger_destroy(logger);
    unlink(path_a);
    unlink(path_b);
    unlink(path_c);
    unlink(path_file);
    printf("PASS: %d flooded event pairs with no reader (%ld drops logged), "
           "collapse and reader-exit paths clean, %ld lines strictly "
           "alternating under %d contending event pairs per thread\n",
           FLOOD_PAIRS, log_drops, delivered_lines, RACE_EMITS);
    return 0;
}
