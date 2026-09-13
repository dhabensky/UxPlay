/* Regression/unit test for raop_should_teardown_existing_connection()
 * (lib/raop_conn_policy.c) -- the 2026-09-13 fix for
 * bugs/2026-09-13-audio-dies-on-repeated-track-switch-setup.md.
 *
 * Deliberately depends on NOTHING beyond the one function under test --
 * no GStreamer, no hardware, no network, no httpd_t/raop_t mocking. This
 * is exactly why the fix logic was extracted into its own tiny,
 * dependency-free file rather than left inline in raop.c's much larger,
 * more entangled conn_request(): raop.c pulls in raop_handlers.h (huge)
 * plus pairing/fairplay/httpd, which would make a directly-included-.c
 * test (the pattern test_bus_callback_null_renderer.c uses) impractical
 * here.
 *
 * Exit code 0 = PASS, non-zero = FAIL (asserts abort the process). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../lib/raop_conn_policy.h"

static const unsigned char ipv4_a[4] = {192, 168, 1, 34};
static const unsigned char ipv4_b[4] = {192, 168, 1, 77};
static const unsigned char ipv6_a[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0xba, 0x27, 0xeb, 0xff, 0xfe, 0xba, 0xc1, 0x05};

int main(void) {
    /* Same address, same length -- the actual bug scenario: a second
     * connection from the same client should NOT tear down the existing
     * one's audio/mirror/NTP services. */
    assert(raop_should_teardown_existing_connection(ipv4_a, 4, ipv4_a, 4) == false);
    {
        unsigned char copy[4];
        memcpy(copy, ipv4_a, 4);
        /* Same bytes, but a genuinely different buffer -- must compare
         * content, not pointer identity. */
        assert(raop_should_teardown_existing_connection(ipv4_a, 4, copy, 4) == false);
    }

    /* Different address, same length -- a genuinely different client;
     * upstream's original preemption behavior must be preserved. */
    assert(raop_should_teardown_existing_connection(ipv4_a, 4, ipv4_b, 4) == true);

    /* Different lengths (e.g. IPv4 vs IPv6) -- can't confidently say
     * "same client", must default to upstream's original teardown. */
    assert(raop_should_teardown_existing_connection(ipv4_a, 4, ipv6_a, 16) == true);

    /* Same IPv6 address, same length -- also must be recognized as same
     * client (not just the IPv4 case). */
    assert(raop_should_teardown_existing_connection(ipv6_a, 16, ipv6_a, 16) == false);

    /* Missing/invalid inputs must fail toward the safe, original
     * (always-teardown) behavior, never toward the new permissive one. */
    assert(raop_should_teardown_existing_connection(NULL, 4, ipv4_a, 4) == true);
    assert(raop_should_teardown_existing_connection(ipv4_a, 4, NULL, 4) == true);
    assert(raop_should_teardown_existing_connection(ipv4_a, 0, ipv4_a, 0) == true);
    assert(raop_should_teardown_existing_connection(ipv4_a, -1, ipv4_a, 4) == true);

    printf("PASS: raop_should_teardown_existing_connection all cases correct\n");
    return 0;
}
