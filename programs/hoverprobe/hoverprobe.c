/*
 * hoverprobe - accurate end-to-end Plasma responsiveness instrument.
 *
 * Measures, in one guest process (so injection and detection share a single
 * monotonic clock), the user-perceived latency of three desktop events on a
 * live KDE/Plasma session:
 *
 *   hover_in  : pointer moves ONTO a taskbar/desktop icon  -> first pixel
 *               change in the region of interest (highlight fade-in edge).
 *   hover_out : pointer moves AWAY from the hovered icon    -> first pixel
 *               change (highlight removal edge).
 *   click     : left button pressed on the icon            -> first pixel
 *               change (press effect / menu / launch flash).
 *
 * Each event is timed from the INPUT-INJECTION timestamp (sampled with the
 * guest CLOCK_MONOTONIC immediately before the /dev/mouse write) to the first
 * scanout readback whose region-of-interest hash differs from the reference
 * captured just before injection. Detection is a tight readback loop over a
 * SMALL rectangle via the FB_GPU_SCANOUT_READ ioctl (the same primitive
 * fbstat's `sample-current` uses); the measurement resolution is therefore the
 * per-sample readback period, which the tool measures and reports.
 *
 * Input follows the plan's guest-only policy: absolute 16-bit coordinates are
 * written to /dev/mouse via the same struct mouse_event that /bin/mouseinject
 * uses. Coordinates are bounded to 0..65535. The region of interest is derived
 * in pixels from the icon's abs16 coordinate using the harness mapping
 * (pixel = (abs16 * (res-1) + 32767) / 65535).
 *
 * Note on the hardware cursor: kwin drives the pointer sprite through the DRM
 * cursor plane (DRM_IOCTL_MODE_CURSOR), which is NOT composited into the
 * primary scanout that FB_GPU_SCANOUT_READ reads back. The region-of-interest
 * therefore reflects desktop content (the highlight / press feedback), not the
 * moving cursor sprite.
 *
 * Usage:
 *   hoverprobe [icon_x] [icon_y] [away_x] [away_y] [iters] [rw] [rh]
 *              [settle_ms] [timeout_ms] [calib_samples]
 * All arguments optional; abs16 coords default to the taskbar-left-icons
 * target (11000,64200) and an upper-right desktop rest point (60000,10000).
 * The latter remains outside the large lower-left Kickoff popup, so a menu
 * close probe cannot accidentally activate one of the popup's application
 * tiles.
 */

#include "kernel/inc/types.h"
#include "kernel/inc/errno.h"
#include "kernel/inc/dev/fb.h"
#include "kernel/inc/dev/ps2mouse.h"
#include "kernel/inc/uabi/fcntl.h"
#include "user/user.h"

#define CLOCK_MONOTONIC 1

int clock_gettime(int clockid, struct timespec *tp);

static long long monotonic_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Print a microsecond value as fixed-point milliseconds (%lld.%03lld). */
#define MS_I(us) ((long long)((us) / 1000))
#define MS_F(us) ((long long)(((us) < 0 ? -(us) : (us)) % 1000))

static void sleep_ms(long long ms)
{
    struct timespec ts;

    if (ms <= 0)
        return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
}

static int clamp_abs16(int v)
{
    if (v < 0)
        return 0;
    if (v > 65535)
        return 65535;
    return v;
}

static int arg_int(int argc, char **argv, int idx, int fallback)
{
    if (idx >= argc || !argv[idx] || !argv[idx][0])
        return fallback;
    return atoi(argv[idx]);
}

/* --- pointer injection (guest-only, absolute16) -------------------------- */

static int mouse_fd = -1;

static int inject_abs(int x, int y, int buttons)
{
    struct mouse_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.flags = MOUSE_EVENT_F_ABSOLUTE;
    ev.dx = (int16)clamp_abs16(x);
    ev.dy = (int16)clamp_abs16(y);
    ev.buttons = (uint8)buttons;
    if (write(mouse_fd, &ev, sizeof(ev)) != (int)sizeof(ev))
        return -1;
    return 0;
}

/* --- region-of-interest scanout readback --------------------------------- */

static int fb_fd = -1;
static struct fb_gpu_scanout_read g_req;
static uint32 *g_pixels;
static uint32 g_npix;

/* Read an explicit region of interest and return its FNV-1a hash. */
static int sample_roi_req(struct fb_gpu_scanout_read *req, uint32 *pixels,
                          uint32 npix, uint64 *hash_out)
{
    uint64 h = 1469598103934665603UL;

    if (ioctl(fb_fd, FB_GPU_SCANOUT_READ, req) < 0)
        return -1;
    for (uint32 i = 0; i < npix; i++) {
        h ^= pixels[i];
        h *= 1099511628211UL;
    }
    *hash_out = h;
    return 0;
}

/* Read the primary region of interest and return its FNV-1a hash. */
static int sample_roi(uint64 *hash_out)
{
    return sample_roi_req(&g_req, g_pixels, g_npix, hash_out);
}

/*
 * Poll the region of interest until its hash differs from baseline.
 * Returns 1 = change detected, 0 = timeout, -1 = readback error.
 * On return *t_change_us is the stamp of the detecting sample, *period_us the
 * mean inter-sample interval observed, *samples_out the sample count.
 */
