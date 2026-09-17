/**
 * synthetic-client: a minimal AirPlay RTSP/RTP client that drives the
 * real uxplay server, as a genuinely separate process, over loopback.
 * Links libairplay.a for real FairPlay/AES primitives. Audio-only.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sstream>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <plist/plist.h>

#include "../lib/crypto.h"
#include "../lib/fairplay.h"

namespace {

std::string g_host = "127.0.0.1";
unsigned short g_port = 0;

double now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

/* Send one RTSP request with an optional binary-plist body over `sock` and
 * read back the response body (blocks until Content-Length bytes arrive).
 * Returns false on any socket error. */
bool rtsp_request(int sock, const char *method, const char *url, int cseq,
                   const unsigned char *body, size_t body_len,
                   std::vector<unsigned char> &resp_body) {
    std::ostringstream req;
    req << method << " " << url << " RTSP/1.0\r\n";
    req << "CSeq: " << cseq << "\r\n";
    req << "DACP-ID: 0000000000000001\r\n";
    req << "Active-Remote: 123456789\r\n";
    req << "User-Agent: UxPlaySyntheticClient/1.0\r\n";
    if (body_len) {
        req << "Content-Type: application/x-apple-binary-plist\r\n";
        req << "Content-Length: " << body_len << "\r\n";
    }
    req << "\r\n";
    std::string reqstr = req.str();
    if (send(sock, reqstr.data(), reqstr.size(), 0) < 0) return false;
    if (body_len && send(sock, body, body_len, 0) < 0) return false;

    std::string headers;
    char c;
    while (headers.size() < 4 || headers.compare(headers.size() - 4, 4, "\r\n\r\n") != 0) {
        ssize_t n = recv(sock, &c, 1, 0);
        if (n <= 0) return false;
        headers += c;
        if (headers.size() > 8192) return false; /* sanity limit */
    }
    size_t clpos = headers.find("Content-Length:");
    long content_length = (clpos != std::string::npos) ? atol(headers.c_str() + clpos + 16) : 0;
    resp_body.assign((size_t) content_length, 0);
    size_t got = 0;
    while ((long) got < content_length) {
        ssize_t n = recv(sock, resp_body.data() + got, content_length - got, 0);
        if (n <= 0) return false;
        got += (size_t) n;
    }
    return true;
}

void plist_to_bytes(plist_t node, std::vector<unsigned char> &out) {
    char *data = NULL;
    uint32_t len = 0;
    plist_to_bin(node, &data, &len);
    out.assign(data, data + len);
    free(data);
}

/* A real AAC-ELD frame (captured via -capture), so avdec_aac has
 * genuine content to decode instead of fake bytes. */
