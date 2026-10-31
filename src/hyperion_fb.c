/*
 * hyperion_fb.c - see hyperion_fb.h
 *
 * The flatbuffers are laid out by hand (fixed shapes, no builder library) so
 * this compiles unchanged with gcc/clang on Linux and with the ps5-payload-sdk.
 * All offsets below are relative to the start of the flatbuffer (after the
 * 4-byte length prefix).
 */
#include "hyperion_fb.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* union Command {Color=1, Image=2, Clear=3, Register=4}; ImageType {RawImage=1} */
#define CMD_IMAGE    2
#define CMD_REGISTER 4
#define IMG_RAW      1

typedef struct {
    uint8_t *p;   /* start of flatbuffer region */
    size_t cap;
    size_t len;
    int err;
} W;

static void w_u8(W *w, uint8_t v)
{
    if (w->len + 1 > w->cap) { w->err = 1; return; }
    w->p[w->len++] = v;
}
static void w_u16(W *w, uint16_t v) { w_u8(w, v & 0xff); w_u8(w, v >> 8); }
static void w_u32(W *w, uint32_t v)
{
    w_u8(w, v & 0xff); w_u8(w, (v >> 8) & 0xff);
    w_u8(w, (v >> 16) & 0xff); w_u8(w, v >> 24);
}
static void w_i32(W *w, int32_t v) { w_u32(w, (uint32_t)v); }
static void w_bytes(W *w, const void *d, size_t n)
{
    if (w->len + n > w->cap) { w->err = 1; return; }
    memcpy(w->p + w->len, d, n);
    w->len += n;
}
static void w_align4(W *w) { while (w->len & 3) w_u8(w, 0); }
static void patch_u32(W *w, size_t at, uint32_t v)
{
    if (at + 4 > w->len) { w->err = 1; return; }
    w->p[at] = v & 0xff; w->p[at + 1] = (v >> 8) & 0xff;
    w->p[at + 2] = (v >> 16) & 0xff; w->p[at + 3] = v >> 24;
}

/* Request { command_type:ubyte(id0); command:union(id1) } -> vtable + table.
 * Returns position of the table; *cmd_field = position of the offset to patch. */
static size_t put_request(W *w, uint8_t cmd_type, size_t *cmd_field)
{
    size_t vt = w->len;
    w_u16(w, 8);  /* vtable size  */
    w_u16(w, 12); /* table size   */
    w_u16(w, 4);  /* command_type @ +4 */
    w_u16(w, 8);  /* command      @ +8 */
    size_t t = w->len;
    w_i32(w, (int32_t)(t - vt));
    w_u8(w, cmd_type); w_u8(w, 0); w_u8(w, 0); w_u8(w, 0);
    *cmd_field = w->len;
    w_u32(w, 0);
    return t;
}

static size_t finish(W *w, uint8_t *out, size_t root)
{
    if (w->err) return 0;
    patch_u32(w, 0, (uint32_t)root);
    if (w->err) return 0;
    uint32_t n = (uint32_t)w->len;
    out[0] = n >> 24; out[1] = (n >> 16) & 0xff;
    out[2] = (n >> 8) & 0xff; out[3] = n & 0xff;
    return w->len + 4;
}

size_t hfb_build_register(uint8_t *out, size_t cap, const char *origin,
                          int32_t priority)
{
    if (cap < 8 || !origin) return 0;
    W w = { out + 4, cap - 4, 0, 0 };
    size_t olen = strlen(origin);

    w_u32(&w, 0); /* root offset, patched below */
    size_t cmd_field;
    size_t req_t = put_request(&w, CMD_REGISTER, &cmd_field);

    /* Register { origin:string(id0); priority:int(id1) } */
    size_t vt = w.len;
    w_u16(&w, 8); w_u16(&w, 12); w_u16(&w, 4); w_u16(&w, 8);
    size_t t = w.len;
    w_i32(&w, (int32_t)(t - vt));
    size_t origin_field = w.len;
    w_u32(&w, 0);
    w_i32(&w, priority);

    size_t str = w.len;
    w_u32(&w, (uint32_t)olen);
    w_bytes(&w, origin, olen);
    w_u8(&w, 0);
    w_align4(&w);

    patch_u32(&w, cmd_field, (uint32_t)(t - cmd_field));
    patch_u32(&w, origin_field, (uint32_t)(str - origin_field));
    return finish(&w, out, req_t);
}

size_t hfb_build_image(uint8_t *out, size_t cap, int32_t width, int32_t height,
                       const uint8_t *pixels, size_t pixels_len)
{
    if (cap < 8 || !pixels || width <= 0 || height <= 0) return 0;
    W w = { out + 4, cap - 4, 0, 0 };

    w_u32(&w, 0);
    size_t cmd_field;
    size_t req_t = put_request(&w, CMD_IMAGE, &cmd_field);

    /* Image { data:ImageType(id0 type, id1 value); duration:int(id2, omitted) } */
    size_t ivt = w.len;
    w_u16(&w, 8); w_u16(&w, 12); w_u16(&w, 4); w_u16(&w, 8);
    size_t it = w.len;
    w_i32(&w, (int32_t)(it - ivt));
    w_u8(&w, IMG_RAW); w_u8(&w, 0); w_u8(&w, 0); w_u8(&w, 0);
    size_t img_data_field = w.len;
    w_u32(&w, 0);

    /* RawImage { data:[ubyte](id0); width:int(id1); height:int(id2) } */
    size_t rvt = w.len;
    w_u16(&w, 10); w_u16(&w, 16); w_u16(&w, 4); w_u16(&w, 8); w_u16(&w, 12);
    w_align4(&w);
    size_t rt = w.len;
    w_i32(&w, (int32_t)(rt - rvt));
    size_t raw_data_field = w.len;
    w_u32(&w, 0);
    w_i32(&w, width);
    w_i32(&w, height);

    size_t vec = w.len;
    w_u32(&w, (uint32_t)pixels_len);
    w_bytes(&w, pixels, pixels_len);
    w_align4(&w);

    patch_u32(&w, cmd_field, (uint32_t)(it - cmd_field));
    patch_u32(&w, img_data_field, (uint32_t)(rt - img_data_field));
    patch_u32(&w, raw_data_field, (uint32_t)(vec - raw_data_field));
    return finish(&w, out, req_t);
}

