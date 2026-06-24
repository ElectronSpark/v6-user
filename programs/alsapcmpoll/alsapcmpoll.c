#include "kernel/inc/types.h"
#include "kernel/inc/syscall.h"
#include "kernel/inc/uabi/fcntl.h"
#include "kernel/inc/uabi/poll.h"
#include "user/user.h"

#define SNDRV_PCM_IOCTL_PVERSION      0x80044100
#define SNDRV_PCM_IOCTL_HW_REFINE     0xc2604110
#define SNDRV_PCM_IOCTL_HW_PARAMS     0xc2604111
#define SNDRV_PCM_IOCTL_SW_PARAMS     0xc0884113
#define SNDRV_PCM_IOCTL_STATUS        0x80984120
#define SNDRV_PCM_IOCTL_DELAY         0x80084121
#define SNDRV_PCM_IOCTL_PREPARE       0x4140
#define SNDRV_PCM_IOCTL_DROP          0x4143
#define SNDRV_PCM_IOCTL_DRAIN         0x4144

#define SNDRV_PCM_STATE_OPEN     0
#define SNDRV_PCM_STATE_SETUP    1
#define SNDRV_PCM_STATE_PREPARED 2
#define SNDRV_PCM_STATE_RUNNING  3
#define SNDRV_PCM_STATE_XRUN     4
#define SNDRV_PCM_STATE_PAUSED   6
#define SNDRV_PCM_STATE_SUSPENDED 7
#define SNDRV_PCM_STATE_DISCONNECTED 8

#define SNDRV_PCM_ACCESS_RW_INTERLEAVED 3
#define SNDRV_PCM_FORMAT_S16_LE 2
#define SNDRV_PCM_SUBFORMAT_STD 0

#define SNDRV_PCM_HW_PARAM_ACCESS       0
#define SNDRV_PCM_HW_PARAM_FORMAT       1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT    2
#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS  8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS   9
#define SNDRV_PCM_HW_PARAM_CHANNELS     10
#define SNDRV_PCM_HW_PARAM_RATE         11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME  12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE  13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS      15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME  16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE  17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME    19

#define RATE_HZ 48000U
#define CHANNELS 2U
#define FRAME_BYTES 4U
#define PERIOD_FRAMES 512U
#define BUFFER_FRAMES (PERIOD_FRAMES * 4U)
#define ALSA_BOUNDARY 0x40000000ULL

typedef struct alsa_mask {
    uint bits[8];
} alsa_mask_t;

typedef struct alsa_interval {
    uint min, max;
    uint openmin : 1, openmax : 1, integer : 1, empty : 1;
} alsa_interval_t;

typedef struct alsa_pcm_hw_params {
    uint flags;
    alsa_mask_t masks[3];
    alsa_mask_t mres[5];
    alsa_interval_t intervals[12];
    alsa_interval_t ires[9];
    uint rmask;
    uint cmask;
    uint info;
    uint msbits;
    uint rate_num;
    uint rate_den;
    uint64 fifo_size;
    unsigned char reserved[64];
} alsa_pcm_hw_params_t;

typedef struct alsa_pcm_sw_params {
    int tstamp_mode;
    uint period_step;
    uint sleep_min;
    uint64 avail_min;
    uint64 xfer_align;
    uint64 start_threshold;
    uint64 stop_threshold;
    uint64 silence_threshold;
    uint64 silence_size;
    uint64 boundary;
    uint proto;
    uint tstamp_type;
    unsigned char reserved[56];
} alsa_pcm_sw_params_t;

typedef struct alsa_timespec {
    int64 tv_sec;
    int64 tv_nsec;
} alsa_timespec_t;

typedef struct alsa_pcm_status {
    int state;
    int pad1;
    alsa_timespec_t trigger_tstamp;
    alsa_timespec_t tstamp;
    uint64 appl_ptr;
    uint64 hw_ptr;
    int64 delay;
    uint64 avail;
    uint64 avail_max;
    uint64 overrange;
    int suspended_state;
    uint audio_tstamp_data;
    alsa_timespec_t audio_tstamp;
    alsa_timespec_t driver_tstamp;
    uint audio_tstamp_accuracy;
    unsigned char reserved[20];
} alsa_pcm_status_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