const unsigned char kRealAacEldFrame[267] = {
    0x8d, 0xff, 0xff, 0xff, 0xe0, 0xf4, 0x96, 0x6a, 0x54, 0x31, 0x88, 0x82, 0x00, 0xbe, 0xe0, 0xd0,
    0x49, 0xb4, 0x13, 0xaa, 0x71, 0x4b, 0x5a, 0xee, 0xdf, 0x6c, 0xed, 0xae, 0x53, 0xb5, 0xd8, 0x44,
    0x64, 0xfe, 0x25, 0x06, 0x7c, 0x04, 0x1e, 0x25, 0xfe, 0xf3, 0x29, 0x3d, 0xf7, 0x73, 0x36, 0xd2,
    0x46, 0x73, 0xd9, 0x3a, 0x2d, 0x26, 0xbf, 0xd2, 0xca, 0x92, 0xce, 0x71, 0x59, 0x4e, 0x1f, 0xa7,
    0x59, 0xed, 0x49, 0x1d, 0x50, 0x3e, 0xd1, 0x30, 0x99, 0x25, 0x21, 0xa1, 0xde, 0xf9, 0x3c, 0x7d,
    0x22, 0x3b, 0xf4, 0x1b, 0xfa, 0x91, 0x8c, 0xde, 0xdd, 0x65, 0x44, 0x81, 0x3b, 0x0b, 0x1c, 0xa4,
    0xf4, 0x9a, 0x6b, 0x5c, 0x1a, 0xda, 0xed, 0xeb, 0xc9, 0x14, 0xf2, 0xcf, 0x36, 0x9c, 0xf1, 0xe2,
    0x10, 0xa1, 0xce, 0xda, 0x60, 0x74, 0x18, 0xe5, 0xbe, 0x4b, 0x44, 0x2d, 0x24, 0xd2, 0x11, 0xea,
    0xce, 0x03, 0xe9, 0x19, 0x90, 0x73, 0x27, 0x56, 0xd1, 0x0b, 0xc7, 0x25, 0xf9, 0x73, 0x79, 0x55,
    0x5f, 0x32, 0x9f, 0x3e, 0x14, 0x3f, 0x2b, 0x14, 0x06, 0x85, 0xcf, 0x61, 0x4f, 0x91, 0x5d, 0x3a,
    0x21, 0x93, 0xeb, 0xe1, 0x65, 0x50, 0xb5, 0xd3, 0x2e, 0x3d, 0x92, 0xe9, 0x6c, 0xb7, 0xcf, 0xce,
    0x43, 0x1e, 0xfa, 0x1f, 0xa8, 0xa8, 0x88, 0x33, 0x1f, 0x80, 0x28, 0x13, 0x38, 0xc3, 0x09, 0x40,
    0x8a, 0x34, 0xdf, 0x67, 0xfa, 0xbe, 0x9f, 0x88, 0x85, 0x05, 0xa2, 0x10, 0x24, 0x03, 0xf9, 0x44,
    0x40, 0x17, 0x9b, 0x3c, 0x7e, 0xaf, 0xe3, 0xf7, 0x4a, 0xaf, 0x01, 0x0a, 0x8f, 0xf9, 0xc3, 0x13,
    0xf5, 0xae, 0x3a, 0xfc, 0x61, 0x93, 0xf6, 0x40, 0xed, 0x81, 0x0e, 0xf6, 0x1f, 0xbb, 0xd7, 0x8e,
    0x35, 0x9f, 0x5f, 0xc7, 0xda, 0x21, 0x0d, 0x8b, 0xd5, 0x34, 0x9e, 0x21, 0xb5, 0x00, 0x22, 0x72,
    0x80, 0xaf, 0xf6, 0x7e, 0xf3, 0xb7, 0xe3, 0x9a, 0xe4, 0xc0, 0x00
};

/* One AES-128-CBC-encrypted RTP audio packet, matching
 * raop_buffer_decrypt()'s expected wire format exactly. */
ssize_t send_audio_packet(int sock, struct sockaddr_in *dest, unsigned short seqnum,
                           uint32_t rtp_ts, aes_ctx_t *enc_ctx) {
    const unsigned char *plain = kRealAacEldFrame;
    const size_t plain_len = sizeof(kRealAacEldFrame);
    const size_t encrypted_len = (plain_len / 16) * 16;
    unsigned char packet[12 + sizeof(kRealAacEldFrame)];
    packet[0] = 0x80; packet[1] = 0x60;
    packet[2] = (unsigned char) (seqnum >> 8); packet[3] = (unsigned char) seqnum;
    packet[4] = (unsigned char) (rtp_ts >> 24); packet[5] = (unsigned char) (rtp_ts >> 16);
    packet[6] = (unsigned char) (rtp_ts >> 8);  packet[7] = (unsigned char) rtp_ts;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    aes_cbc_encrypt(enc_ctx, plain, packet + 12, (int) encrypted_len);
    aes_cbc_reset(enc_ctx);
    memcpy(packet + 12 + encrypted_len, plain + encrypted_len, plain_len - encrypted_len);
    return sendto(sock, packet, (size_t) (12 + plain_len), 0, (struct sockaddr *) dest, sizeof(*dest));
}

/* RTCP sync packet (type 0x54), sent to the SETUP-response controlPort
 * to establish the RTP-timestamp<->NTP-time mapping (wire format per
 * lib/raop_rtp.c's own parsing). */