static int poll_for_change(uint64 baseline, long long t_inject_us,
                           long long timeout_us, long long *t_change_us,
                           long long *period_us, long long *samples_out)
{
    long long first_us = 0;
    long long last_us = 0;
    long long samples = 0;
    long long deadline = t_inject_us + timeout_us;
    uint64 h;

    for (;;) {
        long long now;

        if (sample_roi(&h) < 0)
            return -1;
        now = monotonic_us();
        if (samples == 0)
            first_us = now;
        last_us = now;
        samples++;
        *samples_out = samples;
        *period_us = (samples > 1) ? (last_us - first_us) / (samples - 1) : 0;
        if (h != baseline) {
            *t_change_us = now;
            return 1;
        }
        if (now >= deadline) {
            *t_change_us = now;
            return 0;
        }
    }
}

/* Like poll_for_change but over an explicit ROI (used for the menu body). */
static int poll_menu_change(struct fb_gpu_scanout_read *req, uint32 *pixels,
                            uint32 npix, uint64 baseline, long long t_inject_us,
                            long long timeout_us, long long *t_change_us,
                            long long *period_us, long long *samples_out)
{
    long long first_us = 0, last_us = 0, samples = 0;
    long long deadline = t_inject_us + timeout_us;
    uint64 h;

    for (;;) {
        long long now;

        if (sample_roi_req(req, pixels, npix, &h) < 0)
            return -1;
        now = monotonic_us();
        if (samples == 0)
            first_us = now;
        last_us = now;
        samples++;
        *samples_out = samples;
        *period_us = (samples > 1) ? (last_us - first_us) / (samples - 1) : 0;
        if (h != baseline) {
            *t_change_us = now;
            return 1;
        }
        if (now >= deadline) {
            *t_change_us = now;
            return 0;
        }
    }
}

/* Wait for the region of interest to stop changing (settle), or give up. */
static void wait_roi_stable(long long settle_ms)
{
    long long deadline = monotonic_us() + settle_ms * 1000;
    uint64 prev = 0;
    int have_prev = 0;
    int stable = 0;

    for (;;) {
        uint64 h;
        long long now = monotonic_us();

        if (sample_roi(&h) < 0)
            break;
        if (have_prev && h == prev)
            stable++;
        else
            stable = 0;
        prev = h;
        have_prev = 1;
        if (stable >= 3)
            break;
        if (now >= deadline)
            break;
        sleep_ms(15);
    }
}

/* Dump the full primary scanout to a P6 PPM (proof that the menu is open). */
static void dump_full_frame_ppm(const char *path, uint32 xres, uint32 yres)
{
    struct fb_gpu_scanout_read req;
    uint32 *px;
    uint32 n = xres * yres;
    int fd;
    char hdr[64];
    unsigned char *rgb;

    px = malloc(n * sizeof(uint32));
    rgb = malloc((uint64)n * 3);
    if (!px || !rgb) { free(px); free(rgb); return; }
    memset(&req, 0, sizeof(req));
    req.x = 0; req.y = 0; req.w = xres; req.h = yres;
    req.pitch = xres * sizeof(uint32);
    req.pixels = (uint64)px;
    if (ioctl(fb_fd, FB_GPU_SCANOUT_READ, &req) < 0) { free(px); free(rgb); return; }
    for (uint32 i = 0; i < n; i++) {
        uint32 p = px[i];               /* assume XRGB8888 little-endian */
        rgb[i * 3 + 0] = (p >> 16) & 0xff;
        rgb[i * 3 + 1] = (p >> 8) & 0xff;
        rgb[i * 3 + 2] = p & 0xff;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        int hlen = 0;
        const char *magic = "P6\n";
        int ok = 1;
        while (magic[hlen]) hlen++;
        if (write(fd, magic, hlen) != hlen) ok = 0;
        {   /* "W H\n255\n" without snprintf */
            char *w = hdr;
            long long vals[2]; vals[0] = xres; vals[1] = yres;
            for (int k = 0; k < 2; k++) {
                char tmp[16]; int t = 0; long long v = vals[k];
                if (v == 0) tmp[t++] = '0';
                while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
                while (t > 0) *w++ = tmp[--t];
                *w++ = (k == 0) ? ' ' : '\n';
            }
            *w++ = '2'; *w++ = '5'; *w++ = '5'; *w++ = '\n';
            if (write(fd, hdr, (int)(w - hdr)) != (int)(w - hdr)) ok = 0;
        }
        if (ok && write(fd, rgb, (int)((uint64)n * 3)) != (int)((uint64)n * 3))
            ok = 0;
        (void)ok;
        close(fd);
    }
    free(px);
    free(rgb);
}

/* Wait for an EXPLICIT region of interest to stop changing (settle). */
static void wait_req_stable(struct fb_gpu_scanout_read *req, uint32 *pixels,
                            uint32 npix, long long settle_ms)
{
    long long deadline = monotonic_us() + settle_ms * 1000;
    uint64 prev = 0;
    int have_prev = 0;
    int stable = 0;

    for (;;) {
        uint64 h;
        long long now = monotonic_us();

        if (sample_roi_req(req, pixels, npix, &h) < 0)
            break;
        if (have_prev && h == prev)
            stable++;
        else
            stable = 0;
        prev = h;
        have_prev = 1;
        if (stable >= 3)
            break;
        if (now >= deadline)
            break;
        sleep_ms(15);
    }
}