_Static_assert(sizeof(alsa_mask_t) == 32, "alsa_mask size");
_Static_assert(sizeof(alsa_interval_t) == 12, "alsa_interval size");
_Static_assert(sizeof(alsa_pcm_hw_params_t) == 608, "alsa hw params size");
_Static_assert(sizeof(alsa_pcm_sw_params_t) == 136, "alsa sw params size");
_Static_assert(sizeof(alsa_pcm_status_t) == 152, "alsa status size");

#if defined(__riscv)
static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    register int64 a7 asm("a7") = num;
    register int64 a0 asm("a0") = a;
    register int64 a1 asm("a1") = b;
    register int64 a2 asm("a2") = c;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
#elif defined(__x86_64__)
static inline int64 raw_syscall3(int num, int64 a, int64 b, int64 c)
{
    int64 ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"((int64)num), "D"(a), "S"(b), "d"(c)
                 : "rcx", "r11", "memory");
    return ret;
}
#else
#error "raw_syscall3 is not defined for this architecture"
#endif

static int poll_raw(struct pollfd *fds, int nfds, int timeout)
{
    return (int)raw_syscall3(SYS_poll, (int64)fds, nfds, timeout);
}

static int saved_errno(int ret)
{
#ifdef HOST_LIBC_PROGRAM
    return ret < 0 ? errno : 0;
#else
    return ret < 0 ? -ret : 0;
#endif
}

static int raw_errno(int ret)
{
    return ret < 0 ? -ret : 0;
}

static const char *state_name(int state)
{
    switch (state) {
    case SNDRV_PCM_STATE_OPEN:
        return "OPEN";
    case SNDRV_PCM_STATE_SETUP:
        return "SETUP";
    case SNDRV_PCM_STATE_PREPARED:
        return "PREPARED";
    case SNDRV_PCM_STATE_RUNNING:
        return "RUNNING";
    case SNDRV_PCM_STATE_XRUN:
        return "XRUN";
    case SNDRV_PCM_STATE_PAUSED:
        return "PAUSED";
    case SNDRV_PCM_STATE_SUSPENDED:
        return "SUSPENDED";
    case SNDRV_PCM_STATE_DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "UNKNOWN";
    }
}

static void print_revent_names(short revents)
{
    int any = 0;

    if (revents & POLLOUT) {
        printf("POLLOUT");
        any = 1;
    }
    if (revents & POLLERR) {
        printf("%sPOLLERR", any ? "|" : "");
        any = 1;
    }
    if (revents & POLLHUP) {
        printf("%sPOLLHUP", any ? "|" : "");
        any = 1;
    }
    if (revents & POLLWRNORM) {
        printf("%sPOLLWRNORM", any ? "|" : "");
        any = 1;
    }
    if (revents & POLLWRBAND) {
        printf("%sPOLLWRBAND", any ? "|" : "");
        any = 1;
    }
    if (!any)
        printf("none");
}

static void print_ret(const char *label, int ret)
{
    printf("ALSAPCMPOLL: %s ret=%d errno=%d\n", label, ret,
           saved_errno(ret));
}

static alsa_interval_t *hw_interval(alsa_pcm_hw_params_t *params, uint hw)
{
    return &params->intervals[hw - SNDRV_PCM_HW_PARAM_SAMPLE_BITS];
}

static void set_mask(alsa_pcm_hw_params_t *params, uint hw, uint value)
{
    memset(&params->masks[hw], 0, sizeof(params->masks[hw]));
    params->masks[hw].bits[value / 32] = 1U << (value % 32);
}

static void set_interval(alsa_pcm_hw_params_t *params, uint hw, uint value)
{
    alsa_interval_t *ival = hw_interval(params, hw);

    memset(ival, 0, sizeof(*ival));
    ival->min = value;
    ival->max = value;
    ival->integer = 1;
}

static uint interval_min(const alsa_pcm_hw_params_t *params, uint hw)
{
    return params->intervals[hw - SNDRV_PCM_HW_PARAM_SAMPLE_BITS].min;
}