void send_sync_packet(int sock, struct sockaddr_in *dest, bool first,
                       uint32_t sync_rtp, uint64_t ntp_seconds_since_epoch) {
    unsigned char packet[20] = {0};
    packet[0] = first ? 0x90 : 0x80;
    packet[1] = 0xd4;
    packet[2] = 0x00; packet[3] = 0x04;
    packet[4] = (unsigned char) (sync_rtp >> 24); packet[5] = (unsigned char) (sync_rtp >> 16);
    packet[6] = (unsigned char) (sync_rtp >> 8);  packet[7] = (unsigned char) sync_rtp;
    uint64_t ntp_raw = ntp_seconds_since_epoch << 32; /* zero fraction */
    for (int i = 0; i < 8; i++) packet[8 + i] = (unsigned char) (ntp_raw >> (56 - 8 * i));
    uint32_t next_rtp = sync_rtp + 7497;
    packet[16] = (unsigned char) (next_rtp >> 24); packet[17] = (unsigned char) (next_rtp >> 16);
    packet[18] = (unsigned char) (next_rtp >> 8);  packet[19] = (unsigned char) next_rtp;
    sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *) dest, sizeof(*dest));
}

int connect_rtsp() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = inet_addr(g_host.c_str());
    if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fprintf(stderr, "synthetic-client: connect to %s:%u failed: %s\n", g_host.c_str(), g_port, strerror(errno));
        close(sock);
        return -1;
    }
    return sock;
}

/* Common connection bring-up shared by every mode: GET /info, the two
 * fp-setup messages, then derive myfp (offline, no RSA needed). */
bool handshake(int sock, int &cseq, fairplay_t **out_myfp) {
    std::vector<unsigned char> resp;
    rtsp_request(sock, "GET", "/info", cseq++, NULL, 0, resp);
    unsigned char fp1[16] = {0};
    fp1[4] = 0x03; fp1[14] = 0x00;
    rtsp_request(sock, "POST", "/fp-setup", cseq++, fp1, sizeof(fp1), resp);
    unsigned char fp2[164];
    get_random_bytes(fp2, sizeof(fp2));
    fp2[4] = 0x03;
    rtsp_request(sock, "POST", "/fp-setup", cseq++, fp2, sizeof(fp2), resp);
    fairplay_t *myfp = fairplay_init(NULL);
    unsigned char scratch142[142], scratch32[32];
    fairplay_setup(myfp, fp1, scratch142);
    fairplay_handshake(myfp, fp2, scratch32);
    *out_myfp = myfp;
    return true;
}

void teardown(int sock, int &cseq) {
    plist_t td = plist_new_dict();
    plist_t td_streams = plist_new_array();
    plist_t td_stream = plist_new_dict();
    plist_dict_set_item(td_stream, "type", plist_new_uint(96));
    plist_array_append_item(td_streams, td_stream);
    plist_dict_set_item(td, "streams", td_streams);
    std::vector<unsigned char> td_body;
    plist_to_bytes(td, td_body);
    plist_free(td);
    std::vector<unsigned char> td_resp;
    rtsp_request(sock, "TEARDOWN", "rtsp://127.0.0.1/1", cseq++, td_body.data(), td_body.size(), td_resp);
}

/* SETUP a stream. controlPort=0 keeps no_resend=true; non-zero opts
 * into the resend-wait path. Returns (dataPort<<32)|controlPort. */
