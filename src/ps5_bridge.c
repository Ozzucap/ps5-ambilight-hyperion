/*
 * ps5_bridge.c - receives the PS5 screen as an NV12 stream and feeds Hyperion.
 *
 * Source:  TCP stream of NV12 frames from a payload on the console (default port
 *          5599). Each frame: 12-byte little-endian header (magic, frame counter,
 *          payload length), then NV12 (Y plane, then interleaved UV at half height).
 *          Magic "AVS2" = 1920x1080 (PSCapture's stream format). "AVS3" = 480x270,
 *          an optional pre-reduced variant of the same format.
 * Output:  64x36 RGB image to Hyperion's Flatbuffers server (default 19400).
 *
 * Runs on any Linux box on the LAN (NAS, PC). It only speaks the stream format
 * above and contains no code from any console payload.
 *
 * usage: ps5_bridge --ps5 <ip> --hyperion <ip> [options]
 *   --ps5 IP            console address                       (env PS5_HOST)
 *   --ps5-port N        first stream port, tries N..N+30      (env PS5_PORT, 5599)
 *   --hyperion IP       Hyperion address                      (env HYPERION_HOST)
 *   --hyperion-port N   Flatbuffers port                      (env HYPERION_PORT, 19400)
 *   --priority N        100..199                              (env HYPERION_PRIORITY, 150)
 *   --fps N             max frames/s sent to Hyperion         (env FPS, 30)
 *   --full-range        input is full-range YUV (default: limited 16-235)
 *   --swap-uv           swap U and V (use if reds and blues are inverted)
 *   --hdr               input is HDR10 (PQ, BT.2020): convert to SDR     (env HDR=1)
 *   --paper-white N     HDR level mapped to SDR white, nits (default 203; lower = brighter LEDs)
 *   --hdr-peak N        HDR level that clips to full white, nits (default 1000)
 *   --crop-bottom N     ignore the last N rows (even, default 32)  (env CROP_BOTTOM)
 *   --snapshot FILE     write a 480x270 PNG of the screen every 5 s (for checking)
 */
#include "hyperion_fb.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SW 1920
#define SH 1080
#define NV12_SZ (SW * SH * 3 / 2)
#define OW 64
#define OH 36
#define SM_W 480
#define SM_H 270
#define SM_SZ (SM_W * SM_H * 3 / 2)

static uint8_t frame[NV12_SZ];
static uint8_t out[OW * OH * 3];

static int g_full_range, g_swap_uv;
static int g_hdr;
static float g_paper = 203.0f, g_peak = 1000.0f;
static int g_crop = 32; /* 1080p rows ignored at the bottom: the console writes the last ~30 late */

/* --------------------------------- helpers -------------------------------- */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int rd_exact(int fd, uint8_t *p, size_t n)
{
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; } /* incl. timeout */
        p += r; n -= (size_t)r;
    }
    return 0;
}

static uint8_t clamp8(float v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)(v + 0.5f)); }

/* ---- HDR10 (SMPTE ST 2084 PQ, BT.2020) -> SDR (BT.709, gamma 2.2) ---- */
#define PQ_N 4096
static float pq_lut[PQ_N + 1]; /* PQ code 0..1 -> nits */

