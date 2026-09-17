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
 * RAOP connection's audio/mirror/NTP services for a new AIRPLAY
 * connection. Returns true (original always-teardown behavior) unless
 * both addresses confidently match -- never more permissive than that. */
bool raop_should_teardown_existing_connection(const unsigned char *existing_remote,
                                               int existing_remotelen,
                                               const unsigned char *new_remote,
                                               int new_remotelen);

#ifdef __cplusplus
}
#endif

#endif //RAOP_CONN_POLICY_H
