/**
 * UxPlay - An open-source AirPlay mirroring server
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include "raop_conn_policy.h"

bool raop_should_teardown_existing_connection(const unsigned char *existing_remote,
                                               int existing_remotelen,
                                               const unsigned char *new_remote,
                                               int new_remotelen) {
    if (!existing_remote || !new_remote || existing_remotelen <= 0 || new_remotelen <= 0) {
        return true; /* can't tell -- fall back to upstream's original always-teardown behavior */
    }
    if (existing_remotelen != new_remotelen) {
        return true; /* different address family/length -- treat as a different client */
    }
    return memcmp(existing_remote, new_remote, (size_t) existing_remotelen) != 0;
}