static void pq_init(void)
{
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096,
                 c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
    for (int i = 0; i <= PQ_N; i++) {
        double p = pow((double)i / PQ_N, 1.0 / m2);
        double num = p - c1 > 0 ? p - c1 : 0;
        pq_lut[i] = (float)(10000.0 * pow(num / (c2 - c3 * p), 1.0 / m1));
    }
}
static float pq_eotf(float e)
{
    if (e <= 0) return 0;
    if (e >= 1) return pq_lut[PQ_N];
    float f = e * PQ_N; int i = (int)f; float t = f - i;
    return pq_lut[i] + (pq_lut[i + 1] - pq_lut[i]) * t;
}
/* identity up to the knee, then a soft roll-off that reaches 1.0 at peak/paper */
static float tonemap(float x)
{
    const float k = 0.8f;
    if (x <= k) return x;
    float xmax = g_peak / g_paper; if (xmax < 1.2f) xmax = 1.2f;
    float w = (xmax - k) / (1 - k), t = (x - k) / (1 - k);
    float f = t * (1.0f + t / (w * w)) / (1.0f + t);
    float y = k + (1 - k) * f;
    return y > 1 ? 1 : y;
}
/* Y in 0..1, Cb/Cr in -0.5..0.5 (BT.2020 non-constant-luminance, PQ-encoded) */
static void hdr_to_sdr(float Y, float Cb, float Cr, uint8_t *rgb)
{
    float rp = Y + 1.4746f * Cr, gp = Y - 0.16455f * Cb - 0.57135f * Cr, bp = Y + 1.8814f * Cb;
    float R2 = pq_eotf(rp), G2 = pq_eotf(gp), B2 = pq_eotf(bp);
    float r = (1.6605f * R2 - 0.5876f * G2 - 0.0728f * B2) / g_paper;
    float g = (-0.1246f * R2 + 1.1329f * G2 - 0.0083f * B2) / g_paper;
    float b = (-0.0182f * R2 - 0.1006f * G2 + 1.1187f * B2) / g_paper;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (m > 0.8f) { float f = tonemap(m) / m; r *= f; g *= f; b *= f; } /* hue-preserving */
    rgb[0] = clamp8(powf(r > 1 ? 1 : r, 1.0f / 2.2f) * 255.0f);
    rgb[1] = clamp8(powf(g > 1 ? 1 : g, 1.0f / 2.2f) * 255.0f);
    rgb[2] = clamp8(powf(b > 1 ? 1 : b, 1.0f / 2.2f) * 255.0f);
}

/* BT.709 YCbCr -> RGB */
static void yuv2rgb(float y, float u, float v, uint8_t *rgb)
{
    if (g_swap_uv) { float t = u; u = v; v = t; }
    if (g_hdr) {
        float Yn = g_full_range ? y / 255.0f : (y - 16.0f) / 219.0f;
        float Cbn = (u - 128.0f) / (g_full_range ? 255.0f : 224.0f);
        float Crn = (v - 128.0f) / (g_full_range ? 255.0f : 224.0f);
        hdr_to_sdr(Yn, Cbn, Crn, rgb);
        return;
    }
    float Y = g_full_range ? y : (y - 16.0f) * (255.0f / 219.0f);
    float Cb = (g_full_range ? (u - 128.0f) : (u - 128.0f) * (255.0f / 224.0f));
    float Cr = (g_full_range ? (v - 128.0f) : (v - 128.0f) * (255.0f / 224.0f));
    rgb[0] = clamp8(Y + 1.5748f * Cr);
    rgb[1] = clamp8(Y - 0.1873f * Cb - 0.4681f * Cr);
    rgb[2] = clamp8(Y + 1.8556f * Cb);
}

/* NV12 (sw x sh) -> 64x36 RGB by block averaging. The bottom rows (g_crop,
 * scaled to this resolution) are ignored: the last rows of the console's ring
 * are written late and show up as a grey strip. Block edges are even so luma
 * and chroma line up; blocks may differ by a pixel when sw/64 is not whole. */
static void downsample(const uint8_t *nv12, int sw, int sh, uint8_t *rgb)
{
    const uint8_t *Y = nv12, *UV = nv12 + (size_t)sw * sh;
    int used_h = sh - (g_crop * sh / SH);
    for (int oy = 0; oy < OH; oy++) {
        int y0 = ((oy * used_h) / OH) & ~1;
        int y1 = (((oy + 1) * used_h) / OH) & ~1;
        if (y1 <= y0) y1 = y0 + 2;
        for (int ox = 0; ox < OW; ox++) {
            int x0 = ((ox * sw) / OW) & ~1;
            int x1 = (((ox + 1) * sw) / OW) & ~1;
            if (x1 <= x0) x1 = x0 + 2;
            unsigned sy = 0, su = 0, sv = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t *row = Y + (size_t)y * sw;
                for (int x = x0; x < x1; x++) sy += row[x];
            }
            for (int y = y0 / 2; y < y1 / 2; y++) {
                const uint8_t *row = UV + (size_t)y * sw;
                for (int x = x0; x < x1; x += 2) { su += row[x]; sv += row[x + 1]; }
            }
            float ny = (float)((y1 - y0) * (x1 - x0));
            float nc = (float)(((y1 - y0) / 2) * ((x1 - x0) / 2));
            yuv2rgb(sy / ny, su / nc, sv / nc, &rgb[(oy * OW + ox) * 3]);
        }
    }
}

/* ------------------------- snapshot PNG (no deps) -------------------------- */

