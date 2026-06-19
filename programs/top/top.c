/**
 * @file top.c
 * @brief System monitor — per-process CPU%, disk/network throughput.
 *
 * Usage: top [-a] [-n N] [-d SECS]
 *   -a        show all processes (including kernel threads)
 *   -n N      refresh N times then exit (default: 1)
 *   -d SECS   delay between refreshes in seconds (default: 2)
 *
 * On the first invocation (or with -n 1), absolute cumulative values are
 * shown.  With -n >1 the display switches to per-interval deltas/rates
 * starting from the second iteration.
 */
#include "kernel/inc/types.h"
#include "user/user.h"

/* ------------------------------------------------------------------ */
/*  linux_dirent64 — needed for getdents()                            */
/* ------------------------------------------------------------------ */
struct linux_dirent64 {
	uint64 d_ino;
	int64  d_off;
	uint16 d_reclen;
	uint8  d_type;
	char   d_name[];
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static int strncmp_local(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++) {
		if (a[i] != b[i])
			return (unsigned char)a[i] - (unsigned char)b[i];
		if (a[i] == '\0')
			return 0;
	}
	return 0;
}

static int is_numeric(const char *s)
{
	if (*s == '\0')
		return 0;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}

static int parse_u64_field(const char *line, const char *key, uint64 *val)
{
	int klen = strlen(key);
	if (strncmp_local(line, key, klen) != 0)
		return 0;
	const char *p = line + klen;
	while (*p == ':' || *p == '\t' || *p == ' ')
		p++;
	uint64 v = 0;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0');
	*val = v;
	return 1;
}

/* ------------------------------------------------------------------ */
/*  Per-process snapshot                                              */
/* ------------------------------------------------------------------ */

#define MAX_PROCS 128
#define MAX_CPUS 64
#define USER_HZ 100

struct proc_snap {
	int    pid;
	int    ppid;
	int    uid;
	char   name[17];
	char   state[8];
	uint64 vm_kb;
	uint64 cputime_ticks;    /* USER_HZ ticks from /proc/<pid>/stat */
	/* from /proc/<pid>/resources */
	uint64 fs_bytes_read;
	uint64 fs_bytes_written;
	uint64 net_bytes_sent;
	uint64 net_bytes_recv;
	uint64 bio_reads;
	uint64 bio_writes;
};

struct cpu_snap {
	uint64 busy_ticks;
	uint64 total_ticks;
};

struct sys_snap {
	uint64 uptime_ms;
	uint64 load_avg_5s_x100;
	uint64 cpu_busy;
	uint64 cpu_total;
	int ncpus;
	struct cpu_snap cpu[MAX_CPUS];
	uint64 fs_bytes_read;
	uint64 fs_bytes_written;
	uint64 net_bytes_sent;
	uint64 net_bytes_recv;
	uint64 bio_reads;
	uint64 bio_writes;
};

static struct proc_snap snap[2][MAX_PROCS]; /* two snapshot buffers */
static int snap_count[2];                   /* number of procs in each */

static int read_file_into(const char *path, char *buf, int bufsz)
{
	int fd = open(path, 0);
	if (fd < 0)
		return -1;
	int total = 0, n;
	while (total < bufsz - 1 &&
	       (n = read(fd, buf + total, bufsz - 1 - total)) > 0)
		total += n;
	buf[total] = '\0';
	close(fd);
	return total;
}

static int parse_status(int pid, struct proc_snap *ps)
{
	char path[64], buf[512];
	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	if (read_file_into(path, buf, sizeof(buf)) <= 0)
		return -1;

	ps->pid = pid;
	ps->ppid = 0;
	ps->uid = -1;
	ps->name[0] = '\0';
	ps->state[0] = '\0';
	ps->vm_kb = 0;
	ps->cputime_ticks = 0;

	char *p = buf;
	while (*p) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		uint64 tmp;
		if (strncmp_local(p, "Name:\t", 6) == 0) {
			char *src = p + 6;
			int i = 0;
			while (*src && i < 16)
				ps->name[i++] = *src++;
			ps->name[i] = '\0';
		} else if (strncmp_local(p, "State:\t", 7) == 0) {
			char *src = p + 7;
			int i = 0;
			while (*src && i < 7)
				ps->state[i++] = *src++;
			ps->state[i] = '\0';
		} else if (parse_u64_field(p, "Pid", &tmp)) {
			ps->pid = (int)tmp;
		} else if (parse_u64_field(p, "PPid", &tmp)) {
			ps->ppid = (int)tmp;
		} else if (parse_u64_field(p, "Uid", &tmp)) {
			ps->uid = (int)tmp;
		} else if (parse_u64_field(p, "VmSize", &tmp)) {
			ps->vm_kb = tmp;
		}
		if (nl)
			p = nl + 1;
		else
			break;
	}
	return 0;
}

