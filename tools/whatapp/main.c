/* whatapp - read-only helper: logs which processes carry an application title id.
 *
 * Based on the `ps` sample of the ps5-payload-sdk,
 * Copyright (C) 2023 John Törnblom, https://github.com/ps5-payload-dev/sdk
 * Modifications Copyright (C) 2026 Ozzucap.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any
 * later version. See COPYING.
 *
 * For 180 s it watches which processes have an application title id and
 * writes every change to /data/ps5-ambilight/apps.log. Games and store apps
 * show up with type "PPSA", system processes with "NPXS"; on the home screen
 * only the system ones are present.
 */
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  uint32_t app_type;
  char     title_id[10];
  char     unknown2[0x3c];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

typedef struct notify_request { char useless1[45]; char message[3075]; } notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
static void notify(const char *msg)
{
  notify_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", msg);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

#define DUR 180

static int snapshot(char *out, size_t cap)
{
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t sz = 0;
  void *buf;
  size_t n = 0;
  app_info_t ai;

  out[0] = 0;
  if(sysctl(mib, 4, NULL, &sz, NULL, 0)) return -1;
  sz += sz / 8;
  if(!(buf = malloc(sz))) return -1;
  if(sysctl(mib, 4, buf, &sz, NULL, 0)) { free(buf); return -1; }
  for(void *p = buf; p < buf + sz;) {
    struct kinfo_proc *ki = (struct kinfo_proc *)p;

    p += ki->ki_structsize;
    memset(&ai, 0, sizeof ai);
    if(sceKernelGetAppInfo(ki->ki_pid, &ai) || !ai.title_id[0]) continue;
    n += snprintf(out + n, cap - n, "[%s type=%u appid=%04x comm=%s] ",
                  ai.title_id, ai.app_type, ai.app_id, ki->ki_comm);
    if(n >= cap - 128) break;
  }
  free(buf);
  return 0;
}

int main(void)
{
  static char cur[4096], prev[4096];
  struct timespec t0, t;
  FILE *f;

  mkdir("/data/ps5-ambilight", 0777);
  f = fopen("/data/ps5-ambilight/apps.log", "w");
  if(!f) { notify("whatapp: cannot write apps.log"); return 1; }
  notify("whatapp: logging for 3 minutes, switch screens now");
  clock_gettime(CLOCK_MONOTONIC, &t0);
  prev[0] = 1;
  for(;;) {
    clock_gettime(CLOCK_MONOTONIC, &t);
    if(t.tv_sec - t0.tv_sec >= DUR) break;
    if(snapshot(cur, sizeof cur) == 0 && strcmp(cur, prev)) {
      fprintf(f, "t=%3lds  %s\n", (long)(t.tv_sec - t0.tv_sec), cur[0] ? cur : "(no process with a title id)");
      fflush(f);
      strcpy(prev, cur);
    }
    usleep(500000);
  }
  fprintf(f, "done\n");
  fclose(f);
  notify("whatapp: done, apps.log is ready");
  return 0;
}