static uint32_t crc_tab[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[n] = c;
    }
}
static uint32_t crc_upd(uint32_t c, const uint8_t *p, size_t n)
{
    while (n--) c = crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    be32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc_upd(0xFFFFFFFFu, (const uint8_t *)type, 4);
    if (n) c = crc_upd(c, d, n);
    uint8_t t[4]; be32(t, c ^ 0xFFFFFFFFu);
    fwrite(t, 1, 4, f);
}

static void write_snapshot(const char *path, const uint8_t *nv12, int sw, int sh)
{
    enum { PW = 480, PH = 270 };
    static uint8_t raw[PH * (1 + PW * 3)];
    const uint8_t *Y = nv12, *UV = nv12 + (size_t)sw * sh;
    int step = sw / PW; /* 4 for 1080p, 1 for the console-reduced 480x270 */
    for (int y = 0; y < PH; y++) {
        uint8_t *line = &raw[y * (1 + PW * 3)];
        line[0] = 0; /* PNG filter: none */
        for (int x = 0; x < PW; x++) {
            int sx = x * step + step / 2, sy = y * step + step / 2;
            const uint8_t *uv = UV + (size_t)(sy / 2) * sw + (sx / 2) * 2;
            yuv2rgb(Y[(size_t)sy * sw + sx], uv[0], uv[1], &line[1 + x * 3]);
        }
    }
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "snapshot: cannot write %s\n", tmp); return; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, PW); be32(ihdr + 4, PH);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    /* zlib stream made of stored (uncompressed) deflate blocks */
    size_t total = sizeof raw, nblocks = (total + 65534) / 65535;
    size_t zlen = 2 + total + nblocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(zlen);
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    for (size_t off = 0; off < total; off += 65535) {
        size_t n = total - off < 65535 ? total - off : 65535;
        z[o++] = (off + n == total) ? 1 : 0;
        z[o++] = n & 0xff; z[o++] = n >> 8;
        z[o++] = ~n & 0xff; z[o++] = (~n >> 8) & 0xff;
        memcpy(z + o, raw + off, n); o += n;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < total; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    be32(z + o, (b << 16) | a); o += 4;
    chunk(f, "IDAT", z, (uint32_t)o);
    free(z);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    if (rename(tmp, path) != 0) fprintf(stderr, "snapshot: rename failed\n");
}

/* --------------------------------- main ----------------------------------- */

static const char *opt(int argc, char **argv, const char *flag, const char *env, const char *def)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], flag)) return argv[i + 1];
    const char *e = env ? getenv(env) : NULL;
    return (e && *e) ? e : def;
}
static int flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], name)) return 1;
    return 0;
}

