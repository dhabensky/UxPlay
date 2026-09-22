/* on_url()'s post-URL protocol read must never go past the bytes actually
 * fed to http_request_add_data() -- sweeps request-line split points with
 * a poison-filled buffer past the fed data. Exit 0 = PASS. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../lib/http_request.h"

/* Recognizable stand-in for "some other connection's stale stack bytes"
 * sitting just past the real data in a reused buffer -- never a valid
 * protocol-string byte, so any leak is unambiguous. */
#define POISON 0x5A

static const char full_req[] = "GET / RTSP/1.0\r\nCSeq: 1\r\n\r\n";
/* Index of 'R' in full_req -- one past the "GET / " method+URL+delimiter
 * prefix, i.e. where request->protocol's bytes should start. */
#define PROTO_START 6

static int
check_split(int datalen)
{
    char buf[64];
    memset(buf, POISON, sizeof(buf));
    memcpy(buf, full_req, (size_t) datalen);

    http_request_t *req = http_request_init();
    assert(req);
    http_request_add_data(req, buf, datalen);

    const char *protocol = http_request_get_protocol(req);

    int avail = datalen - PROTO_START;
    if (avail < 0) avail = 0;
    if (avail > 8) avail = 8;

    char expected[9] = {0};
    memcpy(expected, full_req + PROTO_START, (size_t) avail);

    int ok = (memcmp(protocol, expected, 9) == 0);
    if (!ok) {
        fprintf(stderr, "datalen=%d: FAIL protocol mismatch, avail=%d, got:", datalen, avail);
        for (int i = 0; i < 9; i++) fprintf(stderr, " %02x", (unsigned char) protocol[i]);
        fprintf(stderr, "\n");
    }

    for (int i = 0; i < 9; i++) {
        if ((unsigned char) protocol[i] == POISON) {
            fprintf(stderr, "datalen=%d: FAIL poison byte leaked into protocol[%d]\n", datalen, i);
            ok = 0;
        }
    }

    http_request_destroy(req);
    return ok;
}

int
main(void)
{
    int splits[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, (int) sizeof(full_req) - 1};
    int n = (int) (sizeof(splits) / sizeof(splits[0]));
    int failures = 0;

    for (int i = 0; i < n; i++) {
        if (!check_split(splits[i])) {
            failures++;
        }
    }

    if (failures) {
        fprintf(stderr, "test_on_url_protocol_bounds: FAIL (%d/%d split points)\n", failures, n);
        return 1;
    }
    printf("test_on_url_protocol_bounds: PASS (%d split points)\n", n);
    return 0;
}