uint64_t do_setup(int sock, int &cseq, bool first_setup, unsigned short my_control_port,
                   const unsigned char *ekey, const unsigned char *eiv) {
    plist_t root = plist_new_dict();
    if (first_setup) {
        plist_dict_set_item(root, "ekey", plist_new_data((const char *) ekey, 72));
        plist_dict_set_item(root, "eiv", plist_new_data((const char *) eiv, 16));
        plist_dict_set_item(root, "deviceID", plist_new_string("11:22:33:44:55:66"));
        plist_dict_set_item(root, "timingProtocol", plist_new_string("None"));
        plist_dict_set_item(root, "timingPort", plist_new_uint(0));
    }
    plist_t streams = plist_new_array();
    plist_t stream = plist_new_dict();
    plist_dict_set_item(stream, "type", plist_new_uint(96));
    plist_dict_set_item(stream, "ct", plist_new_uint(8));
    plist_dict_set_item(stream, "spf", plist_new_uint(480));
    plist_dict_set_item(stream, "controlPort", plist_new_uint(my_control_port));
    plist_dict_set_item(stream, "audioFormat", plist_new_uint(0x1000000));
    plist_dict_set_item(stream, "isMedia", plist_new_bool(1));
    plist_dict_set_item(stream, "usingScreen", plist_new_bool(0));
    plist_array_append_item(streams, stream);
    plist_dict_set_item(root, "streams", streams);
    std::vector<unsigned char> body;
    plist_to_bytes(root, body);
    plist_free(root);

    std::vector<unsigned char> setup_resp;
    if (!rtsp_request(sock, "SETUP", "rtsp://127.0.0.1/1", cseq++, body.data(), body.size(), setup_resp)) {
        return 0;
    }
    plist_t resp_plist = NULL;
    plist_from_bin((const char *) setup_resp.data(), (uint32_t) setup_resp.size(), &resp_plist);
    uint64_t dport = 0, cport = 0;
    if (resp_plist) {
        plist_t rstreams = plist_dict_get_item(resp_plist, "streams");
        if (rstreams && plist_array_get_size(rstreams) > 0) {
            plist_t rstream0 = plist_array_get_item(rstreams, 0);
            plist_t dport_node = plist_dict_get_item(rstream0, "dataPort");
            plist_t cport_node = plist_dict_get_item(rstream0, "controlPort");
            if (dport_node) plist_get_uint_val(dport_node, &dport);
            if (cport_node) plist_get_uint_val(cport_node, &cport);
        }
        plist_free(resp_plist);
    }
    if (!dport || !cport) return 0;
    return (dport << 32) | cport;
}

/* --- mode: threadtest --------------------------------------------------
 * Repeated SETUP/audio/TEARDOWN cycles over one connection, exercising
 * the real multi-threaded server architecture end to end. */
int mode_threadtest(int cycles, int gap_s) {
    fprintf(stderr, "threadtest: connecting to %s:%u\n", g_host.c_str(), g_port);
    int sock = connect_rtsp();
    if (sock < 0) return 1;

    int cseq = 1;
    fairplay_t *myfp = NULL;
    handshake(sock, cseq, &myfp);

    for (int cycle = 0; cycle < cycles; cycle++) {
        unsigned char ekey[72], eiv[16], aeskey[16];
        get_random_bytes(ekey, sizeof(ekey));
        get_random_bytes(eiv, sizeof(eiv));
        fairplay_decrypt(myfp, ekey, aeskey);

        fprintf(stderr, "threadtest: cycle %d SEND-SETUP t=%.6f\n", cycle, now_s());
        uint64_t ports = do_setup(sock, cseq, cycle == 0, 0, ekey, eiv);
        fprintf(stderr, "threadtest: cycle %d RECV-SETUP-response t=%.6f\n", cycle, now_s());
        if (!ports) {
            fprintf(stderr, "threadtest: cycle %d got no dataPort/controlPort in SETUP response, aborting\n", cycle);
            break;
        }
        unsigned short dport = (unsigned short) (ports >> 32);
        unsigned short cport = (unsigned short) ports;

        /* A sync packet is required before audio can render (initial_sync
         * cold-start guard). Sent twice: UDP has no ACK/retry. */
        struct sockaddr_in csync_dest;
        memset(&csync_dest, 0, sizeof(csync_dest));
        csync_dest.sin_family = AF_INET;
        csync_dest.sin_port = htons(cport);
        csync_dest.sin_addr.s_addr = inet_addr(g_host.c_str());
        int csock = socket(AF_INET, SOCK_DGRAM, 0);
        send_sync_packet(csock, &csync_dest, cycle == 0, (uint32_t) (cycle * 1000 * 480), (uint64_t) time(NULL));
        usleep(20000);
        send_sync_packet(csock, &csync_dest, cycle == 0, (uint32_t) (cycle * 1000 * 480), (uint64_t) time(NULL));
        close(csock);
        fprintf(stderr, "threadtest: cycle %d SENT-SYNC t=%.6f\n", cycle, now_s());
        usleep(100000); /* let the server process the sync packet */

        int asock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(dport);
        dest.sin_addr.s_addr = inet_addr(g_host.c_str());
        aes_ctx_t *enc = aes_cbc_init(aeskey, eiv, AES_ENCRYPT);
        fprintf(stderr, "threadtest: cycle %d FIRST-AUDIO-PACKET t=%.6f (dataPort=%u)\n", cycle, now_s(), (unsigned) dport);
        for (int p = 0; p < 20; p++) {
            send_audio_packet(asock, &dest, (unsigned short) (cycle * 1000 + p), (uint32_t) ((cycle * 1000 + p) * 480), enc);
            usleep(10000);
        }
        aes_cbc_destroy(enc);
        close(asock);

        fprintf(stderr, "threadtest: cycle %d SEND-TEARDOWN t=%.6f\n", cycle, now_s());
        teardown(sock, cseq);
        fprintf(stderr, "threadtest: cycle %d RECV-TEARDOWN-response t=%.6f\n", cycle, now_s());

        /* Send a /feedback keepalive at least every 2s during a long gap,
         * or the server's own missed-feedback timeout tears it down. */
        for (int waited = 0; waited < gap_s; waited += 2) {
            int chunk = (gap_s - waited) < 2 ? (gap_s - waited) : 2;
            sleep((unsigned) chunk);
            if (waited + chunk < gap_s) {
                std::vector<unsigned char> fb_resp;
                rtsp_request(sock, "POST", "/feedback", cseq++, NULL, 0, fb_resp);
            }
        }
    }

    fairplay_destroy(myfp);
    close(sock);
    fprintf(stderr, "threadtest: done (%d cycles)\n", cycles);
    return 0;
}