static uint64 parse_u64_token(const char *s)
{
	uint64 v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return v;
}

static void parse_pid_stat(int pid, struct proc_snap *ps)
{
	char path[64], buf[512];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (read_file_into(path, buf, sizeof(buf)) <= 0)
		return;

	char *rp = 0;
	for (char *q = buf; *q; q++)
		if (*q == ')')
			rp = q;
	if (rp == 0)
		return;
	char *p = rp + 1;
	while (*p == ' ')
		p++;

	uint64 utime = 0;
	uint64 stime = 0;
	int field = 3; /* first token after "(comm)" is state */
	while (*p && field <= 15) {
		while (*p == ' ')
			p++;
		char *start = p;
		while (*p && *p != ' ')
			p++;
		if (field == 14)
			utime = parse_u64_token(start);
		else if (field == 15)
			stime = parse_u64_token(start);
		field++;
	}
	ps->cputime_ticks = utime + stime;
}

static void parse_resources(int pid, struct proc_snap *ps)
{
	char path[64], buf[2048];
	snprintf(path, sizeof(path), "/proc/%d/resources", pid);
	if (read_file_into(path, buf, sizeof(buf)) <= 0)
		return;

	char *p = buf;
	while (*p) {
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		parse_u64_field(p, "fs_bytes_read", &ps->fs_bytes_read);
		parse_u64_field(p, "fs_bytes_written", &ps->fs_bytes_written);
		parse_u64_field(p, "net_bytes_sent", &ps->net_bytes_sent);
		parse_u64_field(p, "net_bytes_recv", &ps->net_bytes_recv);
		parse_u64_field(p, "bio_reads", &ps->bio_reads);
		parse_u64_field(p, "bio_writes", &ps->bio_writes);
		if (nl)
			p = nl + 1;
		else
			break;
	}
}

static void scan_procs(int slot)
{
	int count = 0;
	int fd = open("/proc", 0);
	if (fd < 0) {
		printf("top: cannot open /proc\n");
		snap_count[slot] = 0;
		return;
	}
	char dirent_buf[1024];
	int nread;
	while ((nread = getdents(fd, dirent_buf, sizeof(dirent_buf))) > 0) {
		int pos = 0;
		while (pos < nread) {
			struct linux_dirent64 *de =
			    (struct linux_dirent64 *)(dirent_buf + pos);
			if (de->d_ino != 0 && is_numeric(de->d_name)) {
				int pid = atoi(de->d_name);
				if (count < MAX_PROCS) {
					struct proc_snap *ps = &snap[slot][count];
					memset(ps, 0, sizeof(*ps));
					if (parse_status(pid, ps) == 0) {
						parse_pid_stat(pid, ps);
						parse_resources(pid, ps);
						count++;
					}
				}
			}
			pos += de->d_reclen;
		}
	}
	close(fd);
	snap_count[slot] = count;
}

static void fill_io_totals(int slot, struct sys_snap *ss)
{
	ss->fs_bytes_read = 0;
	ss->fs_bytes_written = 0;
	ss->net_bytes_sent = 0;
	ss->net_bytes_recv = 0;
	ss->bio_reads = 0;
	ss->bio_writes = 0;

	for (int i = 0; i < snap_count[slot]; i++) {
		ss->fs_bytes_read += snap[slot][i].fs_bytes_read;
		ss->fs_bytes_written += snap[slot][i].fs_bytes_written;
		ss->net_bytes_sent += snap[slot][i].net_bytes_sent;
		ss->net_bytes_recv += snap[slot][i].net_bytes_recv;
		ss->bio_reads += snap[slot][i].bio_reads;
		ss->bio_writes += snap[slot][i].bio_writes;
	}
}

/* Find a process by PID in the given snapshot slot. Returns NULL if gone. */
static struct proc_snap *find_by_pid(int slot, int pid)
{
	for (int i = 0; i < snap_count[slot]; i++)
		if (snap[slot][i].pid == pid)
			return &snap[slot][i];
	return 0;
}