static void fill_hw_params(alsa_pcm_hw_params_t *params)
{
    memset(params, 0, sizeof(*params));
    set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
             SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    set_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    set_interval(params, SNDRV_PCM_HW_PARAM_FRAME_BITS, 32);
    set_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS, CHANNELS);
    set_interval(params, SNDRV_PCM_HW_PARAM_RATE, RATE_HZ);
    set_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD_FRAMES);
    set_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
                 PERIOD_FRAMES * FRAME_BYTES);
    set_interval(params, SNDRV_PCM_HW_PARAM_PERIODS,
                 BUFFER_FRAMES / PERIOD_FRAMES);
    set_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, BUFFER_FRAMES);
    set_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
                 BUFFER_FRAMES * FRAME_BYTES);
    set_interval(params, SNDRV_PCM_HW_PARAM_TICK_TIME, 0);
}

static void print_hw_params(const char *label, const alsa_pcm_hw_params_t *p)
{
    printf("ALSAPCMPOLL: %s rate=%u channels=%u format_mask=0x%x "
           "period_size=%u buffer_size=%u periods=%u period_bytes=%u "
           "buffer_bytes=%u\n",
           label,
           interval_min(p, SNDRV_PCM_HW_PARAM_RATE),
           interval_min(p, SNDRV_PCM_HW_PARAM_CHANNELS),
           p->masks[SNDRV_PCM_HW_PARAM_FORMAT].bits[0],
           interval_min(p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE),
           interval_min(p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE),
           interval_min(p, SNDRV_PCM_HW_PARAM_PERIODS),
           interval_min(p, SNDRV_PCM_HW_PARAM_PERIOD_BYTES),
           interval_min(p, SNDRV_PCM_HW_PARAM_BUFFER_BYTES));
}

static int pcm_status(int fd, const char *label, alsa_pcm_status_t *status)
{
    int ret;

    memset(status, 0, sizeof(*status));
    ret = ioctl(fd, SNDRV_PCM_IOCTL_STATUS, status);
    printf("ALSAPCMPOLL: STATUS %s ret=%d errno=%d state=%d(%s) "
           "delay=%ld appl_ptr=%lu hw_ptr=%lu avail=%lu avail_max=%lu\n",
           label, ret, saved_errno(ret), status->state,
           ret == 0 ? state_name(status->state) : "NA", status->delay,
           status->appl_ptr, status->hw_ptr, status->avail,
           status->avail_max);
    return ret;
}

static int pcm_delay(int fd, const char *label, int64 *delay)
{
    int ret;

    *delay = 0;
    ret = ioctl(fd, SNDRV_PCM_IOCTL_DELAY, delay);
    printf("ALSAPCMPOLL: DELAY %s ret=%d errno=%d delay=%ld\n",
           label, ret, saved_errno(ret), *delay);
    return ret;
}

static int pcm_poll(int fd, const char *label)
{
    struct pollfd pfd;
    int ret;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLOUT | POLLERR | POLLHUP;
    ret = poll_raw(&pfd, 1, 0);
    printf("ALSAPCMPOLL: POLL %s ret=%d errno=%d events=%d revents=%d names=",
           label, ret, raw_errno(ret), pfd.events, pfd.revents);
    print_revent_names(pfd.revents);
    printf("\n");
    return ret;
}

static void snapshot(int fd, const char *label, alsa_pcm_status_t *status)
{
    int64 delay;

    (void)pcm_status(fd, label, status);
    (void)pcm_delay(fd, label, &delay);
    (void)pcm_poll(fd, label);
}

static void fill_audio(unsigned char *buf, uint frames)
{
    uint samples = frames * CHANNELS;

    for (uint i = 0; i < samples; i++) {
        int sample = (int)((i * 97U) & 0x3ffU) - 0x200;
        buf[i * 2] = (unsigned char)(sample & 0xff);
        buf[i * 2 + 1] = (unsigned char)((sample >> 8) & 0xff);
    }
}

static int write_frames(int fd, const char *label, unsigned char *buf,
                        uint frames)
{
    int bytes = (int)(frames * FRAME_BYTES);
    int ret = write(fd, buf, bytes);

    printf("ALSAPCMPOLL: WRITE %s ret=%d errno=%d bytes=%d frames=%u\n",
           label, ret, saved_errno(ret), bytes, frames);
    return ret;
}

