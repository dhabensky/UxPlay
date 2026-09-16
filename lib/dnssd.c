/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *=================================================================
 * modified by fduncanh 2022
 */


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>

#include "dnssd.h"

#include "dnssdint.h"
#include "utils.h"

#if defined(__APPLE__) && defined(UXPLAY_HAVE_APPLE_P2P)
/* p2p support (macOS only) */
#include "netutils.h"
void dnssd_set_peer_to_peer(dnssd_t *dnssd, int enabled) {
    assert(dnssd);
    dnssd->peer_to_peer = enabled ? 1 : 0;
    /* Warning: Advertising without the macOS host set to accept traffic from P2P interfaces produces a
     * receiver that clients can see but cannot connect to. */
    netutils_set_peer_to_peer(dnssd->peer_to_peer);
}
#endif

dnssd_t *
dnssd_init(const char* name, int name_len, const char* hw_addr, int hw_addr_len, unsigned char pin_pw, int *error)
{
    /* pin_pw = 0: no pin or password
                1: use onscreen pin for client access control
                2 or 3: require password for client access control  
    */
    const char *dot_local = ".local";
  
    if (error) *error = DNSSD_ERROR_NOERROR;
  
    /* first verify that  name is a null-terminated string with strlen = name_len, and is not ".local" */
    if (*(name + name_len) != '\0' || name_len != (int) strlen(name) || !strcmp(name, dot_local)) {
        if (error) *error = DNSSD_ERROR_BADNAME;
        return NULL;
    }

    /* use only that part of name before any substring ".local"
       (this fixes  namespace issues when a custom mDNSResponder is used on macOS) */
    const char *dot_local_start = strstr(name, dot_local);
    if (dot_local_start) {
        name_len = dot_local_start - name;
    }

    dnssd_t *dnssd = (dnssd_t *) calloc(1, sizeof(dnssd_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    dnssd->pin_pw = pin_pw;

    char *end = NULL;
    unsigned long features  = strtoul(FEATURES_1, &end, 16);
    if (!end || (features & 0xFFFFFFFF) != features) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_BADFEATURES;
        return NULL;
    } 
    dnssd->features1 = (uint32_t) features;

    features  = strtoul(FEATURES_2, &end, 16);
    if (!end || (features & 0xFFFFFFFF) != features) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_BADFEATURES;
        return NULL;
    } 
    dnssd->features2 = (uint32_t) features;

    dnssd->name_len = name_len;
    dnssd->name = calloc(1, name_len + 1);
    if (!dnssd->name) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    memcpy(dnssd->name, name, name_len);

    dnssd->hw_addr_len = hw_addr_len;
    dnssd->hw_addr = calloc(1, dnssd->hw_addr_len);
    if (!dnssd->hw_addr) {
        free(dnssd->name);
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    memcpy(dnssd->hw_addr, hw_addr, hw_addr_len);

    dnssd->dnssd_private = NULL;
    dnssd->dnssd_private = dnssd_private_init(dnssd, error);
    if (!dnssd->dnssd_private) {
        free(dnssd->hw_addr);
        free(dnssd->name);
        free(dnssd);
        return NULL;
    }

    return dnssd;
}

void
dnssd_destroy(dnssd_t *dnssd)
{
    if (dnssd) {
        if (dnssd->hw_addr) {
            free(dnssd->hw_addr);
        }
        if (dnssd->name) {
            free(dnssd->name);
        }
        if (dnssd->dnssd_private) {
            dnssd_private_destroy(dnssd->dnssd_private);
        }

        free(dnssd);
    }
}

const char *
dnssd_get_name(dnssd_t *dnssd, int *length)
{
    *length = dnssd->name_len;
    return dnssd->name;
}

const char *
dnssd_get_hw_addr(dnssd_t *dnssd, int *length)
{
    *length = dnssd->hw_addr_len;
    return dnssd->hw_addr;
}

int
dnssd_reregister(dnssd_t *dnssd, unsigned short raop_port, unsigned short airplay_port)
{
    /* Built purely from the public unregister/register API, not backend
     * internals (dnssd_t's raop_service/TXTRecordDeallocate-style fields
     * are private to each backend -- lib/dns_sd/dns_sd.c and
     * lib/mdnsd/dnssd_mdnsd.c -- and unreachable from this file). Safe to
     * compose this way on both backends: neither's unregister_raop/
     * airplay frees dnssd->name/hw_addr as a side effect (unlike the
     * pre-refactor monolithic dnssd.c this was originally written
     * against, where that free -- fine when followed by dnssd_destroy(),
     * a real use-after-free otherwise -- crashed the process with
     * SIGABRT every ~5 minutes when called from a periodic refresh
     * instead), and both backends' register_raop/airplay restart their
     * underlying daemon/connection unconditionally, so calling them again
     * after a full unregister is a normal, supported sequence. */
    assert(dnssd);
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
    int err = dnssd_register_raop(dnssd, raop_port);
    if (err) return err;
    return dnssd_register_airplay(dnssd, airplay_port);
}

uint64_t dnssd_get_airplay_features(dnssd_t *dnssd) {
    uint64_t features = ((uint64_t) dnssd->features2) << 32;
    features += (uint64_t) dnssd->features1;
    return features;
}

void dnssd_set_pk(dnssd_t *dnssd, char * pk_str) {
    dnssd->pk = pk_str;
}

void dnssd_set_airplay_features(dnssd_t *dnssd, int bit, int val) {
    uint32_t mask = 0;
    uint32_t *features = 0;
    if (bit < 0 || bit > 63) return;
    if (val < 0 || val > 1) return;
    if (bit >= 32) {
        mask = 0x1 << (bit - 32);
        features = &(dnssd->features2);
    } else {
        mask = 0x1 << bit;
        features = &(dnssd->features1);
    }
    if (val) {
        *features = *features | mask;
    } else {
        *features = *features & ~mask;
    }
}