/* ------------------------------ socket side ------------------------------ */

static int send_all(int fd, const uint8_t *buf, size_t n)
{
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    while (n) {
        ssize_t r = send(fd, buf, n, flags);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        buf += r; n -= (size_t)r;
    }
    return 0;
}

static int recv_all(int fd, uint8_t *buf, size_t n)
{
    while (n) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        buf += r; n -= (size_t)r;
    }
    return 0;
}

/* Parse a Reply { error:string(id0); video:int(id1); registered:int(id2) }.
 * Returns 0 if no error field, -1 if the server reported an error. */
static int parse_reply(const uint8_t *b, size_t n, int32_t *registered)
{
    if (n < 8) return -1;
    uint32_t root = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    if (root + 4 > n) return -1;
    int32_t so = (int32_t)(b[root] | (b[root+1] << 8) | (b[root+2] << 16) |
                           ((uint32_t)b[root+3] << 24));
    int64_t vt = (int64_t)root - so;
    if (vt < 0 || (size_t)vt + 4 > n) return -1;
    uint16_t vsize = b[vt] | (b[vt + 1] << 8);
    uint16_t slot[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        size_t off = (size_t)vt + 4 + 2 * i;
        if (4 + 2 * i < vsize && off + 2 <= n)
            slot[i] = b[off] | (b[off + 1] << 8);
    }
    if (registered) {
        *registered = -1;
        if (slot[2] && root + slot[2] + 4 <= n) {
            size_t p = root + slot[2];
            *registered = (int32_t)(b[p] | (b[p+1] << 8) | (b[p+2] << 16) |
                                    ((uint32_t)b[p+3] << 24));
        }
    }
    return slot[0] ? -1 : 0;
}

/* Replies to images are collected without ever blocking the sender: whatever
 * has arrived is read, complete replies are parsed, a partial one is kept for
 * the next call. (Blocking for the rest of a reply costs ~40 ms per frame when
 * the server sends header and body as two TCP segments.) One connection only. */
static uint8_t g_rx[65536 + 8];
static size_t g_rx_len;

/* 0 = ok, -2 = the server reported an error, -1 = connection lost */
static int drain_replies(int fd)
{
    int server_error = 0;
    for (;;) {
        if (g_rx_len >= sizeof g_rx) { g_rx_len = 0; return -1; } /* garbage */
        ssize_t r = recv(fd, g_rx + g_rx_len, sizeof g_rx - g_rx_len, MSG_DONTWAIT);
        if (r > 0) { g_rx_len += (size_t)r; continue; }
        if (r == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return -1;
    }
    size_t off = 0;
    while (g_rx_len - off >= 4) {
        uint32_t n = ((uint32_t)g_rx[off] << 24) | (g_rx[off + 1] << 16) |
                     (g_rx[off + 2] << 8) | g_rx[off + 3];
        if (n == 0 || n > 65000) { g_rx_len = 0; return -1; }
        if (g_rx_len - off - 4 < n) break; /* incomplete: wait for the rest */
        if (parse_reply(g_rx + off + 4, n, NULL) != 0) server_error = 1;
        off += 4 + (size_t)n;
    }
    if (off) { memmove(g_rx, g_rx + off, g_rx_len - off); g_rx_len -= off; }
    return server_error ? -2 : 0;
}

/* Reads one framed reply. timeout_ms < 0: block; 0: only if already there. */
static int read_reply(int fd, int timeout_ms, int32_t *registered)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 0;
    tv.tv_usec = timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    uint8_t hdr[4];
    ssize_t r = recv(fd, hdr, 4, 0);
    if (r <= 0) return (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? 1 : -1;
    if (r < 4 && recv_all(fd, hdr + r, 4 - (size_t)r) < 0) return -1;
    uint32_t n = ((uint32_t)hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
    if (n == 0 || n > 65536) return -1;
    uint8_t body[65536];
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (recv_all(fd, body, n) < 0) return -1;
    return parse_reply(body, n, registered) == 0 ? 0 : -2;
}

int hfb_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    int one = 1;
    g_rx_len = 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

int hfb_register(int fd, const char *origin, int priority)
{
    uint8_t buf[HFB_REGISTER_MAX];
    size_t n = hfb_build_register(buf, sizeof buf, origin, priority);
    if (!n || send_all(fd, buf, n) < 0) return -1;
    int32_t reg = -1;
    int r = read_reply(fd, 3000, &reg);
    if (r != 0) return -1;
    return reg == priority ? 0 : -1;
}

int hfb_send_image(int fd, int width, int height, const uint8_t *rgb)
{
    size_t px = (size_t)width * (size_t)height * 3;
    size_t cap = px + HFB_IMAGE_OVERHEAD;
    static uint8_t frame[1 << 20]; /* not thread-safe: one sender thread */
    if (cap > sizeof frame) return -1;
    size_t n = hfb_build_image(frame, cap, width, height, rgb, px);
    if (!n || send_all(fd, frame, n) < 0) return -1;
    /* The server answers every image; collect replies without blocking. */
    return drain_replies(fd);
}

void hfb_close(int fd) { if (fd >= 0) close(fd); }