/* --- mode: ntpresync -- SETUP+sync, restart, pre-sync packet then a
 * fresh sync+packet: fixed code withholds the pre-sync one; unfixed
 * code renders it immediately using the stale mapping. */
int mode_ntpresync() {
    int sock = connect_rtsp();
    if (sock < 0) return 1;
    int cseq = 1;
    fairplay_t *myfp = NULL;
    handshake(sock, cseq, &myfp);

    unsigned char ekey[72], eiv[16];
    get_random_bytes(ekey, sizeof(ekey));
    get_random_bytes(eiv, sizeof(eiv));

    /* --- First SETUP: establish a real session and a real sync. --- */
    uint64_t ports1 = do_setup(sock, cseq, true, 0, ekey, eiv);
    if (!ports1) {
        fprintf(stderr, "NTP-RESYNC-CHECK: FAIL (first SETUP gave no dataPort/controlPort)\n");
        return 1;
    }
    unsigned short cport1 = (unsigned short) ports1;
    struct sockaddr_in csync_dest;
    memset(&csync_dest, 0, sizeof(csync_dest));
    csync_dest.sin_family = AF_INET;
    csync_dest.sin_port = htons(cport1);
    csync_dest.sin_addr.s_addr = inet_addr(g_host.c_str());
    int csock = socket(AF_INET, SOCK_DGRAM, 0);
    send_sync_packet(csock, &csync_dest, true, 0, (uint64_t) time(NULL));
    usleep(100000); /* let the server process the sync packet */
    close(csock);

    /* --- Restart on the same connection: TEARDOWN + second SETUP. --- */
    teardown(sock, cseq);
    uint64_t ports2 = do_setup(sock, cseq, false, 0, NULL, NULL);
    if (!ports2) {
        fprintf(stderr, "NTP-RESYNC-CHECK: FAIL (second SETUP gave no dataPort/controlPort)\n");
        return 1;
    }
    unsigned short dport2 = (unsigned short) (ports2 >> 32);
    unsigned short cport2 = (unsigned short) ports2;

    unsigned char probe_key[16] = {0}, probe_iv[16] = {0};
    struct sockaddr_in dest2;
    memset(&dest2, 0, sizeof(dest2));
    dest2.sin_family = AF_INET;
    dest2.sin_port = htons(dport2);
    dest2.sin_addr.s_addr = inet_addr(g_host.c_str());
    struct sockaddr_in csync_dest2;
    memset(&csync_dest2, 0, sizeof(csync_dest2));
    csync_dest2.sin_family = AF_INET;
    csync_dest2.sin_port = htons(cport2);
    csync_dest2.sin_addr.s_addr = inet_addr(g_host.c_str());

    /* Step 3: probe packet A, small fresh RTP timestamp, sent BEFORE any
     * new sync -- exactly the window the bug lives in. */
    int asock = socket(AF_INET, SOCK_DGRAM, 0);
    aes_ctx_t *enc = aes_cbc_init(probe_key, probe_iv, AES_ENCRYPT);
    fprintf(stderr, "NTP-RESYNC-CHECK: SENT-PROBE-A t=%.6f\n", now_s());
    send_audio_packet(asock, &dest2, 1, 100, enc);
    usleep(400000); /* give an unfixed binary's immediate render a chance to show up */

    /* Step 4: a second, fresh sync packet. */
    int csock2 = socket(AF_INET, SOCK_DGRAM, 0);
    fprintf(stderr, "NTP-RESYNC-CHECK: SENT-SYNC-2 t=%.6f\n", now_s());
    send_sync_packet(csock2, &csync_dest2, false, 0, (uint64_t) time(NULL));
    usleep(200000);
    close(csock2);

    /* Step 5: probe packet B, after the fresh sync -- must render on
     * both fixed and unfixed code (the "does it ever recover at all"
     * control). */
    fprintf(stderr, "NTP-RESYNC-CHECK: SENT-PROBE-B t=%.6f\n", now_s());
    send_audio_packet(asock, &dest2, 2, 200, enc);
    usleep(400000);
    close(asock);
    aes_cbc_destroy(enc);
    fprintf(stderr, "NTP-RESYNC-CHECK: done, verdict is in the RENDER-BUFFER-CALL timestamps above\n");

    fairplay_destroy(myfp);
    close(sock);
    return 0;
}

