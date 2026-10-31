/*
 * test_sender.c - step 1: send an animated test image to Hyperion.
 *
 * Draws a 64x36 RGB frame: a rainbow that travels around the screen border,
 * with a black centre. Hyperion maps the border to your LEDs, so you should
 * see colour chasing around the strip. No framebuffer involved yet.
 *
 * Linux:  gcc -O2 -o test_sender test_sender.c hyperion_fb.c -lm
 *         usage: test_sender <hyperion_ip> [port=19400] [seconds=15] [priority=150]
 * PS5:    make (ps5-payload-sdk, -DPS5_PAYLOAD). Settings are read from
 *         /data/ps5-ambilight/config.ini (host=, port=, seconds=, priority=).
 */
#include "hyperion_fb.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 64
#define H 36

#ifdef PS5_PAYLOAD
#include <stdarg.h>
/* On-screen notification (same call the SDK's notify_debug sample uses). */
typedef struct notify_request { char useless1[45]; char message[3075]; } notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
static void notify(const char *fmt, ...)
{
    notify_request_t req;
    va_list ap;
    memset(&req, 0, sizeof req);
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
#define CONFIG_PATH "/data/ps5-ambilight/config.ini"
/* key=value lines; '#' starts a comment */
static void read_config(char *host, size_t hostcap, int *port, int *seconds, int *prio)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "host")) snprintf(host, hostcap, "%s", v);
        else if (!strcmp(line, "port")) *port = atoi(v);
        else if (!strcmp(line, "seconds")) *seconds = atoi(v);
        else if (!strcmp(line, "priority")) *prio = atoi(v);
    }
    fclose(f);
}
#endif

static void hsv(float h, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float x = h * 6.0f; int i = (int)x; float f = x - i;
    float q = 1 - f;
    float R, G, B;
    switch (i % 6) {
    case 0: R = 1; G = f; B = 0; break;
    case 1: R = q; G = 1; B = 0; break;
    case 2: R = 0; G = 1; B = f; break;
    case 3: R = 0; G = q; B = 1; break;
    case 4: R = f; G = 0; B = 1; break;
    default: R = 1; G = 0; B = q; break;
    }
    *r = (uint8_t)(R * 255); *g = (uint8_t)(G * 255); *b = (uint8_t)(B * 255);
}

int main(int argc, char **argv)
{
#ifdef PS5_PAYLOAD
    (void)argc; (void)argv;
    char host_buf[64] = "";
    const char *host = host_buf;
    int port = 19400, seconds = 30, prio = 150;
    read_config(host_buf, sizeof host_buf, &port, &seconds, &prio);
    if (!host_buf[0]) {
        notify("PS5-Ambilight: host= mancante in " CONFIG_PATH);
        return 2;
    }
#else
    if (argc < 2) {
        fprintf(stderr, "usage: %s <hyperion_ip> [port] [seconds] [priority]\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    int port = argc > 2 ? atoi(argv[2]) : 19400;
    int seconds = argc > 3 ? atoi(argv[3]) : 15;
    int prio = argc > 4 ? atoi(argv[4]) : 150;
#endif
    if (prio < HFB_PRIORITY_MIN || prio > HFB_PRIORITY_MAX) {
        fprintf(stderr, "priority must be %d..%d\n", HFB_PRIORITY_MIN, HFB_PRIORITY_MAX);
        return 2;
    }

    int fd = hfb_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "connect to %s:%d failed\n", host, port);
#ifdef PS5_PAYLOAD
        notify("PS5-Ambilight: impossibile connettersi a %s:%d", host, port);
#endif
        return 1;
    }
    if (hfb_register(fd, "PS5-Ambilight", prio) != 0) {
        fprintf(stderr, "register refused by Hyperion\n"); hfb_close(fd);
#ifdef PS5_PAYLOAD
        notify("PS5-Ambilight: Hyperion refused the registration");
#endif
        return 1;
    }
    printf("registered on %s:%d, priority %d, sending for %d s\n", host, port, prio, seconds);
#ifdef PS5_PAYLOAD
    notify("PS5-Ambilight: connesso a Hyperion, test LED per %d s", seconds);
#endif

    static uint8_t img[W * H * 3];
    const int perim = 2 * (W + H) - 4;
    const int fps = 30;
    for (int frame = 0; frame < seconds * fps; frame++) {
        memset(img, 0, sizeof img);
        for (int k = 0; k < perim; k++) {
            int x, y;
            if (k < W)                    { x = k;            y = 0; }
            else if (k < W + H - 1)       { x = W - 1;        y = k - (W - 1); }
            else if (k < 2 * W + H - 2)   { x = W - 1 - (k - (W + H - 2)); y = H - 1; }
            else                          { x = 0;            y = H - 1 - (k - (2 * W + H - 3)); }
            float hue = fmodf((float)(k + frame * 2) / perim, 1.0f);
            uint8_t *p = &img[(y * W + x) * 3];
            hsv(hue, &p[0], &p[1], &p[2]);
            /* thicken the border to 3 px so sampling has something to average */
            for (int d = 1; d < 3; d++) {
                int xi = x == 0 ? d : (x == W - 1 ? W - 1 - d : x);
                int yi = y == 0 ? d : (y == H - 1 ? H - 1 - d : y);
                uint8_t *q = &img[(yi * W + xi) * 3];
                if (q[0] == 0 && q[1] == 0 && q[2] == 0) { q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; }
            }
        }
        int r = hfb_send_image(fd, W, H, img);
        if (r == -1) { fprintf(stderr, "connection lost\n"); return 1; }
        if (r == -2) { fprintf(stderr, "Hyperion reported an error for a frame\n"); }
        usleep(1000000 / fps);
    }
    hfb_close(fd);   /* Hyperion frees our priority on disconnect */
    printf("done\n");
#ifdef PS5_PAYLOAD
    notify("PS5-Ambilight: test terminato");
#endif
    return 0;
}