static void check_row(const char *name, int pass, const char *detail,
                      int state, uint64 appl_ptr, uint64 hw_ptr)
{
    printf("ALSAPCMPOLL: CHECK %s result=%s %s state=%d(%s) "
           "appl_ptr=%lu hw_ptr=%lu\n",
           name, pass ? "PASS" : "FAIL", detail, state, state_name(state),
           appl_ptr, hw_ptr);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    alsa_pcm_hw_params_t hw;
    alsa_pcm_hw_params_t refine;
    alsa_pcm_sw_params_t sw;
    alsa_pcm_status_t status;
    unsigned char audio[PERIOD_FRAMES * FRAME_BYTES];
    uint period_frames;
    uint buffer_frames;
    int version = 0;
    int fd;
    int ret;

    fill_audio(audio, PERIOD_FRAMES);

    fd = open("/dev/snd/pcmC0D0p", O_WRONLY | O_NONBLOCK);
    printf("ALSAPCMPOLL: OPEN path=/dev/snd/pcmC0D0p flags=O_WRONLY|O_NONBLOCK "
           "ret=%d errno=%d\n", fd, saved_errno(fd));
    if (fd < 0)
        exit(1);

    ret = ioctl(fd, SNDRV_PCM_IOCTL_PVERSION, &version);
    printf("ALSAPCMPOLL: PVERSION ret=%d errno=%d version=0x%x\n",
           ret, saved_errno(ret), version);

    fill_hw_params(&refine);
    ret = ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &refine);
    print_ret("HW_REFINE", ret);
    print_hw_params("HW_REFINE_PARAMS", &refine);

    fill_hw_params(&hw);
    ret = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
    print_ret("HW_PARAMS", ret);
    print_hw_params("HW_PARAMS_ACTUAL", &hw);
    if (ret < 0) {
        close(fd);
        exit(1);
    }

    period_frames = interval_min(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    buffer_frames = interval_min(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    if (period_frames == 0 || period_frames > PERIOD_FRAMES)
        period_frames = PERIOD_FRAMES;
    if (buffer_frames == 0)
        buffer_frames = BUFFER_FRAMES;

    memset(&sw, 0, sizeof(sw));
    sw.period_step = 1;
    sw.avail_min = period_frames;
    sw.xfer_align = 1;
    sw.start_threshold = buffer_frames;
    sw.stop_threshold = ALSA_BOUNDARY;
    sw.boundary = ALSA_BOUNDARY;
    ret = ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw);
    printf("ALSAPCMPOLL: SW_PARAMS ret=%d errno=%d avail_min=%lu "
           "start_threshold=%lu stop_threshold=%lu boundary=%lu\n",
           ret, saved_errno(ret), sw.avail_min, sw.start_threshold,
           sw.stop_threshold, sw.boundary);
    if (ret < 0) {
        close(fd);
        exit(1);
    }

    ret = ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0);
    print_ret("PREPARE initial", ret);
    if (ret < 0) {
        close(fd);
        exit(1);
    }

    (void)write_frames(fd, "initial_less_than_start_threshold", audio,
                       period_frames);
    snapshot(fd, "after_initial_write", &status);
    check_row("start_threshold_holds_before_threshold",
              status.state != SNDRV_PCM_STATE_RUNNING,
              "expected_state_not_RUNNING_before_threshold",
              status.state, status.appl_ptr, status.hw_ptr);

    ret = ioctl(fd, SNDRV_PCM_IOCTL_DROP, 0);
    print_ret("DROP", ret);
    snapshot(fd, "after_drop", &status);
    ret = write_frames(fd, "after_drop_without_prepare", audio, period_frames);
    check_row("write_after_drop_requires_prepare", ret < 0,
              "expected_negative_write_after_DROP",
              status.state, status.appl_ptr, status.hw_ptr);
    snapshot(fd, "after_drop_write", &status);

    ret = ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0);
    print_ret("PREPARE before_drain", ret);
    if (ret < 0) {
        close(fd);
        exit(1);
    }
    (void)write_frames(fd, "before_drain_less_than_start_threshold", audio,
                       period_frames);
    snapshot(fd, "before_drain", &status);
    ret = ioctl(fd, SNDRV_PCM_IOCTL_DRAIN, 0);
    print_ret("DRAIN", ret);
    snapshot(fd, "after_drain", &status);
    ret = write_frames(fd, "after_drain_without_prepare", audio,
                       period_frames);
    check_row("write_after_drain_requires_prepare", ret < 0,
              "expected_negative_write_after_DRAIN",
              status.state, status.appl_ptr, status.hw_ptr);
    snapshot(fd, "after_drain_write", &status);

    close(fd);
    printf("ALSAPCMPOLL: DONE ret=0 errno=0\n");
    exit(0);
}