static int connect_ps5(const char *host, int port0)
{
    for (int p = port0; p <= port0 + 30; p++) {
        int fd = hfb_connect(host, p);
        if (fd >= 0) {
            struct timeval tv = { 5, 0 }; /* a silent console = lost stream */
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            printf("connected to PS5 %s:%d\n", host, p);
            return fd;
        }
        /* Only "refused" means the console is up but the payload picked
         * another port; a timeout means it is off/unreachable: stop here. */
        if (errno != ECONNREFUSED) break;
    }
    return -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *ps5 = opt(argc, argv, "--ps5", "PS5_HOST", NULL);
    const char *hy = opt(argc, argv, "--hyperion", "HYPERION_HOST", NULL);
    int ps5_port = atoi(opt(argc, argv, "--ps5-port", "PS5_PORT", "5599"));
    int hy_port = atoi(opt(argc, argv, "--hyperion-port", "HYPERION_PORT", "19400"));
    int prio = atoi(opt(argc, argv, "--priority", "HYPERION_PRIORITY", "150"));
    int fps = atoi(opt(argc, argv, "--fps", "FPS", "30"));
    const char *snap = opt(argc, argv, "--snapshot", "SNAPSHOT", NULL);
    int crop = atoi(opt(argc, argv, "--crop-bottom", "CROP_BOTTOM", "32")) & ~1;
    if (crop < 0 || crop > 200) crop = 32;
    g_crop = crop;
    g_hdr = flag(argc, argv, "--hdr") || atoi(opt(argc, argv, "--hdr-env", "HDR", "0")) == 1;
    g_paper = (float)atof(opt(argc, argv, "--paper-white", "HDR_PAPER_WHITE", "203"));
    g_peak = (float)atof(opt(argc, argv, "--hdr-peak", "HDR_PEAK", "1000"));
    if (g_paper < 20 || g_paper > 1000 || g_peak < 200 || g_peak > 10000) { fprintf(stderr, "bad --paper-white (20..1000) or --hdr-peak (200..10000)\n"); return 2; }
    if (g_hdr) { pq_init(); printf("HDR->SDR conversion on (paper white %.0f nits, peak %.0f nits)\n", g_paper, g_peak); }
    g_full_range = flag(argc, argv, "--full-range");
    g_swap_uv = flag(argc, argv, "--swap-uv");
    if (!ps5 || !hy) {
        fprintf(stderr, "usage: %s --ps5 <ip> --hyperion <ip> [--ps5-port 5599] "
                "[--hyperion-port 19400] [--priority 150] [--fps 30] "
                "[--full-range] [--swap-uv] [--hdr] [--paper-white 203] [--hdr-peak 1000] [--crop-bottom 32] [--snapshot out.png]\n", argv[0]);
        return 2;
    }
    if (prio < HFB_PRIORITY_MIN || prio > HFB_PRIORITY_MAX || fps < 1 || fps > 60) {
        fprintf(stderr, "bad priority (100..199) or fps (1..60)\n");
        return 2;
    }
    crc_init();

    int hfd = -1;
    long last_hy_try = 0, last_send = 0, last_snap = 0, last_stat = now_ms();
    long ps5_lost_at = 0; /* 0 = currently connected / not yet lost */
    unsigned in_frames = 0, out_frames = 0;
    for (;;) {
        int fd = connect_ps5(ps5, ps5_port);
        if (fd < 0) {
            /* A short network hiccup shouldn't blank the LEDs: only release
             * Hyperion once the PS5 has stayed unreachable for a while. */
            if (hfd >= 0 && ps5_lost_at && now_ms() - ps5_lost_at >= 2000) {
                hfb_close(hfd); hfd = -1; last_hy_try = 0;
                fprintf(stderr, "PS5 stream lost, LEDs released\n");
            }
            sleep(1);
            continue;
        }
        ps5_lost_at = 0;
        long last_in = now_ms();
        for (;;) {
            uint8_t hdr[12];
            if (rd_exact(fd, hdr, 12) < 0) break;
            uint32_t len = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
            int sw, sh;
            if (!memcmp(hdr, "AVS2", 4) && len == NV12_SZ) { sw = SW; sh = SH; }
            else if (!memcmp(hdr, "AVS3", 4) && len == SM_SZ) { sw = SM_W; sh = SM_H; }
            else {
                fprintf(stderr, "unexpected stream data (len=%u): reconnecting\n", len);
                break;
            }
            if (rd_exact(fd, frame, len) < 0) break;
            in_frames++;
            long t = now_ms();
            if (t - last_in >= 250)
                fprintf(stderr, "gap: no frame from PS5 for %ld ms\n", t - last_in);
            last_in = t;

            if (t - last_send >= 1000 / fps) {
                last_send = t;
                if (hfd < 0 && t - last_hy_try >= 300) {
                    last_hy_try = t;
                    hfd = hfb_connect(hy, hy_port);
                    if (hfd >= 0 && hfb_register(hfd, "PS5-Ambilight", prio) != 0) {
                        hfb_close(hfd); hfd = -1;
                        fprintf(stderr, "Hyperion refused registration\n");
                    } else if (hfd >= 0) {
                        printf("registered on Hyperion %s:%d (priority %d)\n", hy, hy_port, prio);
                    }
                }
                if (hfd >= 0) {
                    downsample(frame, sw, sh, out);
                    long ts = now_ms();
                    int r = hfb_send_image(hfd, OW, OH, out);
                    if (now_ms() - ts >= 100)
                        fprintf(stderr, "slow: Hyperion send took %ld ms\n", now_ms() - ts);
                    if (r == -1) { hfb_close(hfd); hfd = -1; fprintf(stderr, "Hyperion connection lost\n"); }
                    else out_frames++;
                }
            }
            if (snap && t - last_snap >= 5000) { last_snap = t; write_snapshot(snap, frame, sw, sh); }
            if (t - last_stat >= 10000) {
                printf("stream %.1f fps in, %.1f fps to Hyperion%s\n",
                       in_frames * 1000.0 / (t - last_stat), out_frames * 1000.0 / (t - last_stat),
                       hfd < 0 ? " (Hyperion NOT connected)" : "");
                in_frames = out_frames = 0; last_stat = t;
            }
        }
        close(fd);
        ps5_lost_at = now_ms();
        fprintf(stderr, "PS5 connection lost, retrying\n");
    }
}
