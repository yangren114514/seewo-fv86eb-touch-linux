/*
 * seewo-touchd - CVTE 1ff7:0f26 userspace touch driver
 *
 * Reads 62-byte report ID 2 from /dev/hidrawN and injects standard
 * multi-touch events through /dev/uinput.
 *
 * Report layout (Windows 8 compatible):
 *   byte 0      : report ID = 0x02
 *   6 records x 10 bytes:
 *     +0 flags (bit0 tip, bit1 in-range, bit2 confidence)
 *     +1 contact id
 *     +2..3 X uint16 LE (0..32767)
 *     +4..5 Y uint16 LE (0..32767)
 *     +6..7 width uint16 LE
 *     +8..9 height uint16 LE
 *   byte 61     : contact count
 *
 * Additional features:
 *   - HIDIOCSFEATURE(Report ID 3) sent right after opening hidraw, so the
 *     controller leaves "low rate" mode and reports at full speed.
 *     Value configurable: --feature-value N  or  SEEWO_TOUCH_FEATURE_VALUE.
 *   - Reporting-rate diagnostics: every 10 s log how many frames arrived
 *     (and warn loudly when zero).
 *   - --probe   : no uinput; dump device, feature-report result, per-length
 *                 report histogram and the first frames as hex.
 *   - --replay  : feed a Windows capture file through the very same
 *                 parse + uinput injection path, without touching hidraw.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#if defined(__has_include)
#  if __has_include(<linux/hidraw.h>)
#    include <linux/hidraw.h>
#  endif
#endif

/*
 * Some minimal build roots ship linux/input.h but not linux/hidraw.h.
 * Fall back to the kernel ABI definition:
 *   HIDIOCSFEATURE(len) = _IOWR('H', 0x06, len)
 */
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len) _IOC(_IOC_WRITE | _IOC_READ, 'H', 0x06, len)
#endif

#define LOG_PATH "/var/log/seewo-touchd.log"
#define LOG_MAX_BYTES (1024 * 1024)
#define REPORT_ID 0x02
#define REPORT_LEN 62
#define FINGER_COUNT 6
#define FINGER_SIZE 10
#define SLOT_COUNT 10
#define VID 0x1FF7
#define PID 0x0F26

#define FEATURE_REPORT_ID 0x03
#define FEATURE_DEFAULT_VALUE 0x0A   /* contact max = 10 */
#define STAT_INTERVAL_MS 10000

static int g_debug = 0;
static int g_feature_value = FEATURE_DEFAULT_VALUE;
static const char *g_feature_source = "default";
static volatile sig_atomic_t g_stop = 0;
static FILE *g_log = NULL;

