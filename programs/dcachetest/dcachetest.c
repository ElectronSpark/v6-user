/*
 * dcachetest — VFS dcache mount/unmount/remount correctness reducer (U6).
 *
 * Targets the dcache eviction/no-flush gaps that a dangling superblock pointer
 * would expose through the POSITIVE dcache (which is honored by default, no
 * gate).  The core sequence is:
 *
 *   mount tmpfs /mnt (sb A) -> create /mnt/f -> stat /mnt/f  (caches a POSITIVE
 *   entry parent_sb=A, name="f") -> umount /mnt (frees A's slab) -> mount tmpfs
 *   /mnt (sb B, typically re-handed A's freed address, fresh & empty) ->
 *   stat /mnt/f
 *
 * POSIX-correct result of the final stat is failure: /mnt/f does not exist in
 * the fresh mount.  PRE-FIX, the stale positive entry (keyed on the reused sb
 * address + a colliding lookup_seq) is honored and stat wrongly succeeds, or
 * the child_sb->valid deref reads freed/reused slab (UAF -> panic).  POST-FIX
 * (dcache flushed on sb free + globally-unique generation tokens) the stat
 * fails cleanly.
 *
 * Userspace-only, deterministic; no gate required.  Prints one
 * "RESULT=PASS/FAIL test=<name>" line per subtest and exits 0 iff all pass.
 *
 * xv6 userspace has no errno: syscalls return <0 on failure, so we assert on
 * the sign of the return value.  A UAF surfaces as a kernel panic caught by the
 * boot harness, not as a return code here.
 */
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define MNT "/mnt"

static int failures = 0;

static void report(const char *name, int pass) {
    printf("RESULT=%s test=%s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) {
        failures++;
    }
}

/* mount a fresh empty tmpfs on /mnt; returns 0 on success, <0 on failure. */
static int mnt_tmpfs(void) {
    return mount("none", MNT, "tmpfs", 0, 0);
}

/* create an (empty) file at path via O_CREAT; returns 0 on success. */
static int touch(const char *path) {
    int fd = open(path, O_CREAT | O_RDWR);
    if (fd < 0) {
        return -1;
    }
    close(fd);
    return 0;
}

/* ---- Subtest 1: basic remount, single name -------------------------------- */
static void test_remount_basic(void) {
    struct stat st;
    int ok = 1;

    if (mnt_tmpfs() < 0) {
        report("remount_basic_mountA", 0);
        return;
    }
    if (touch(MNT "/f") < 0) {
        ok = 0;
    }
    /* Positive lookup: must succeed and cache a positive dcache entry. */
    if (stat(MNT "/f", &st) < 0) {
        ok = 0;
    }
    if (umount(MNT) < 0) {
        report("remount_basic_umountA", 0);
        return;
    }
    if (mnt_tmpfs() < 0) {
        report("remount_basic_mountB", 0);
        return;
    }
    /* The crux: fresh empty tmpfs B, so /mnt/f MUST NOT exist. A stale positive
     * dcache hit against freed sb A would make this wrongly succeed. */
    if (stat(MNT "/f", &st) >= 0) {
        ok = 0; /* stale positive honored — bug present */
    }
    (void)umount(MNT);
    report("remount_basic", ok);
}

/* ---- Subtest 2: 50x distinct-name loop, re-stat prev name after remount ---- */
static void test_remount_loop(void) {
    char cur[32];
    char prev[32];
    struct stat st;
    int ok = 1;
    int have_prev = 0;

    for (int i = 0; i < 50; i++) {
        snprintf(cur, sizeof(cur), MNT "/f%d", i);

        if (mnt_tmpfs() < 0) {
            ok = 0;
            break;
        }
        if (touch(cur) < 0) {
            ok = 0;
        }
        /* fresh positive against this incarnation must succeed */
        if (stat(cur, &st) < 0) {
            ok = 0;
        }
        if (umount(MNT) < 0) {
            ok = 0;
            break;
        }
        if (mnt_tmpfs() < 0) {
            ok = 0;
            break;
        }
        /* fresh empty B: current name must be absent */
        if (stat(cur, &st) >= 0) {
            ok = 0;
        }
        /* and the previous iteration's name (its sb long freed) must be absent
         * too — exercises repeated slab reuse and cross-incarnation aliasing */
        if (have_prev && stat(prev, &st) >= 0) {
            ok = 0;
        }
        if (umount(MNT) < 0) {
            ok = 0;
            break;
        }
        strcpy(prev, cur);
        have_prev = 1;
    }
    report("remount_loop_50x", ok);
}

/* ---- Subtest 3: 200x mount/unmount/remount stability (UAF sentinel) -------- */
static void test_mount_churn(void) {
    int ok = 1;

    for (int i = 0; i < 200; i++) {
        if (mnt_tmpfs() < 0) {
            ok = 0;
            break;
        }
        /* touch a name each time to populate + churn dcache buckets */
        (void)touch(MNT "/a");
        if (umount(MNT) < 0) {
            ok = 0;
            break;
        }
    }
    /* Reaching here without a kernel panic is the pass signal. */
    report("mount_churn_200x", ok);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Ensure the mount point exists (ignore EEXIST). */
    (void)mkdir(MNT);

    printf("dcachetest: start\n");
    test_remount_basic();
    test_remount_loop();
    test_mount_churn();

    if (failures == 0) {
        printf("dcachetest: ALL PASS\n");
        exit(0);
    }
    printf("dcachetest: %d subtest(s) FAILED\n", failures);
    exit(1);
}