/* --- mode: resendstorm -- real controlPort (resend-wait path), sends
 * 0-4, a permanent gap at 5-7, then 8 onward; counts resend requests. */
int mode_resendstorm() {
    int sock = connect_rtsp();
    if (sock < 0) return 1;
    int cseq = 1;
    fairplay_t *myfp = NULL;
    handshake(sock, cseq, &myfp);

    int csock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in my_caddr;
    memset(&my_caddr, 0, sizeof(my_caddr));
    my_caddr.sin_family = AF_INET;
    my_caddr.sin_addr.s_addr = inet_addr(g_host.c_str());
    if (bind(csock, (struct sockaddr *) &my_caddr, sizeof(my_caddr)) < 0) {
        fprintf(stderr, "RESEND-STORM-CHECK: FAIL (bind failed: %s)\n", strerror(errno));
        return 1;
    }
    socklen_t my_caddr_len = sizeof(my_caddr);
    getsockname(csock, (struct sockaddr *) &my_caddr, &my_caddr_len);
    unsigned short my_cport = ntohs(my_caddr.sin_port);

    unsigned char ekey[72], eiv[16], aeskey[16];
    get_random_bytes(ekey, sizeof(ekey));
    get_random_bytes(eiv, sizeof(eiv));
    fairplay_decrypt(myfp, ekey, aeskey);

    uint64_t ports = do_setup(sock, cseq, true, my_cport, ekey, eiv);
    if (!ports) {
        fprintf(stderr, "RESEND-STORM-CHECK: FAIL (no dataPort/controlPort in SETUP response)\n");
        return 1;
    }
    unsigned short dport = (unsigned short) (ports >> 32);
    unsigned short server_cport = (unsigned short) ports;

    struct sockaddr_in csync_dest;
    memset(&csync_dest, 0, sizeof(csync_dest));
    csync_dest.sin_family = AF_INET;
    csync_dest.sin_port = htons(server_cport);
    csync_dest.sin_addr.s_addr = inet_addr(g_host.c_str());
    /* Sent from csock (our bound, declared-controlPort socket), not a
     * fresh one -- this is how the server learns to route resend
     * requests back to us at all. */
    send_sync_packet(csock, &csync_dest, true, 0, (uint64_t) time(NULL));
    usleep(100000);

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dport);
    dest.sin_addr.s_addr = inet_addr(g_host.c_str());
    int asock = socket(AF_INET, SOCK_DGRAM, 0);
    aes_ctx_t *enc = aes_cbc_init(aeskey, eiv, AES_ENCRYPT);

    for (int p = 0; p < 5; p++) {
        send_audio_packet(asock, &dest, (unsigned short) p, (uint32_t) (p * 480), enc);
        usleep(5000);
    }
    fprintf(stderr, "RESEND-STORM-CHECK: SENT-GAP t=%.6f (seqnum 5-7 never sent)\n", now_s());

    int resend_request_count = 0;
    int keepalive_seqnum = 8;
    double window_start = now_s();
    const double keepalive_interval_s = 480.0 / 44100.0;
    const double window_s = 3.5;
    double next_send = window_start;
    double last_request_t = -1.0; /* -1: never received one at all */
    struct timeval tv;
    tv.tv_sec = 0; tv.tv_usec = 2000;
    setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (now_s() - window_start < window_s) {
        double t = now_s();
        if (t >= next_send) {
            send_audio_packet(asock, &dest, (unsigned short) keepalive_seqnum, (uint32_t) (keepalive_seqnum * 480), enc);
            keepalive_seqnum++;
            next_send += keepalive_interval_s;
        }
        unsigned char buf[64];
        ssize_t n = recv(csock, buf, sizeof(buf), 0);
        if (n == 8 && buf[1] == 0xD5) {
            unsigned short req_seqnum = (unsigned short) ((buf[4] << 8) | buf[5]);
            if (req_seqnum == 5) {
                resend_request_count++;
                last_request_t = now_s() - window_start;
            }
        }
    }
    /* RESOLVED-AT is the real recovery-time metric; request count alone
     * can't distinguish "asks a lot but slow to give up" from "fast". */
    fprintf(stderr, "RESEND-STORM-CHECK: RESEND-REQUEST-COUNT %d (in %.1fs, sent %d keepalive packets)\n",
            resend_request_count, window_s, keepalive_seqnum - 8);
    fprintf(stderr, "RESEND-STORM-CHECK: RESOLVED-AT %.4f\n", last_request_t);

    aes_cbc_destroy(enc);
    close(asock);
    fairplay_destroy(myfp);
    close(sock);
    close(csock);
    return 0;
}