static void log_rotate(void)
{
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES)
    {
        if (g_log) { fclose(g_log); g_log = NULL; }
        unlink(LOG_PATH ".1");
        rename(LOG_PATH, LOG_PATH ".1");
    }
    if (!g_log)
        g_log = fopen(LOG_PATH, "a");
}

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    time_t now;
    struct tm tmv;
    char ts[32];

    log_rotate();
    if (!g_log) return;

    now = time(NULL);
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(g_log, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);

    if (g_debug)
    {
        fprintf(stderr, "%s ", ts);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fprintf(stderr, "\n");
    }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int find_hidraw(char *out, size_t outlen)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir("/sys/class/hidraw");
    if (!dir) return -1;

    while ((de = readdir(dir)) != NULL)
    {
        char uevent[512];
        char line[256];
        FILE *f;
        int match = 0;

        if (strncmp(de->d_name, "hidraw", 6) != 0) continue;
        snprintf(uevent, sizeof(uevent), "/sys/class/hidraw/%s/device/uevent", de->d_name);
        f = fopen(uevent, "r");
        if (!f) continue;
        while (fgets(line, sizeof(line), f))
        {
            if (strstr(line, "HID_ID=0003:00001FF7:00000F26") != NULL)
            {
                match = 1;
                break;
            }
        }
        fclose(f);
        if (match)
        {
            snprintf(out, outlen, "/dev/%s", de->d_name);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

/*
 * Send Feature Report ID 3 (Usage 0x55 "Contact Max") to switch the
 * controller into normal reporting mode.  len passed to HIDIOCSFEATURE
 * includes the report-ID byte, hence 2.
 */
static int send_feature_report(int fd, int value, int verbose)
{
    unsigned char feat[2];
    int rc;

    feat[0] = FEATURE_REPORT_ID;
    feat[1] = (unsigned char)(value & 0xFF);

    rc = ioctl(fd, HIDIOCSFEATURE(2), feat);
    if (rc < 0)
    {
        log_msg("HIDIOCSFEATURE(report 3, value=0x%02X) failed: %s", feat[1], strerror(errno));
        if (verbose) printf("[probe] HIDIOCSFEATURE(report 3) failed: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }
    log_msg("sent feature report 3 = 0x%02X (%d)", feat[1], feat[1]);
    if (verbose) printf("[probe] HIDIOCSFEATURE(report 3) ok, value=0x%02X (%d)\n", feat[1], feat[1]);
    return 0;
}

static int emit_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    ssize_t n;

    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;

    do {
        n = write(fd, &ev, sizeof(ev));
    } while (n < 0 && errno == EINTR);
    return (n == (ssize_t)sizeof(ev)) ? 0 : -1;
}

static int open_uinput(void)
{
    int fd;
    struct uinput_user_dev udev;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
    {
        log_msg("open /dev/uinput failed: %s", strerror(errno));
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
    {
        log_msg("UI_SET_EVBIT failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_WIDTH_MAJOR) < 0)
    {
        log_msg("UI_SET_KEYBIT/ABSBIT failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0)
    {
        log_msg("UI_SET_PROPBIT INPUT_PROP_DIRECT failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    memset(&udev, 0, sizeof(udev));
    snprintf(udev.name, sizeof(udev.name), "Seewo FV86EB Touchscreen");
    udev.id.bustype = BUS_USB;
    udev.id.vendor = VID;
    udev.id.product = PID;
    udev.id.version = 1;

    udev.absmin[ABS_MT_SLOT] = 0;
    udev.absmax[ABS_MT_SLOT] = SLOT_COUNT - 1;
    udev.absmin[ABS_MT_TRACKING_ID] = 0;
    udev.absmax[ABS_MT_TRACKING_ID] = 65535;
    udev.absmin[ABS_MT_POSITION_X] = 0;
    udev.absmax[ABS_MT_POSITION_X] = 32767;
    udev.absmin[ABS_MT_POSITION_Y] = 0;
    udev.absmax[ABS_MT_POSITION_Y] = 32767;
    udev.absmin[ABS_MT_TOUCH_MAJOR] = 0;
    udev.absmax[ABS_MT_TOUCH_MAJOR] = 32767;
    udev.absmin[ABS_MT_WIDTH_MAJOR] = 0;
    udev.absmax[ABS_MT_WIDTH_MAJOR] = 32767;

    if (write(fd, &udev, sizeof(udev)) != (ssize_t)sizeof(udev))
    {
        log_msg("write uinput_user_dev failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0)
    {
        log_msg("UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    usleep(200000);
    log_msg("uinput device created: Seewo FV86EB Touchscreen");
    return fd;
}

static void process_report(int ufd, const uint8_t *buf, int *slot_id, int *slot_tip)
{
    int i, s;
    int seen[SLOT_COUNT];
    int active = 0;

    memset(seen, 0, sizeof(seen));

    for (i = 0; i < FINGER_COUNT; i++)
    {
        int base = 1 + i * FINGER_SIZE;
        uint8_t flags = buf[base];
        int tip = (flags & 0x01) ? 1 : 0;
        int cid = buf[base + 1];
        int x = (int)(buf[base + 2] | (buf[base + 3] << 8));
        int y = (int)(buf[base + 4] | (buf[base + 5] << 8));
        int w = (int)(buf[base + 6] | (buf[base + 7] << 8));
        int h = (int)(buf[base + 8] | (buf[base + 9] << 8));
        int slot = -1;
        int is_new = 0;

        if (!tip) continue;
        active = 1;

        for (s = 0; s < SLOT_COUNT; s++)
        {
            if (slot_tip[s] && slot_id[s] == cid)
            {
                slot = s;
                break;
            }
        }
        if (slot < 0)
        {
            for (s = 0; s < SLOT_COUNT; s++)
            {
                if (slot_id[s] < 0)
                {
                    slot = s;
                    slot_id[s] = cid;
                    is_new = 1;
                    break;
                }
            }
        }
        if (slot < 0) continue;
        slot_tip[slot] = 1;
        seen[slot] = 1;

        emit_event(ufd, EV_ABS, ABS_MT_SLOT, slot);
        if (is_new)
            emit_event(ufd, EV_ABS, ABS_MT_TRACKING_ID, cid);
        emit_event(ufd, EV_ABS, ABS_MT_POSITION_X, x);
        emit_event(ufd, EV_ABS, ABS_MT_POSITION_Y, y);
        emit_event(ufd, EV_ABS, ABS_MT_TOUCH_MAJOR, (w > h) ? w : h);
        emit_event(ufd, EV_ABS, ABS_MT_WIDTH_MAJOR, w);

        if (g_debug)
            log_msg("touch tip=1 id=%d x=%d y=%d w=%d h=%d slot=%d", cid, x, y, w, h, slot);
    }

    for (s = 0; s < SLOT_COUNT; s++)
    {
        if (slot_id[s] >= 0 && !seen[s])
        {
            if (g_debug)
                log_msg("touch tip=0 id=%d slot=%d", slot_id[s], s);
            emit_event(ufd, EV_ABS, ABS_MT_SLOT, s);
            emit_event(ufd, EV_ABS, ABS_MT_TRACKING_ID, -1);
            slot_id[s] = -1;
            slot_tip[s] = 0;
        }
    }

    emit_event(ufd, EV_KEY, BTN_TOUCH, active ? 1 : 0);
    emit_event(ufd, EV_SYN, SYN_REPORT, 0);
}

static void bbox_update(const uint8_t *buf, int *minx, int *maxx, int *miny, int *maxy)
{
    int i;
    for (i = 0; i < FINGER_COUNT; i++)
    {
        int base = 1 + i * FINGER_SIZE;
        int x, y;
        if (!(buf[base] & 0x01)) continue;
        x = (int)(buf[base + 2] | (buf[base + 3] << 8));
        y = (int)(buf[base + 4] | (buf[base + 5] << 8));
        if (x < *minx) *minx = x;
        if (x > *maxx) *maxx = x;
        if (y < *miny) *miny = y;
        if (y > *maxy) *maxy = y;
    }
}

static void stat_report(int frames, long long elapsed_ms, const char *who)
{
    double secs = (double)elapsed_ms / 1000.0;
    double fps = (secs > 0.0) ? ((double)frames / secs) : 0.0;
    log_msg("%s: %d frames in %.1fs (%.2f fps)", who, frames, secs, fps);
    if (g_debug)
        fprintf(stderr, "%s: %d frames in %.1fs (%.2f fps)\n", who, frames, secs, fps);
    if (frames == 0)
        log_msg("WARNING: 0 touch reports in the last %.1fs - device idle or not reporting", secs);
}

/* ------------------------------------------------------------------ */
/* --selftest : prove uinput works without any hardware               */
/* ------------------------------------------------------------------ */
static int selftest(void)
{
    int fd;

    fd = open_uinput();
    if (fd < 0)
    {
        fprintf(stderr, "SELFTEST_FAIL: cannot create uinput device\n");
        return 1;
    }

    emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_X, 1000);
    emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, 2000);
    emit_event(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 20);
    emit_event(fd, EV_ABS, ABS_MT_WIDTH_MAJOR, 20);
    emit_event(fd, EV_KEY, BTN_TOUCH, 1);
    emit_event(fd, EV_SYN, SYN_REPORT, 0);
    usleep(200000);

    emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit_event(fd, EV_KEY, BTN_TOUCH, 0);
    emit_event(fd, EV_SYN, SYN_REPORT, 0);
    usleep(100000);

    printf("SELFTEST_OK: uinput device created and synthetic MT events injected\n");
    log_msg("SELFTEST_OK");
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* --probe : device + feature report + 15 s of raw statistics          */
/* ------------------------------------------------------------------ */
static int probe_mode(int seconds)
{
    char path[300];
    int fd;
    int lens[256];
    int total = 0;
    int shown = 0;
    long long t0;
    struct pollfd pfd;

    memset(lens, 0, sizeof(lens));

    printf("[probe] looking for hidraw 1ff7:0f26 ...\n");
    if (find_hidraw(path, sizeof(path)) < 0)
    {
        printf("[probe] RESULT: device NOT found (/sys/class/hidraw has no 1ff7:0f26)\n");
        log_msg("probe: device not found");
        return 2;
    }
    printf("[probe] device: %s\n", path);

    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("[probe] open %s failed: %s\n", path, strerror(errno));
        log_msg("probe: open %s failed: %s", path, strerror(errno));
        return 3;
    }

    printf("[probe] feature value = 0x%02X (%d), source=%s\n",
           g_feature_value, g_feature_value, g_feature_source);
    (void)send_feature_report(fd, g_feature_value, 1);

    printf("[probe] reading for %d s ...\n", seconds);
    fflush(stdout);

    t0 = now_ms();
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    while (!g_stop && (now_ms() - t0) < (long long)seconds * 1000LL)
    {
        int pr = poll(&pfd, 1, 200);
        if (pr < 0)
        {
            if (errno == EINTR) continue;
            printf("[probe] poll failed: %s\n", strerror(errno));
            break;
        }
        if (pr == 0) continue;

        {
            uint8_t buf[256];
            ssize_t n = read(fd, buf, sizeof(buf));
            int i;
            if (n <= 0)
            {
                if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
                printf("[probe] read error/EOF: n=%zd errno=%s\n", n, strerror(errno));
                break;
            }
            total++;
            if (n < 256) lens[n]++;
            if (shown < 20)
            {
                printf("[probe] frame %d len=%zd :", total, n);
                for (i = 0; i < n; i++) printf(" %02X", buf[i]);
                printf("\n");
                shown++;
            }
        }
    }

    printf("[probe] total frames in %d s = %d (%.2f fps)\n", seconds, total, (double)total / (double)seconds);
    printf("[probe] histogram by report length:\n");
    {
        int i;
        for (i = 1; i < 256; i++)
            if (lens[i]) printf("[probe]   len=%d : %d\n", i, lens[i]);
    }
    if (total == 0)
        printf("[probe] WARNING: device produced no reports at all\n");

    log_msg("probe: %d frames in %d s, feature=0x%02X", total, seconds, g_feature_value);
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* --replay : feed a Windows capture file through parse + uinput      */
/* ------------------------------------------------------------------ */
static int parse_hex(const char *s, uint8_t *out, int maxlen)
{
    int n = 0;
    while (s[0] != '\0' && s[1] != '\0')
    {
        char t[3];
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]))
            return -1;
        t[0] = s[0];
        t[1] = s[1];
        t[2] = '\0';
        out[n++] = (uint8_t)strtoul(t, NULL, 16);
        if (n >= maxlen) return n;
        s += 2;
    }
    if (s[0] != '\0') return -1;
    return n;
}

/*
 * Capture line format:  <seconds.milliseconds> <source> <hex bytes>
 * Everything else (# comments, ===PHASE ..., ### ...) is ignored.
 */
static int parse_capture_line(char *line, uint8_t *buf, int maxlen)
{
    char *p = line;
    char *f[3];
    int nf = 0;

    while (*p != '\0' && nf < 3)
    {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\r' || *p == '\n') break;
        f[nf++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p != '\0') { *p = '\0'; p++; }
    }
    if (nf != 3) return -1;
    if (!isdigit((unsigned char)f[0][0])) return -1;
    return parse_hex(f[2], buf, maxlen);
}

static int replay_mode(const char *file, int delay_ms)
{
    FILE *f;
    int ufd;
    int slot_id[SLOT_COUNT];
    int slot_tip[SLOT_COUNT];
    char line[4096];
    int lines = 0, frames = 0, skipped = 0, badlen = 0;
    int minx = INT_MAX, maxx = -1, miny = INT_MAX, maxy = -1;
    int i;

    f = fopen(file, "r");
    if (!f)
    {
        fprintf(stderr, "cannot open %s: %s\n", file, strerror(errno));
        log_msg("replay: cannot open %s: %s", file, strerror(errno));
        return 2;
    }

    printf("[replay] file: %s\n", file);
    ufd = open_uinput();
    if (ufd < 0)
    {
        fprintf(stderr, "replay: cannot create uinput device\n");
        fclose(f);
        return 3;
    }

    for (i = 0; i < SLOT_COUNT; i++) { slot_id[i] = -1; slot_tip[i] = 0; }

    while (fgets(line, sizeof(line), f))
    {
        uint8_t buf[256];
        int n;

        lines++;
        n = parse_capture_line(line, buf, (int)sizeof(buf));
        if (n < 0) { skipped++; continue; }
        if (n != REPORT_LEN || buf[0] != REPORT_ID)
        {
            badlen++;
            continue;
        }
        process_report(ufd, buf, slot_id, slot_tip);
        bbox_update(buf, &minx, &maxx, &miny, &maxy);
        frames++;
        if ((frames % 500) == 0)
            printf("[replay] %d frames ...\n", frames);
        if (delay_ms > 0)
            usleep((useconds_t)delay_ms * 1000);
    }
    fclose(f);

    printf("[replay] lines=%d frames=%d skipped=%d badlen=%d\n", lines, frames, skipped, badlen);
    if (frames > 0)
        printf("[replay] tip bbox: X %d..%d  Y %d..%d\n", minx, maxx, miny, maxy);
    printf("[replay] done\n");
    log_msg("replay: %s lines=%d frames=%d skipped=%d badlen=%d", file, lines, frames, skipped, badlen);

    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --debug                  verbose logging to stderr\n");
    printf("  --selftest               create uinput device, inject one tap, exit\n");
    printf("  --probe                  show device, send feature report, 15 s report stats\n");
    printf("  --probe-seconds N        how long --probe listens (default 15)\n");
    printf("  --replay FILE            replay a Windows capture file through uinput\n");
    printf("  --replay-delay-ms N      delay between replayed frames (default 3)\n");
    printf("  --feature-value N        feature report 3 value (0..255, default 10)\n");
    printf("  --help                   this text\n");
    printf("\nEnvironment: SEEWO_TOUCH_FEATURE_VALUE overrides the default feature value.\n");
}

int main(int argc, char **argv)
{
    int i;
    int ufd;
    int hfd;
    int slot_id[SLOT_COUNT];
    int slot_tip[SLOT_COUNT];
    char path[300];
    int opt_feature = -1;
    const char *replay_file = NULL;
    int replay_delay_ms = 3;
    int probe_seconds = 15;
    int want_probe = 0;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--debug") == 0) g_debug = 1;
        else if (strcmp(argv[i], "--selftest") == 0) return selftest();
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(argv[i], "--probe") == 0) want_probe = 1;
        else if (strcmp(argv[i], "--probe-seconds") == 0 && i + 1 < argc) { probe_seconds = atoi(argv[++i]); want_probe = 1; }
        else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) replay_file = argv[++i];
        else if (strcmp(argv[i], "--replay-delay-ms") == 0 && i + 1 < argc) replay_delay_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--feature-value") == 0 && i + 1 < argc) opt_feature = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    if (opt_feature >= 0)
    {
        g_feature_value = opt_feature & 0xFF;
        g_feature_source = "cmdline";
    }
    else
    {
        const char *env = getenv("SEEWO_TOUCH_FEATURE_VALUE");
        if (env != NULL && env[0] != '\0')
        {
            g_feature_value = atoi(env) & 0xFF;
            g_feature_source = "env";
        }
        else
        {
            g_feature_value = FEATURE_DEFAULT_VALUE;
            g_feature_source = "default";
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (probe_seconds < 1) probe_seconds = 15;

    if (want_probe)
    {
        log_msg("seewo-touchd probe mode (feature=0x%02X source=%s, %ds)",
                g_feature_value, g_feature_source, probe_seconds);
        return probe_mode(probe_seconds);
    }

    if (replay_file != NULL)
    {
        log_msg("seewo-touchd replay mode: file=%s delay=%dms", replay_file, replay_delay_ms);
        return replay_mode(replay_file, replay_delay_ms);
    }

    log_msg("seewo-touchd starting (debug=%d, feature=0x%02X source=%s)",
            g_debug, g_feature_value, g_feature_source);

    ufd = open_uinput();
    if (ufd < 0)
    {
        log_msg("cannot create uinput device; exit for systemd restart");
        return 1;
    }

    for (i = 0; i < SLOT_COUNT; i++) { slot_id[i] = -1; slot_tip[i] = 0; }

    while (!g_stop)
    {
        long long win_start;
        int frames = 0;

        if (find_hidraw(path, sizeof(path)) < 0)
        {
            log_msg("hidraw 1ff7:0f26 not found; retrying in 2s");
            sleep(2);
            continue;
        }

        hfd = open(path, O_RDONLY);
        if (hfd < 0)
        {
            log_msg("open %s failed: %s; retry", path, strerror(errno));
            sleep(2);
            continue;
        }

        log_msg("opened %s", path);
        (void)send_feature_report(hfd, g_feature_value, 0);
        for (i = 0; i < SLOT_COUNT; i++) { slot_id[i] = -1; slot_tip[i] = 0; }

        win_start = now_ms();
        frames = 0;

        while (!g_stop)
        {
            uint8_t buf[128];
            struct pollfd pfd;
            int pr;
            ssize_t n;

            pfd.fd = hfd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            pr = poll(&pfd, 1, 1000);
            if (pr < 0)
            {
                if (errno == EINTR) continue;
                log_msg("poll %s failed: %s", path, strerror(errno));
                break;
            }
            if (pr == 0)
            {
                long long el = now_ms() - win_start;
                if (el >= STAT_INTERVAL_MS)
                {
                    stat_report(frames, el, "rate");
                    if (frames == 0)
                        (void)send_feature_report(hfd, g_feature_value, 0);
                    frames = 0;
                    win_start = now_ms();
                }
                continue;
            }

            n = read(hfd, buf, sizeof(buf));
            if (n < 0)
            {
                if (errno == EINTR) continue;
                log_msg("read %s failed: %s", path, strerror(errno));
                break;
            }
            if (n == 0)
            {
                log_msg("read %s returned EOF", path);
                break;
            }
            if (n != REPORT_LEN || buf[0] != REPORT_ID)
            {
                if (g_debug) log_msg("skip report len=%zd id=0x%02x", n, n > 0 ? buf[0] : 0);
                continue;
            }

            if (g_debug)
                log_msg("report len=%zd count=%d", n, buf[REPORT_LEN - 1]);

            process_report(ufd, buf, slot_id, slot_tip);
            frames++;

            {
                long long el = now_ms() - win_start;
                if (el >= STAT_INTERVAL_MS)
                {
                    stat_report(frames, el, "rate");
                    frames = 0;
                    win_start = now_ms();
                }
            }
        }

        close(hfd);
        log_msg("closed %s; waiting for re-enumeration", path);
        sleep(1);
    }

    log_msg("seewo-touchd stopping");
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    return 0;
}
