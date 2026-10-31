/*
 * hyperion_fb.h - minimal Hyperion.ng Flatbuffers client (no dependencies).
 *
 * Speaks the protocol of Hyperion's Flatbuffers server (default TCP 19400),
 * schema: libsrc/flatbufserver/hyperion_request.fbs / hyperion_reply.fbs.
 *
 * Every message on the wire is: 4-byte big-endian length + flatbuffer bytes.
 * The builders below produce the complete framed message.
 */
#ifndef HYPERION_FB_H
#define HYPERION_FB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hyperion only accepts priorities 100..199 from Flatbuffers clients. */
#define HFB_PRIORITY_MIN 100
#define HFB_PRIORITY_MAX 199

/* Bytes needed for a framed image message carrying pixels_len bytes. */
#define HFB_IMAGE_OVERHEAD 96
#define HFB_REGISTER_MAX   256

/* --- message builders (return total framed length, 0 on error/overflow) --- */
size_t hfb_build_register(uint8_t *out, size_t cap, const char *origin,
                          int32_t priority);

/* pixels: width*height*3 bytes, RGB, row-major (the server infers bpp). */
size_t hfb_build_image(uint8_t *out, size_t cap, int32_t width, int32_t height,
                       const uint8_t *pixels, size_t pixels_len);

/* --- socket client (blocking connect, non-blocking reply draining) --- */
int  hfb_connect(const char *host_ipv4, int port);          /* fd or -1 */
int  hfb_register(int fd, const char *origin, int priority); /* 0 = ok  */
int  hfb_send_image(int fd, int width, int height, const uint8_t *rgb);
void hfb_close(int fd);

#ifdef __cplusplus
}
#endif
#endif