/* --- median helper ------------------------------------------------------- */

static void sort_ll(long long *a, int n)
{
    for (int i = 1; i < n; i++) {
        long long key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

/* Integer sqrt (binary search); inputs are variances of us latencies. */
static long long isqrt_ll(long long v)
{
    long long lo = 0, hi = 40000000; /* 40s in us: > any latency we time */

    if (v <= 0)
        return 0;
    while (lo < hi) {
        long long mid = lo + (hi - lo + 1) / 2;

        if (mid <= v / mid)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* Variance-characterised summary for the tooltip protocol (median/p90/max/
 * stddev are the deliverable for the tooltip-latency metric). Sorts lat[]. */
static void print_tooltip_summary(const char *name, long long *lat, int n,
                                  int iters, int mrx, int mry, int mrw,
                                  int mrh)
{
    long long med = 0, mn = 0, mx = 0, p90 = 0, sd = 0;

    if (n > 0) {
        long long mean = 0, var = 0;
        int i90 = (9 * n) / 10;

        sort_ll(lat, n);
        if (i90 >= n)
            i90 = n - 1;
        med = lat[n / 2];
        mn = lat[0];
        mx = lat[n - 1];
        p90 = lat[i90];
        for (int i = 0; i < n; i++)
            mean += lat[i];
        mean /= n;
        for (int i = 0; i < n; i++) {
            long long d = lat[i] - mean;

            var += d * d;
        }
        var /= n;
        sd = isqrt_ll(var);
    }
    printf("hoverprobe summary event=%s n=%d changed=%d "
           "latency_median_ms=%lld.%03lld latency_p90_ms=%lld.%03lld "
           "latency_min_ms=%lld.%03lld latency_max_ms=%lld.%03lld "
           "latency_stddev_ms=%lld.%03lld tip_rect=%d,%d,%d,%d\n",
           name, iters, n, MS_I(med), MS_F(med), MS_I(p90), MS_F(p90),
           MS_I(mn), MS_F(mn), MS_I(mx), MS_F(mx), MS_I(sd), MS_F(sd),
           mrx, mry, mrw, mrh);
}

/* --- tooltip (hover) protocol --------------------------------------------
 * Measures the taskbar-icon TOOLTIP popup: move the pointer onto the icon
 * (no click) and time until the tip ROI above the icon first changes
 * (tooltip appears = Plasma ToolTipArea show-delay + tooltip QML + kwin
 * popup schedule), then move away and time the ROI reverting (hide). An
 * optional A->B switch phase re-hovers icon A until its tooltip shows, then
 * moves straight to icon B while the dialog is still visible.
 */
static void run_tooltip_protocol(struct fb_gpu_scanout_read *mreq,
                                 uint32 *mpix, uint32 mnpix,
                                 int mrx, int mry, int mrw, int mrh,
                                 int icon_x, int icon_y,
                                 int icon2_x, int icon2_y,
                                 int away_x, int away_y,
                                 int iters, long long settle_ms,
                                 long long timeout_us,
                                 uint32 xres, uint32 yres)
{
    long long open_lat[64], close_lat[64], switch_lat[64];
    int open_n = 0, close_n = 0, switch_n = 0;
    int do_switch = (icon2_x > 0 && icon2_y > 0);

    printf("hoverprobe tooltip_config icon_abs16=%d,%d icon2_abs16=%d,%d "
           "away_abs16=%d,%d tip_rect=%d,%d,%d,%d iters=%d protocol=hover\n",
           icon_x, icon_y, icon2_x, icon2_y, away_x, away_y,
           mrx, mry, mrw, mrh, iters);

    /* Known state: pointer away from the taskbar, no tooltip visible. */
    inject_abs(away_x, away_y, 0);
    sleep_ms(settle_ms);

    for (int it = 0; it < iters; it++) {
        long long t_inject, t_change = 0, period = 0, samples = 0;
        uint64 base;
        int r;

        /* ---- OPEN: pointer rests away; move onto the icon; the tooltip
         * must appear in the tip ROI (show-delay + render + schedule). */
        inject_abs(away_x, away_y, 0);
        sleep_ms(settle_ms);
        wait_req_stable(mreq, mpix, mnpix, settle_ms);
        if (sample_roi_req(mreq, mpix, mnpix, &base) < 0) {
            fprintf(2, "hoverprobe: tooltip baseline readback failed\n");
            return;
        }
        t_inject = monotonic_us();
        inject_abs(icon_x, icon_y, 0);       /* hover, no click */
        r = poll_menu_change(mreq, mpix, mnpix, base, t_inject, timeout_us,
                             &t_change, &period, &samples);
        if (r < 0) {
            fprintf(2, "hoverprobe: tooltip open poll failed\n");
            return;
        }
        {
            long long latency = t_change - t_inject;

            printf("hoverprobe event=tooltip_open iter=%d "
                   "t_inject_ms=%lld.%03lld t_first_change_ms=%lld.%03lld "
                   "latency_ms=%lld.%03lld sampler_period_ms=%lld.%03lld "
                   "samples=%lld result=%s temperature=%s "
                   "tip_rect=%d,%d,%d,%d\n",
                   it + 1, MS_I(t_inject), MS_F(t_inject), MS_I(t_change),
                   MS_F(t_change), MS_I(latency), MS_F(latency), MS_I(period),
                   MS_F(period), samples, r == 1 ? "CHANGED" : "TIMEOUT",
                   it == 0 ? "first" : "repeat", mrx, mry, mrw, mrh);
            if (r == 1)
                open_lat[open_n++] = latency;
        }

        /* Let the tooltip finish its fade-in before measuring the hide. */
        sleep_ms(350);
        wait_req_stable(mreq, mpix, mnpix, settle_ms);
        if (it == 0)
            dump_full_frame_ppm("/kde-plasma-tooltip-proof.ppm", xres, yres);

        /* ---- CLOSE: tooltip visible; move away; ROI reverts on hide. */
        if (sample_roi_req(mreq, mpix, mnpix, &base) < 0) {
            fprintf(2, "hoverprobe: tooltip close baseline readback failed\n");
            return;
        }
        t_inject = monotonic_us();
        inject_abs(away_x, away_y, 0);
        r = poll_menu_change(mreq, mpix, mnpix, base, t_inject, timeout_us,
                             &t_change, &period, &samples);
        if (r < 0) {
            fprintf(2, "hoverprobe: tooltip close poll failed\n");
            return;
        }
        {
            long long latency = t_change - t_inject;

            printf("hoverprobe event=tooltip_close iter=%d "
                   "t_inject_ms=%lld.%03lld t_first_change_ms=%lld.%03lld "
                   "latency_ms=%lld.%03lld sampler_period_ms=%lld.%03lld "
                   "samples=%lld result=%s temperature=%s "
                   "tip_rect=%d,%d,%d,%d\n",
                   it + 1, MS_I(t_inject), MS_F(t_inject), MS_I(t_change),
                   MS_F(t_change), MS_I(latency), MS_F(latency), MS_I(period),
                   MS_F(period), samples, r == 1 ? "CHANGED" : "TIMEOUT",
                   it == 0 ? "first" : "repeat", mrx, mry, mrw, mrh);
            if (r == 1)
                close_lat[close_n++] = latency;
        }
        sleep_ms(settle_ms);
        wait_req_stable(mreq, mpix, mnpix, settle_ms);
    }

    /* ---- SWITCH: hover A until its tooltip shows, then move directly to
     * icon B while the dialog is still visible (per-icon cold question). */
    if (do_switch) {
        for (int it = 0; it < iters; it++) {
            long long t_inject, t_change = 0, period = 0, samples = 0;
            uint64 base;
            int r;

            /* Arm: away -> stable -> hover A -> wait for A's tooltip. */
            inject_abs(away_x, away_y, 0);
            sleep_ms(settle_ms);
            wait_req_stable(mreq, mpix, mnpix, settle_ms);
            if (sample_roi_req(mreq, mpix, mnpix, &base) < 0) {
                fprintf(2, "hoverprobe: tooltip arm readback failed\n");
                return;
            }
            t_inject = monotonic_us();
            inject_abs(icon_x, icon_y, 0);
            r = poll_menu_change(mreq, mpix, mnpix, base, t_inject,
                                 timeout_us, &t_change, &period, &samples);
            if (r < 0) {
                fprintf(2, "hoverprobe: tooltip arm poll failed\n");
                return;
            }
            if (r == 0) {
                printf("hoverprobe event=tooltip_switch iter=%d "
                       "result=ARM_TIMEOUT tip_rect=%d,%d,%d,%d\n",
                       it + 1, mrx, mry, mrw, mrh);
                continue;
            }
            sleep_ms(350);
            wait_req_stable(mreq, mpix, mnpix, settle_ms);

            /* Measured leg: A's tooltip up; move straight onto icon B. */
            if (sample_roi_req(mreq, mpix, mnpix, &base) < 0) {
                fprintf(2, "hoverprobe: tooltip switch readback failed\n");
                return;
            }
            t_inject = monotonic_us();
            inject_abs(icon2_x, icon2_y, 0);
            r = poll_menu_change(mreq, mpix, mnpix, base, t_inject,
                                 timeout_us, &t_change, &period, &samples);
            if (r < 0) {
                fprintf(2, "hoverprobe: tooltip switch poll failed\n");
                return;
            }
            {
                long long latency = t_change - t_inject;

                printf("hoverprobe event=tooltip_switch iter=%d "
                       "t_inject_ms=%lld.%03lld t_first_change_ms=%lld.%03lld "
                       "latency_ms=%lld.%03lld sampler_period_ms=%lld.%03lld "
                       "samples=%lld result=%s temperature=%s "
                       "tip_rect=%d,%d,%d,%d\n",
                       it + 1, MS_I(t_inject), MS_F(t_inject), MS_I(t_change),
                       MS_F(t_change), MS_I(latency), MS_F(latency),
                       MS_I(period), MS_F(period), samples,
                       r == 1 ? "CHANGED" : "TIMEOUT",
                       it == 0 ? "first" : "repeat", mrx, mry, mrw, mrh);
                if (r == 1)
                    switch_lat[switch_n++] = latency;
            }

            /* Dismiss fully before the next arm. */
            inject_abs(away_x, away_y, 0);
            sleep_ms(settle_ms);
            wait_req_stable(mreq, mpix, mnpix, settle_ms);
        }
    }

    print_tooltip_summary("tooltip_open", open_lat, open_n, iters,
                          mrx, mry, mrw, mrh);
    print_tooltip_summary("tooltip_close", close_lat, close_n, iters,
                          mrx, mry, mrw, mrh);
    if (do_switch)
        print_tooltip_summary("tooltip_switch", switch_lat, switch_n, iters,
                              mrx, mry, mrw, mrh);
}

int main(int argc, char **argv)
{
    struct fb_var_screeninfo info;
    int icon_x = clamp_abs16(arg_int(argc, argv, 1, 11000));
    int icon_y = clamp_abs16(arg_int(argc, argv, 2, 64200));
    int away_x = clamp_abs16(arg_int(argc, argv, 3, 60000));
    int away_y = clamp_abs16(arg_int(argc, argv, 4, 10000));
    int iters = arg_int(argc, argv, 5, 6);
    int rw = arg_int(argc, argv, 6, 56);
    int rh = arg_int(argc, argv, 7, 40);
    long long settle_ms = arg_int(argc, argv, 8, 900);
    long long timeout_ms = arg_int(argc, argv, 9, 2500);
    int calib_samples = arg_int(argc, argv, 10, 256);
    /* Menu-mode extension: an ROI over the popup BODY (distinct from the click
     * target) plus a click->open / click-away->close protocol. Enabled when a
     * menu ROI centre pixel is supplied (args 11,12 > 0). Args 13,14 size the
     * menu ROI; arg 15 = iters (default = iters). */
    int menu_px_x = arg_int(argc, argv, 11, 0);
    int menu_px_y = arg_int(argc, argv, 12, 0);
    int menu_rw = arg_int(argc, argv, 13, 64);
    int menu_rh = arg_int(argc, argv, 14, 64);
    int menu_iters = arg_int(argc, argv, 15, iters);
    /* arg 16: run the hover/click phase before menu mode (default 1). Set 0 so
     * the first menu_open is a genuine COLD Kickoff activation (nothing has
     * clicked the launcher yet). */
    int do_hover = arg_int(argc, argv, 16, 1);
    /* arg 17: prewarm Kickoff once (unmeasured open + long wait for full model
     * population + close) before the measured iters. Models a session-start
     * prewarm: the one-time QML-compile/model-build cost is paid here instead
     * of on the user's first click. */
    int prewarm = arg_int(argc, argv, 17, 0);
    /* arg 18: menu protocol. 0 (default) = click protocol (Kickoff open/close,
     * unchanged). 1 = HOVER protocol: move the pointer ONTO the icon with no
     * click and watch the popup ROI for the TOOLTIP that pops above the
     * taskbar icon (Plasma ToolTipArea show-delay + render); move away and
     * watch the ROI revert (tooltip hide). Events are emitted as
     * tooltip_open/tooltip_close with temperature=first|repeat. */
    int menu_protocol = arg_int(argc, argv, 18, 0);
    /* args 19,20: OPTIONAL second icon abs16 coord for the hover protocol's
     * A->B switch phase (hover icon A until its tooltip shows, then move
     * straight to icon B and time the tooltip switch; answers whether tooltip
     * cost is per-icon). 0 = skip the switch phase. */
    int icon2_x = clamp_abs16(arg_int(argc, argv, 19, 0));
    int icon2_y = clamp_abs16(arg_int(argc, argv, 20, 0));
    int menu_mode = (menu_px_x > 0 && menu_px_y > 0);
    uint32 xres, yres;
    int icon_px, icon_py, away_px, away_py;
    int rx, ry;
    uint64 baseline;

    if (iters < 1)
        iters = 1;
    if (iters > 64)
        iters = 64;
    if (rw < 4)
        rw = 4;
    if (rh < 4)
        rh = 4;

    mouse_fd = open("/dev/mouse", O_RDWR);
    if (mouse_fd < 0) {
        fprintf(2, "hoverprobe: open /dev/mouse failed\n");
        return 1;
    }
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        fprintf(2, "hoverprobe: open /dev/fb0 failed\n");
        return 1;
    }
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &info) < 0) {
        fprintf(2, "hoverprobe: FBIOGET_VSCREENINFO failed\n");
        return 1;
    }
    if (info.bits_per_pixel != 32) {
        fprintf(2, "hoverprobe: unsupported fb bpp %u\n", info.bits_per_pixel);
        return 1;
    }
    xres = info.xres;
    yres = info.yres;

    /* abs16 -> pixel using the harness mapping. */
    icon_px = (int)(((long long)icon_x * (xres - 1) + 32767) / 65535);
    icon_py = (int)(((long long)icon_y * (yres - 1) + 32767) / 65535);
    away_px = (int)(((long long)away_x * (xres - 1) + 32767) / 65535);
    away_py = (int)(((long long)away_y * (yres - 1) + 32767) / 65535);

    /* Centre the region of interest on the icon pixel, clamp to screen. */
    rx = icon_px - rw / 2;
    ry = icon_py - rh / 2;
    if (rx < 0)
        rx = 0;
    if (ry < 0)
        ry = 0;
    if ((uint32)(rx + rw) > xres)
        rx = (int)xres - rw;
    if ((uint32)(ry + rh) > yres)
        ry = (int)yres - rh;
    if (rx < 0)
        rx = 0;
    if (ry < 0)
        ry = 0;

    g_npix = (uint32)rw * (uint32)rh;
    g_pixels = malloc(g_npix * sizeof(uint32));
    if (!g_pixels) {
        fprintf(2, "hoverprobe: roi allocation failed\n");
        return 1;
    }
    memset(&g_req, 0, sizeof(g_req));
    g_req.x = (uint32)rx;
    g_req.y = (uint32)ry;
    g_req.w = (uint32)rw;
    g_req.h = (uint32)rh;
    g_req.pitch = (uint32)rw * sizeof(uint32);
    g_req.pixels = (uint64)g_pixels;

    printf("hoverprobe config icon_abs16=%d,%d away_abs16=%d,%d "
           "icon_px=%d,%d away_px=%d,%d rect=%d,%d,%d,%d screen=%ux%u "
           "iters=%d settle_ms=%lld timeout_ms=%lld input_policy=guest-only "
           "coordinate_space=guest-mouseinject-absolute16\n",
           icon_x, icon_y, away_x, away_y, icon_px, icon_py, away_px, away_py,
           rx, ry, rw, rh, xres, yres, iters, settle_ms, timeout_ms);

    /* --- sampler calibration: measure the readback period (resolution). --- */
    if (calib_samples < 8)
        calib_samples = 8;
    if (calib_samples > 4096)
        calib_samples = 4096;
    {
        long long first = 0, last = 0, prev = 0;
        long long minint = -1, maxint = 0;
        int ok = 0;

        for (int i = 0; i < calib_samples; i++) {
            uint64 h;
            long long now;

            if (sample_roi(&h) < 0)
                break;
            now = monotonic_us();
            if (i == 0) {
                first = now;
            } else {
                long long d = now - prev;
                if (minint < 0 || d < minint)
                    minint = d;
                if (d > maxint)
                    maxint = d;
            }
            prev = now;
            last = now;
            ok++;
        }
        if (minint < 0)
            minint = 0;
        {
            long long mean = ok > 1 ? (last - first) / (ok - 1) : 0;

            printf("hoverprobe sampler_calibration samples=%d "
                   "period_mean_ms=%lld.%03lld period_min_ms=%lld.%03lld "
                   "period_max_ms=%lld.%03lld rect=%d,%d,%d,%d\n",
                   ok, MS_I(mean), MS_F(mean), MS_I(minint), MS_F(minint),
                   MS_I(maxint), MS_F(maxint), rx, ry, rw, rh);
        }
    }

    /* Rest the pointer off the icon before starting. */
    inject_abs(away_x, away_y, 0);
    sleep_ms(settle_ms);

    long long timeout_us = timeout_ms * 1000;

    for (int ev = 0; do_hover && ev < 3; ev++) {
        const char *name = ev == 0 ? "hover_in" :
                           ev == 1 ? "hover_out" : "click";
        long long lat[64];
        long long period_sum = 0;
        int changed = 0;

        for (int it = 0; it < iters; it++) {
            long long t_inject, t_change = 0, period = 0, samples = 0;
            int r;
            const char *res;

            /* Precondition + reference capture per event type. */
            if (ev == 0) {
                /* hover_in: rest off icon, reference = un-hovered desktop. */
                inject_abs(away_x, away_y, 0);
                sleep_ms(settle_ms);
                wait_roi_stable(settle_ms);
                if (sample_roi(&baseline) < 0) {
                    fprintf(2, "hoverprobe: baseline readback failed\n");
                    return 1;
                }
                t_inject = monotonic_us();
                inject_abs(icon_x, icon_y, 0);
            } else if (ev == 1) {
                /* hover_out: hover the icon, reference = hovered state. */
                inject_abs(icon_x, icon_y, 0);
                sleep_ms(settle_ms);
                wait_roi_stable(settle_ms);
                if (sample_roi(&baseline) < 0) {
                    fprintf(2, "hoverprobe: baseline readback failed\n");
                    return 1;
                }
                t_inject = monotonic_us();
                inject_abs(away_x, away_y, 0);
            } else {
                /* click: hover the icon, reference = hovered-not-pressed. */
                inject_abs(icon_x, icon_y, 0);
                sleep_ms(settle_ms);
                wait_roi_stable(settle_ms);
                if (sample_roi(&baseline) < 0) {
                    fprintf(2, "hoverprobe: baseline readback failed\n");
                    return 1;
                }
                t_inject = monotonic_us();
                inject_abs(icon_x, icon_y, 1);
            }

            r = poll_for_change(baseline, t_inject, timeout_us, &t_change,
                                &period, &samples);
            if (r < 0) {
                fprintf(2, "hoverprobe: readback failed during poll\n");
                return 1;
            }
            res = (r == 1) ? "CHANGED" : "TIMEOUT";
            {
                long long latency = t_change - t_inject;

                printf("hoverprobe event=%s iter=%d t_inject_ms=%lld.%03lld "
                       "t_first_change_ms=%lld.%03lld latency_ms=%lld.%03lld "
                       "sampler_period_ms=%lld.%03lld samples=%lld result=%s "
                       "rect=%d,%d,%d,%d\n",
                       name, it + 1, MS_I(t_inject), MS_F(t_inject),
                       MS_I(t_change), MS_F(t_change), MS_I(latency),
                       MS_F(latency), MS_I(period), MS_F(period), samples, res,
                       rx, ry, rw, rh);
                if (r == 1) {
                    lat[changed++] = latency;
                    period_sum += period;
                }
            }

            /* Post-event cleanup: release button, dismiss any opened menu. */
            if (ev == 2) {
                inject_abs(icon_x, icon_y, 0);    /* button up */
                sleep_ms(200);
                inject_abs(away_x, away_y, 0);     /* move off */
                inject_abs(away_x, away_y, 1);     /* click empty desktop */
                inject_abs(away_x, away_y, 0);     /* to dismiss Kickoff */
                sleep_ms(settle_ms);
            }
        }

        {
            long long med = 0, mn = 0, mx = 0, pmean = 0;

            if (changed > 0) {
                sort_ll(lat, changed);
                med = lat[changed / 2];
                mn = lat[0];
                mx = lat[changed - 1];
                pmean = period_sum / changed;
            }
            printf("hoverprobe summary event=%s n=%d changed=%d "
                   "latency_median_ms=%lld.%03lld latency_min_ms=%lld.%03lld "
                   "latency_max_ms=%lld.%03lld sampler_period_mean_ms=%lld.%03lld "
                   "rect=%d,%d,%d,%d\n",
                   name, iters, changed, MS_I(med), MS_F(med), MS_I(mn),
                   MS_F(mn), MS_I(mx), MS_F(mx), MS_I(pmean), MS_F(pmean),
                   rx, ry, rw, rh);
        }
    }

    /* --- menu mode: click Kickoff -> menu-body ROI open, click-away -> close --- */
    if (menu_mode) {
        struct fb_gpu_scanout_read mreq;
        uint32 *mpix;
        uint32 mnpix;
        int mrw = menu_rw, mrh = menu_rh;
        int mrx, mry;
        long long open_lat[64], close_lat[64];
        int open_n = 0, close_n = 0;

        if (mrw < 4) mrw = 4;
        if (mrh < 4) mrh = 4;
        if (menu_iters < 1) menu_iters = 1;
        if (menu_iters > 64) menu_iters = 64;

        mrx = menu_px_x - mrw / 2;
        mry = menu_px_y - mrh / 2;
        if (mrx < 0) mrx = 0;
        if (mry < 0) mry = 0;
        if ((uint32)(mrx + mrw) > xres) mrx = (int)xres - mrw;
        if ((uint32)(mry + mrh) > yres) mry = (int)yres - mrh;
        if (mrx < 0) mrx = 0;
        if (mry < 0) mry = 0;

        mnpix = (uint32)mrw * (uint32)mrh;
        mpix = malloc(mnpix * sizeof(uint32));
        if (!mpix) {
            fprintf(2, "hoverprobe: menu roi allocation failed\n");
            return 1;
        }
        memset(&mreq, 0, sizeof(mreq));
        mreq.x = (uint32)mrx;
        mreq.y = (uint32)mry;
        mreq.w = (uint32)mrw;
        mreq.h = (uint32)mrh;
        mreq.pitch = (uint32)mrw * sizeof(uint32);
        mreq.pixels = (uint64)mpix;

        printf("hoverprobe menu_config icon_abs16=%d,%d away_abs16=%d,%d "
               "menu_rect=%d,%d,%d,%d menu_iters=%d\n",
               icon_x, icon_y, away_x, away_y, mrx, mry, mrw, mrh, menu_iters);

        /* Hover (tooltip) protocol: run it and finish; the click protocol
         * below stays byte-identical for the default menu_protocol=0. */
        if (menu_protocol == 1) {
            run_tooltip_protocol(&mreq, mpix, mnpix, mrx, mry, mrw, mrh,
                                 icon_x, icon_y, icon2_x, icon2_y,
                                 away_x, away_y, menu_iters, settle_ms,
                                 timeout_us, xres, yres);
            free(mpix);
            printf("hoverprobe done status=PASS\n");
            free(g_pixels);
            close(fb_fd);
            close(mouse_fd);
            return 0;
        }

        /* Start from a known-closed state (click empty desktop to dismiss). */
        inject_abs(away_x, away_y, 0);
        sleep_ms(settle_ms);
        inject_abs(away_x, away_y, 1);
        sleep_ms(40);
        inject_abs(away_x, away_y, 0);
        sleep_ms(settle_ms);

        /* Optional prewarm: open Kickoff once, wait long enough for the full
         * QML component + app/recents models to build, then close. Unmeasured. */
        if (prewarm) {
            long long t0 = monotonic_us();
            inject_abs(icon_x, icon_y, 1);
            sleep_ms(40);
            inject_abs(icon_x, icon_y, 0);       /* open */
            sleep_ms(4000);                       /* pay cold QML/model cost */
            inject_abs(away_x, away_y, 1);
            sleep_ms(40);
            inject_abs(away_x, away_y, 0);       /* close */
            sleep_ms(settle_ms);
            printf("hoverprobe menu_prewarm done elapsed_ms=%lld.%03lld\n",
                   MS_I(monotonic_us() - t0), MS_F(monotonic_us() - t0));
        }

        for (int it = 0; it < menu_iters; it++) {
            long long t_inject, t_change = 0, period = 0, samples = 0;
            uint64 base;
            int r;
            const char *res;

            /* ---- OPEN: menu is closed; click the icon, watch the menu body ---- */
            inject_abs(away_x, away_y, 0);
            sleep_ms(settle_ms);
            wait_roi_stable(settle_ms);      /* let the icon-ROI settle */
            if (sample_roi_req(&mreq, mpix, mnpix, &base) < 0) {
                fprintf(2, "hoverprobe: menu baseline readback failed\n");
                return 1;
            }
            t_inject = monotonic_us();
            inject_abs(icon_x, icon_y, 1);   /* press Kickoff ... */
            sleep_ms(40);
            inject_abs(icon_x, icon_y, 0);   /* ... and release => complete click */
            r = poll_menu_change(&mreq, mpix, mnpix, base, t_inject, timeout_us,
                                 &t_change, &period, &samples);
            if (r < 0) { fprintf(2, "hoverprobe: menu open poll failed\n"); return 1; }
            res = (r == 1) ? "CHANGED" : "TIMEOUT";
            {
                long long latency = t_change - t_inject;
                printf("hoverprobe event=menu_open iter=%d t_inject_ms=%lld.%03lld "
                       "t_first_change_ms=%lld.%03lld latency_ms=%lld.%03lld "
                       "sampler_period_ms=%lld.%03lld samples=%lld result=%s "
                       "temperature=%s menu_rect=%d,%d,%d,%d\n",
                       it + 1, MS_I(t_inject), MS_F(t_inject), MS_I(t_change),
                       MS_F(t_change), MS_I(latency), MS_F(latency), MS_I(period),
                       MS_F(period), samples, res, it == 0 ? "first" : "repeat",
                       mrx, mry, mrw, mrh);
                if (r == 1) open_lat[open_n++] = latency;
            }

            /* Let the menu finish painting before closing. */
            sleep_ms(settle_ms);
            wait_roi_stable(settle_ms);

            /* Proof capture: dump the full frame while the menu is open (iter 1). */
            if (it == 0)
                dump_full_frame_ppm("/kde-plasma-menu-open-proof.ppm", xres, yres);

            /* ---- CLOSE: menu is open; click empty desktop, watch the body clear ---- */
            if (sample_roi_req(&mreq, mpix, mnpix, &base) < 0) {
                fprintf(2, "hoverprobe: menu close baseline readback failed\n");
                return 1;
            }
            t_inject = monotonic_us();
            inject_abs(away_x, away_y, 1);   /* click empty desktop ... */
            sleep_ms(40);
            inject_abs(away_x, away_y, 0);   /* ... and release => dismiss */
            r = poll_menu_change(&mreq, mpix, mnpix, base, t_inject, timeout_us,
                                 &t_change, &period, &samples);
            if (r < 0) { fprintf(2, "hoverprobe: menu close poll failed\n"); return 1; }
            res = (r == 1) ? "CHANGED" : "TIMEOUT";
            {
                long long latency = t_change - t_inject;
                printf("hoverprobe event=menu_close iter=%d t_inject_ms=%lld.%03lld "
                       "t_first_change_ms=%lld.%03lld latency_ms=%lld.%03lld "
                       "sampler_period_ms=%lld.%03lld samples=%lld result=%s "
                       "temperature=%s menu_rect=%d,%d,%d,%d\n",
                       it + 1, MS_I(t_inject), MS_F(t_inject), MS_I(t_change),
                       MS_F(t_change), MS_I(latency), MS_F(latency), MS_I(period),
                       MS_F(period), samples, res, it == 0 ? "first" : "repeat",
                       mrx, mry, mrw, mrh);
                if (r == 1) close_lat[close_n++] = latency;
            }
            sleep_ms(settle_ms);
        }

        {
            long long med = 0, mn = 0, mx = 0;
            if (open_n > 0) {
                sort_ll(open_lat, open_n);
                med = open_lat[open_n / 2]; mn = open_lat[0]; mx = open_lat[open_n - 1];
            }
            printf("hoverprobe summary event=menu_open n=%d changed=%d "
                   "latency_median_ms=%lld.%03lld latency_min_ms=%lld.%03lld "
                   "latency_max_ms=%lld.%03lld menu_rect=%d,%d,%d,%d\n",
                   menu_iters, open_n, MS_I(med), MS_F(med), MS_I(mn), MS_F(mn),
                   MS_I(mx), MS_F(mx), mrx, mry, mrw, mrh);
            med = mn = mx = 0;
            if (close_n > 0) {
                sort_ll(close_lat, close_n);
                med = close_lat[close_n / 2]; mn = close_lat[0]; mx = close_lat[close_n - 1];
            }
            printf("hoverprobe summary event=menu_close n=%d changed=%d "
                   "latency_median_ms=%lld.%03lld latency_min_ms=%lld.%03lld "
                   "latency_max_ms=%lld.%03lld menu_rect=%d,%d,%d,%d\n",
                   menu_iters, close_n, MS_I(med), MS_F(med), MS_I(mn), MS_F(mn),
                   MS_I(mx), MS_F(mx), mrx, mry, mrw, mrh);
        }
        free(mpix);
    }

    printf("hoverprobe done status=PASS\n");
    free(g_pixels);
    close(fb_fd);
    close(mouse_fd);
    return 0;
}