/* ------------------------------------------------------------------ */
/*  UID → username cache (loaded once from /etc/passwd)               */
/* ------------------------------------------------------------------ */

#define MAX_USERS 64
#define MAX_UNAME 16

static struct {
	int  uid;
	char name[MAX_UNAME];
} uid_cache[MAX_USERS];
static int uid_cache_count;

static void load_passwd(void)
{
	uid_cache_count = 0;
	char buf[2048];
	int fd = open("/etc/passwd", 0);
	if (fd < 0)
		return;
	int total = 0, n;
	while (total < (int)sizeof(buf) - 1 &&
	       (n = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
		total += n;
	close(fd);
	buf[total] = '\0';

	/* Parse lines:  name:x:uid:gid:... */
	char *p = buf;
	while (*p && uid_cache_count < MAX_USERS) {
		char *c1 = 0;
		for (char *q = p; *q && *q != '\n'; q++)
			if (*q == ':' && !c1) { c1 = q; break; }
		if (!c1) { while (*p && *p != '\n') p++; if (*p) p++; continue; }
		char *c2 = 0;
		for (char *q = c1 + 1; *q && *q != '\n'; q++)
			if (*q == ':') { c2 = q; break; }
		if (!c2) { while (*p && *p != '\n') p++; if (*p) p++; continue; }
		int u = 0;
		for (char *q = c2 + 1; *q >= '0' && *q <= '9'; q++)
			u = u * 10 + (*q - '0');
		int nlen = c1 - p;
		if (nlen >= MAX_UNAME) nlen = MAX_UNAME - 1;
		memcpy(uid_cache[uid_cache_count].name, p, nlen);
		uid_cache[uid_cache_count].name[nlen] = '\0';
		uid_cache[uid_cache_count].uid = u;
		uid_cache_count++;
		while (*p && *p != '\n') p++;
		if (*p) p++;
	}
}

static const char *uid_to_name(int uid)
{
	for (int i = 0; i < uid_cache_count; i++)
		if (uid_cache[i].uid == uid)
			return uid_cache[i].name;
	return 0;
}

/* ------------------------------------------------------------------ */
/*  System snapshot from Linux-shaped procfs files                    */
/* ------------------------------------------------------------------ */

static uint64 parse_decimal_x100(const char *s)
{
	uint64 whole = 0;
	uint64 frac = 0;
	int frac_digits = 0;

	while (*s >= '0' && *s <= '9')
		whole = whole * 10 + (*s++ - '0');
	if (*s == '.') {
		s++;
		while (*s >= '0' && *s <= '9' && frac_digits < 2) {
			frac = frac * 10 + (*s++ - '0');
			frac_digits++;
		}
	}
	while (frac_digits++ < 2)
		frac *= 10;
	return whole * 100 + frac;
}

static void parse_cpu_line(const char *line, uint64 *busy, uint64 *total)
{
	uint64 vals[10];
	int n = 0;

	while (*line && *line != ' ')
		line++;
	while (*line == ' ')
		line++;
	while (*line && n < (int)(sizeof(vals) / sizeof(vals[0]))) {
		vals[n++] = parse_u64_token(line);
		while (*line && *line != ' ')
			line++;
		while (*line == ' ')
			line++;
	}

	uint64 sum = 0;
	for (int i = 0; i < n; i++)
		sum += vals[i];
	uint64 idle = 0;
	if (n > 3)
		idle += vals[3];
	if (n > 4)
		idle += vals[4];
	*total = sum;
	*busy = sum > idle ? sum - idle : 0;
}

static void read_proc_uptime(struct sys_snap *ss)
{
	char buf[128];
	if (read_file_into("/proc/uptime", buf, sizeof(buf)) <= 0)
		return;
	ss->uptime_ms = parse_decimal_x100(buf) * 10;
}

static void read_proc_loadavg(struct sys_snap *ss)
{
	char buf[128];
	if (read_file_into("/proc/loadavg", buf, sizeof(buf)) <= 0)
		return;

	char *p = buf;
	while (*p && *p != ' ')
		p++;
	while (*p == ' ')
		p++;
	ss->load_avg_5s_x100 = parse_decimal_x100(p);
}

static void read_proc_stat(struct sys_snap *ss)
{
	char buf[2048];
	if (read_file_into("/proc/stat", buf, sizeof(buf)) <= 0)
		return;

	char *p = buf;
	while (*p) {
		char *line = p;
		char *nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';

		if (strncmp_local(line, "cpu ", 4) == 0) {
			parse_cpu_line(line, &ss->cpu_busy, &ss->cpu_total);
		} else if (strncmp_local(line, "cpu", 3) == 0 &&
		           line[3] >= '0' && line[3] <= '9' &&
		           ss->ncpus < MAX_CPUS) {
			parse_cpu_line(line, &ss->cpu[ss->ncpus].busy_ticks,
			               &ss->cpu[ss->ncpus].total_ticks);
			ss->ncpus++;
		}

		if (!nl)
			break;
		p = nl + 1;
	}
}

static void read_system_snapshot(struct sys_snap *ss)
{
	memset(ss, 0, sizeof(*ss));
	read_proc_uptime(ss);
	read_proc_loadavg(ss);
	read_proc_stat(ss);
}

/* ------------------------------------------------------------------ */
/*  Formatting helpers                                                */
/* ------------------------------------------------------------------ */

static void fmt_bytes(char *buf, int bufsz, uint64 bytes)
{
	if (bytes < 1024)
		snprintf(buf, bufsz, "%d B", (int)bytes);
	else if (bytes < 1024 * 1024)
		snprintf(buf, bufsz, "%d.%d KB", (int)(bytes / 1024),
		         (int)((bytes % 1024) * 10 / 1024));
	else if (bytes < (uint64)1024 * 1024 * 1024)
		snprintf(buf, bufsz, "%d.%d MB",
		         (int)(bytes / (1024 * 1024)),
		         (int)((bytes % (1024 * 1024)) * 10 / (1024 * 1024)));
	else
		snprintf(buf, bufsz, "%d.%d GB",
		         (int)(bytes / (1024ULL * 1024 * 1024)),
		         (int)((bytes % (1024ULL * 1024 * 1024)) * 10 /
		               (1024ULL * 1024 * 1024)));
}

static void fmt_rate(char *buf, int bufsz, uint64 delta_bytes, int secs)
{
	if (secs <= 0)
		secs = 1;
	uint64 rate = delta_bytes / (uint64)secs;
	if (rate < 1024)
		snprintf(buf, bufsz, "%d B/s", (int)rate);
	else if (rate < 1024 * 1024)
		snprintf(buf, bufsz, "%d.%d KB/s", (int)(rate / 1024),
		         (int)((rate % 1024) * 10 / 1024));
	else
		snprintf(buf, bufsz, "%d.%d MB/s",
		         (int)(rate / (1024 * 1024)),
		         (int)((rate % (1024 * 1024)) * 10 / (1024 * 1024)));
}

/* ------------------------------------------------------------------ */
/*  Display                                                           */
/* ------------------------------------------------------------------ */

static void print_header(struct sys_snap *cur, struct sys_snap *prev,
                         int interval_secs)
{
	uint64 up_s = cur->uptime_ms / 1000;
	int load_int  = (int)(cur->load_avg_5s_x100 / 100);
	int load_frac = (int)(cur->load_avg_5s_x100 % 100);

	printf("Uptime: %d:%02d:%02d   CPUs: %d   Load(5s): %d.%02d\n",
	       (int)(up_s / 3600), (int)((up_s / 60) % 60),
	       (int)(up_s % 60), cur->ncpus, load_int, load_frac);

	printf("CPU  ");
	for (int i = 0; i < cur->ncpus && i < MAX_CPUS; i++) {
		uint64 busy = cur->cpu[i].busy_ticks;
		uint64 total = cur->cpu[i].total_ticks;
		if (prev && i < prev->ncpus) {
			busy -= prev->cpu[i].busy_ticks;
			total -= prev->cpu[i].total_ticks;
		}
		int util_pct = total > 0 ? (int)(busy * 100 / total) : 0;
		if (util_pct > 100)
			util_pct = 100;
		printf(" [%d:u=%d%%]", i, util_pct);
	}
	printf("\n");

	/* Disk */
	char rbuf[32], wbuf[32];
	if (prev) {
		fmt_rate(rbuf, sizeof(rbuf),
		         cur->fs_bytes_read - prev->fs_bytes_read,
		         interval_secs);
		fmt_rate(wbuf, sizeof(wbuf),
		         cur->fs_bytes_written - prev->fs_bytes_written,
		         interval_secs);
	} else {
		fmt_bytes(rbuf, sizeof(rbuf), cur->fs_bytes_read);
		fmt_bytes(wbuf, sizeof(wbuf), cur->fs_bytes_written);
	}
	printf("Disk   rd=%d wr=%d  %s / %s\n",
	       (int)cur->bio_reads, (int)cur->bio_writes, rbuf, wbuf);

	/* Net */
	char txb[32], rxb[32];
	if (prev) {
		fmt_rate(txb, sizeof(txb),
		         cur->net_bytes_sent - prev->net_bytes_sent,
		         interval_secs);
		fmt_rate(rxb, sizeof(rxb),
		         cur->net_bytes_recv - prev->net_bytes_recv,
		         interval_secs);
	} else {
		fmt_bytes(txb, sizeof(txb), cur->net_bytes_sent);
		fmt_bytes(rxb, sizeof(rxb), cur->net_bytes_recv);
	}
	printf("Net    tx/rx  %s / %s\n", txb, rxb);
}

/**
 * Print the process table.
 *
 * @param cur_slot  index into snap[] for the current snapshot
 * @param prev_slot index into snap[] for the prev snapshot (-1 if none)
 * @param show_all  include kernel threads (VmSize==0)
 * @param interval_ms  milliseconds between snapshots (for rate calc)
 */
static void print_procs(int cur_slot, int prev_slot, int show_all,
                        uint64 interval_ms, uint64 interval_ticks)
{
	int n = snap_count[cur_slot];
	int user_count = 0, kern_count = 0;
	for (int i = 0; i < n; i++) {
		if (snap[cur_slot][i].vm_kb > 0)
			user_count++;
		else
			kern_count++;
	}
	printf("Procs: %d user, %d kernel, %d total\n\n",
	       user_count, kern_count, n);

	int have_prev = (prev_slot >= 0);

	/* Header line */
	if (have_prev)
		printf("%-6s %-8s %-16s %-5s %5s %8s %6s %9s %9s %9s %9s\n",
		       "PID", "USER", "NAME", "STATE", "VM",
		       "CPU(ms)", "CPU%", "FS_RD/s", "FS_WR/s", "NTX/s", "NRX/s");
	else
		printf("%-6s %-8s %-16s %-5s %5s %8s %6s %9s %9s %9s %9s\n",
		       "PID", "USER", "NAME", "STATE", "VM",
		       "CPU(ms)", "CPU%", "FS_RD", "FS_WR", "NET_TX", "NET_RX");

	for (int i = 0; i < n; i++) {
		struct proc_snap *cur = &snap[cur_slot][i];

		if (!show_all && cur->vm_kb == 0)
			continue;

		/* CPU% = delta(cputime_ticks) / interval_ticks * 100.
		 * Both numerator and denominator use Linux USER_HZ ticks.
		 * pct_x10 = tenths of percent (1000 = 100.0%). */
		int cpu_pct_x10 = 0;
		if (have_prev && interval_ticks > 0) {
			struct proc_snap *old = find_by_pid(prev_slot,
			                                    cur->pid);
			if (old) {
				uint64 dt_ticks = cur->cputime_ticks -
				                  old->cputime_ticks;
				cpu_pct_x10 = (int)(dt_ticks * 1000 /
				              interval_ticks);
			}
		}

		/* Convert raw ticks to ms for display */
		uint64 cputime_ms = cur->cputime_ticks * 1000 / USER_HZ;

		/* Resolve username */
		const char *uname = uid_to_name(cur->uid);
		char uid_str[12];
		if (!uname) {
			snprintf(uid_str, sizeof(uid_str), "%d", cur->uid);
			uname = uid_str;
		}

		if (have_prev) {
			/* Delta mode — compute I/O rates */
			struct proc_snap *old = find_by_pid(prev_slot,
			                                    cur->pid);
			uint64 dt_frd = 0, dt_fwr = 0;
			uint64 dt_ntx = 0, dt_nrx = 0;
			if (old) {
				dt_frd = cur->fs_bytes_read -
				         old->fs_bytes_read;
				dt_fwr = cur->fs_bytes_written -
				         old->fs_bytes_written;
				dt_ntx = cur->net_bytes_sent -
				         old->net_bytes_sent;
				dt_nrx = cur->net_bytes_recv -
				         old->net_bytes_recv;
			} else {
				dt_frd = cur->fs_bytes_read;
				dt_fwr = cur->fs_bytes_written;
				dt_ntx = cur->net_bytes_sent;
				dt_nrx = cur->net_bytes_recv;
			}

			int interval_secs = (int)(interval_ms / 1000);
			if (interval_secs < 1)
				interval_secs = 1;

			char frd[16], fwr[16], ntx[16], nrx[16];
			fmt_rate(frd, sizeof(frd), dt_frd, interval_secs);
			fmt_rate(fwr, sizeof(fwr), dt_fwr, interval_secs);
			fmt_rate(ntx, sizeof(ntx), dt_ntx, interval_secs);
			fmt_rate(nrx, sizeof(nrx), dt_nrx, interval_secs);

			printf("%-6d %-8s %-16s %-5s %5d %8d %3d.%d %9s %9s %9s %9s\n",
			       cur->pid, uname, cur->name, cur->state,
			       (int)cur->vm_kb, (int)cputime_ms,
			       cpu_pct_x10 / 10, cpu_pct_x10 % 10,
			       frd, fwr, ntx, nrx);
		} else {
			/* Absolute mode — show cumulative I/O values */
			char frd[16], fwr[16], ntx[16], nrx[16];
			fmt_bytes(frd, sizeof(frd), cur->fs_bytes_read);
			fmt_bytes(fwr, sizeof(fwr), cur->fs_bytes_written);
			fmt_bytes(ntx, sizeof(ntx), cur->net_bytes_sent);
			fmt_bytes(nrx, sizeof(nrx), cur->net_bytes_recv);

			printf("%-6d %-8s %-16s %-5s %5d %8d %3d.%d %9s %9s %9s %9s\n",
			       cur->pid, uname, cur->name, cur->state,
			       (int)cur->vm_kb, (int)cputime_ms,
			       cpu_pct_x10 / 10, cpu_pct_x10 % 10,
			       frd, fwr, ntx, nrx);
		}
	}
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf("Usage: top [-a] [-n N] [-d SECS]\n");
	printf("  -a        show all (including kernel threads)\n");
	printf("  -n N      refresh N times then exit (default: 1)\n");
	printf("  -d SECS   delay between refreshes (default: 2)\n");
}

int main(int argc, char *argv[])
{
	int show_all = 0;
	int iterations = 1;
	int delay_secs = 2;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-a") == 0) {
			show_all = 1;
		} else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			iterations = atoi(argv[++i]);
			if (iterations < 1)
				iterations = 1;
		} else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			delay_secs = atoi(argv[++i]);
			if (delay_secs < 1)
				delay_secs = 1;
		} else if (strcmp(argv[i], "-h") == 0) {
			usage();
			return 0;
		} else {
			printf("top: unknown option '%s'\n", argv[i]);
			usage();
			return 1;
		}
	}

	struct sys_snap ks[2]; /* two system snapshot buffers for delta */
	int cur = 0;         /* current slot index (alternates 0/1) */

	load_passwd();

	for (int iter = 0; iter < iterations; iter++) {
		int prev = -1;

		if (iter == 0) {
			/* First iteration: take two snapshots with a 1s
			 * sleep to compute meaningful CPU% deltas
			 * (Linux approach). */
			read_system_snapshot(&ks[0]);
			scan_procs(0);
			fill_io_totals(0, &ks[0]);
			sleep(1000); /* 1 second baseline */
			read_system_snapshot(&ks[1]);
			scan_procs(1);
			fill_io_totals(1, &ks[1]);
			prev = 0;
			cur = 1;
		} else {
			/* Subsequent: we already have the previous in cur */
			prev = cur;
			cur = 1 - cur; /* flip to other slot */
			read_system_snapshot(&ks[cur]);
			scan_procs(cur);
			fill_io_totals(cur, &ks[cur]);
		}

		if (iter > 0)
			printf("\n--- refresh %d/%d ---\n\n", iter + 1,
			       iterations);

		uint64 interval_ms = 0;
		uint64 interval_ticks = 0;
		if (prev >= 0) {
			interval_ms = ks[cur].uptime_ms - ks[prev].uptime_ms;
			interval_ticks = ks[cur].cpu_total - ks[prev].cpu_total;
		} else {
			interval_ms = ks[cur].uptime_ms;
			interval_ticks = ks[cur].cpu_total;
		}

		print_header(&ks[cur],
		             (prev >= 0) ? &ks[prev] : 0,
		             (int)(interval_ms / 1000));
		printf("\n");
		print_procs(cur, prev, show_all, interval_ms, interval_ticks);

		if (iter + 1 < iterations)
			sleep(delay_secs * 1000); /* sleep(ms) */
	}

	return 0;
}
