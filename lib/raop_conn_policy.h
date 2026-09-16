/**
 * UxPlay - An open-source AirPlay mirroring server
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef RAOP_CONN_POLICY_H
#define RAOP_CONN_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decides whether conn_request() (raop.c) should tear down an existing
 * RAOP-type connection's audio/mirror/NTP services when a new
 * AIRPLAY-type connection is being classified. See docs/audio-pipeline.md
 * for the full connection-classification context.
 *
 * Extracted as its own pure, dependency-free function (rather than left
 * inline in conn_request()) specifically so it can be unit-tested in
 * complete isolation -- no GStreamer, no httpd_t/raop_t mocking, no
 * hardware -- see tests/test_raop_conn_policy.c.
 *
 * Returns true (matching upstream's original always-teardown behavior)
 * whenever the inputs don't give a confident "same client" answer: NULL
 * pointers, or differing address lengths (e.g. one IPv4-mapped, one
 * genuinely IPv6) are treated as "can't tell, assume different client" --
 * this fix only narrows the *specific*, confidently-identical-address
 * case, it never makes the existing behavior more permissive than
 * upstream in any case it doesn't recognize.
 *
 * Compares raw address bytes, not formatted strings (e.g. not
 * utils_ipaddress_to_string() output) -- avoids depending on that
 * function's formatting and both callers already have the raw bytes on
 * hand (raop_conn_t's own `remote`/`remotelen` fields). */
bool raop_should_teardown_existing_connection(const unsigned char *existing_remote,
                                               int existing_remotelen,
                                               const unsigned char *new_remote,
                                               int new_remotelen);

#ifdef __cplusplus
}
#endif

#endif //RAOP_CONN_POLICY_H