/* --- mode: resendrecovery -- actually resends requested packets,
 * modeling a contended channel via a channel_busy_until deadline. */
int mode_resendrecovery() {
    int sock = connect_rtsp();
    if (sock < 0) return 1;
    int cseq = 1;
    fairplay_t *myfp = NULL;
    handshake(sock, cseq, &myfp);

    int csock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in my_caddr;
    memset(&my_caddr, 0, sizeof(my_caddr));
    my_caddr.sin_family = AF_INET;
    my_caddr.sin_addr.s_addr = inet_addr(g_host.c_str());
    if (bind(csock, (struct sockaddr *) &my_caddr, sizeof(my_caddr)) < 0) {
        fprintf(stderr, "RESEND-RECOVERY-CHECK: FAIL (bind failed: %s)\n", strerror(errno));
        return 1;
    }
    socklen_t my_caddr_len = sizeof(my_caddr);
    getsockname(csock, (struct sockaddr *) &my_caddr, &my_caddr_len);
    unsigned short my_cport = ntohs(my_caddr.sin_port);

    unsigned char ekey[72], eiv[16], aeskey[16];
    get_random_bytes(ekey, sizeof(ekey));
    get_random_bytes(eiv, sizeof(eiv));
    fairplay_decrypt(myfp, ekey, aeskey);

    uint64_t ports = do_setup(sock, cseq, true, my_cport, ekey, eiv);
    if (!ports) {
        fprintf(stderr, "RESEND-RECOVERY-CHECK: FAIL (no dataPort/controlPort in SETUP response)\n");
        return 1;
    }
    unsigned short dport = (unsigned short) (ports >> 32);
    unsigned short server_cport = (unsigned short) ports;

    struct sockaddr_in csync_dest;
    memset(&csync_dest, 0, sizeof(csync_dest));
    csync_dest.sin_family = AF_INET;
    csync_dest.sin_port = htons(server_cport);
    csync_dest.sin_addr.s_addr = inet_addr(g_host.c_str());
    send_sync_packet(csock, &csync_dest, true, 0, (uint64_t) time(NULL));
    usleep(100000);

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dport);
    dest.sin_addr.s_addr = inet_addr(g_host.c_str());
    int asock = socket(AF_INET, SOCK_DGRAM, 0);
    aes_ctx_t *enc = aes_cbc_init(aeskey, eiv, AES_ENCRYPT);

    for (int p = 0; p < 5; p++) {
        send_audio_packet(asock, &dest, (unsigned short) p, (uint32_t) (p * 480), enc);
        usleep(5000);
    }
    double gap_start = now_s();
    fprintf(stderr, "RESEND-RECOVERY-CHECK: SENT-GAP t=%.6f (seqnum 5-7 missing)\n", gap_start);

    const double CHANNEL_COST_S = 0.008; /* 8ms per packet -- see function comment */
    const double WINDOW_S = 4.0;
    double channel_busy_until = 0.0;
    bool recovered = false;
    int keepalive_seqnum = 8;
    double next_send = now_s();
    struct timeval tv;
    tv.tv_sec = 0; tv.tv_usec = 1000;
    setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (now_s() - gap_start < WINDOW_S) {
        double t = now_s();
        if (!recovered && t >= next_send) {
            send_audio_packet(asock, &dest, (unsigned short) keepalive_seqnum, (uint32_t) (keepalive_seqnum * 480), enc);
            keepalive_seqnum++;
            next_send += 0.005;
        }
        unsigned char buf[64];
        ssize_t n = recv(csock, buf, sizeof(buf), 0);
        if (n == 8 && buf[1] == 0xD5) {
            unsigned short req_seqnum = (unsigned short) ((buf[4] << 8) | buf[5]);
            if (req_seqnum == 5) {
                double rt = now_s();
                channel_busy_until = (channel_busy_until > rt ? channel_busy_until : rt) + CHANNEL_COST_S;
                fprintf(stderr, "RESEND-RECOVERY-CHECK: RECV-REQUEST t=%.6f channel_busy_until=%.6f\n", rt, channel_busy_until);
            }
        }
        if (!recovered && now_s() >= channel_busy_until) {
            send_audio_packet(asock, &dest, 5, (uint32_t) (5 * 480), enc);
            send_audio_packet(asock, &dest, 6, (uint32_t) (6 * 480), enc);
            send_audio_packet(asock, &dest, 7, (uint32_t) (7 * 480), enc);
            recovered = true;
            double recovery_time = now_s() - gap_start;
            fprintf(stderr, "RESEND-RECOVERY-CHECK: RECOVERED t=%.6f recovery_time=%.6f\n", now_s(), recovery_time);
        }
    }
    if (!recovered) {
        fprintf(stderr, "RESEND-RECOVERY-CHECK: NEVER-RECOVERED (channel modeled busy for the whole %.1fs window)\n", WINDOW_S);
    }

    aes_cbc_destroy(enc);
    close(asock);
    fairplay_destroy(myfp);
    close(sock);
    close(csock);
    return 0;
}

void print_usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s <mode> --port <raop_port> [--host <ip>] [mode-specific args]\n"
        "modes:\n"
        "  threadtest [N] [--gap-s S]   N SETUP/audio/TEARDOWN cycles (default 8)\n"
        "  ntpresync                    NTP-sync-reset-on-restart regression check\n"
        "  resendstorm                  resend-request-rate regression check\n"
        "  resendrecovery               end-to-end resend recovery-time model\n"
        "--host defaults to 127.0.0.1.\n", argv0);
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    std::string mode = argv[1];
    int cycles = 8;
    int gap_s = 0;
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            g_port = (unsigned short) atoi(argv[++i]);
        } else if (arg == "--host" && i + 1 < argc) {
            g_host = argv[++i];
        } else if (arg == "--gap-s" && i + 1 < argc) {
            gap_s = atoi(argv[++i]);
        } else if (mode == "threadtest" && arg[0] != '-') {
            cycles = atoi(argv[i]);
        } else {
            fprintf(stderr, "synthetic-client: unrecognized argument '%s'\n", arg.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }
    if (!g_port) {
        fprintf(stderr, "synthetic-client: --port is required\n");
        print_usage(argv[0]);
        return 1;
    }

    if (mode == "threadtest") return mode_threadtest(cycles, gap_s);
    if (mode == "ntpresync") return mode_ntpresync();
    if (mode == "resendstorm") return mode_resendstorm();
    if (mode == "resendrecovery") return mode_resendrecovery();

    fprintf(stderr, "synthetic-client: unknown mode '%s'\n", mode.c_str());
    print_usage(argv[0]);
    return 1;
}
