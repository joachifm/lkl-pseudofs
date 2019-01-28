/* buildfs.c - construct rootfs on a filesystem image */

/* lkl stuff inspired by lkl/cptofs.c, written by Octavian Purdila and others.
 *
 * gen_init_cpio spec parsing initially lifted from usr/gen_init_cpio.c, by
 * Jeff Garzik and others.
 */

/* Copyright (C) 2019 Joachim F. <joachifm@fastmail.fm>
 * Distributed under the terms of the GNU General Public License, version 2, or
 * any later version.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <err.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <lkl.h>
#include <lkl_host.h>

#define PROGNAME "lkl-buildfs"

/* Size of buffer holding the fstype spec, including NUL */
#define FSTYPE_MAX 12

/* CPP stringification */
#define XSTR(s) #s
#define STR(s) XSTR(s)

/* Format spec for parsing file paths with scanf */
#define FMT_PATH "%" STR(PATH_MAX) "s"

/* Program globals */
static struct {
    int verbosity;

    char fstype[FSTYPE_MAX];
    char imgfile[PATH_MAX];
    int part; /* Partition, or 0 if the image is unpartitioned. */

    /* A descriptor to the internal fs image mount dir, used to access paths
     * within imgfile via lkl_sys_*_at calls.  This is to avoid having to
     * recover the "real" path by prepending the internal mount path whenever
     * we want to refer to a file within the image.
     *
     * Example:
     *    lkl_sys_mkdirat(prog.relfd, "foo/bar", 0555);
     * Note that the path must be relative for this to work as expected.
     */
    int relfd;
} prog;

static _Noreturn void usage(int);
static void options(int*, char**[]);

/* Spec handlers */

void do_file(char const*, char const*, const mode_t, const uid_t, const gid_t);
void do_slink(char const*, char const*, const uid_t, const gid_t);
void do_dir(char const*, const mode_t, const uid_t, const gid_t);
void do_special(char const*, const mode_t, const uid_t, const gid_t, const char,
            const int, const int);

#define do_pipe(name, mode, uid, gid) (do_special(name, mode, uid, gid, 'p', 0, 0))
#define do_sock(name, mode, uid, gid) (do_special(name, mode, uid, gid, 's', 0, 0))
#define do_bdev(name, mode, uid, gid, maj, min) \
    (do_special(name, mode, uid, gid, 'b', maj, min))
#define do_cdev(name, mode, uid, gid, maj, min) \
    (do_special(name, mode, uid, gid, 'c', maj, min))

/*
 * Misc.
 */

static bool streq(char const* restrict s1, char const* restrict s2) {
    return strcmp(s1, s2) == 0;
}

static size_t strlcpy_notrunc(char* restrict dst, char const* restrict src, size_t siz) {
    int ret = snprintf(dst, siz, "%s", src);
    if (ret >= siz)
        errx(EXIT_FAILURE, "strlcpy_notrunc: truncated output");
    if (ret < 0)
        err(EXIT_FAILURE, "snprintf");
    return ret;
}

static char const* asrelpath(char const* s) {
    return (*s == '/') ? ++s : s;
}

static int xsscanf(char const* fmt, size_t nparam, char const* args, ...) {
    va_list ap;
    va_start(ap, args);
    ssize_t res = vsscanf(args, fmt, ap);
    va_end(ap);
    return res - nparam; /* number of parsed components, 0 = success, >0 = too many, <0 too few */
}

/*
 * Main entry
 */

int main(int argc, char* argv[]) {
    options(&argc, &argv);

    int diskid = -1;
    struct lkl_disk disk = {0};

    disk.fd = openat(AT_FDCWD, prog.imgfile, O_RDWR);
    if (disk.fd < 0)
        err(EXIT_FAILURE, "failed to open %s r/w", prog.imgfile);

    diskid = lkl_disk_add(&disk);
    if (diskid < 0)
        errx(EXIT_FAILURE, "failed to add disk: %s", lkl_strerror(diskid));

    if (prog.verbosity < 3)
        lkl_host_ops.print = 0;

    lkl_start_kernel(&lkl_host_ops, "mem=128M");

    char mnt[PATH_MAX]; /* internal image mount path */
    int mounted = lkl_mount_dev(diskid, prog.part, prog.fstype, 0, 0,
            mnt, sizeof(mnt));
    if (mounted < 0)
        errx(EXIT_FAILURE, "failed to mount filesystem image: %s",
             lkl_strerror(mounted));

    prog.relfd = lkl_sys_open(mnt, LKL_O_PATH | LKL_O_DIRECTORY, 0);
    if (prog.relfd < 0)
        errx(EXIT_FAILURE, "failed to open dirfd: %s", lkl_strerror(prog.relfd));

    FILE* spec_list = stdin;
    char line[2 * PATH_MAX + 64];
    long lineno = 0;
    char* type; /* holds the type part of the spec */
    char* args; /* holds the remaining args part of the spec */
    while (fgets(line, sizeof(line), spec_list)) {
        size_t slen = strlen(line);
        ++lineno;

        if (*line == '#')  /* comment */
            continue;

        if (!(type = strtok(line, " \t")))
            errx(EXIT_FAILURE, "%ld: expected separator", lineno);

        if (*type == '\n' || slen == strlen(type)) /* blank line */
            continue;

        if (!(args = strtok(0, "\n"))) {
            fprintf(stderr, "%s: %ld: expected args\n", PROGNAME, lineno);
            continue;
        }

        int err = 0;
        char name[PATH_MAX];
        mode_t mode;
        uid_t uid;
        gid_t gid;

        if        (streq(type, "file")) {
            char infile[PATH_MAX];
            char const fmt[] = FMT_PATH " " FMT_PATH " %o %d %d";
            if (xsscanf(fmt, 5, args, name, infile, &mode, &uid, &gid) != 0)
                errx(EXIT_FAILURE, "line %ld: malformed file spec", lineno);
            do_file(asrelpath(name), infile, mode, uid, gid);
        } else if (streq(type, "dir")) {
            char const fmt[] = FMT_PATH " %o %d %d";
            if (xsscanf(fmt, 4, args, name, &mode, &uid, &gid) != 0)
                errx(EXIT_FAILURE, "line %ld: malformed dir spec", lineno);
            do_dir(asrelpath(name), mode, uid, gid);
        } else if (streq(type, "slink")) {
            char target[PATH_MAX];
            char const fmt[] = FMT_PATH " " FMT_PATH " %d %d";
            if (xsscanf(fmt, 4, args, name, target, &uid, &gid) != 0)
                errx(EXIT_FAILURE, "line %ld: malformed slink spec", lineno);
            do_slink(asrelpath(name), target, uid, gid);
        } else if (streq(type, "nod")) {
            char devtype;
            int maj;
            int min;
            char const fmt[] = FMT_PATH " %o %d %d %c %d %d";
            if (xsscanf(fmt, 7, args, name, &mode, &uid, &gid, &devtype, &maj, &min) != 0)
                errx(EXIT_FAILURE, "line %ld: malformed nod spec", lineno);
            do_special(asrelpath(name), mode, uid, gid, devtype, maj, min);
        } else if (streq(type, "pipe")) {
            char const fmt[] = FMT_PATH " %o %d %d";
            if (xsscanf(fmt, 4, args, name, &mode, &uid, &gid) != 0)
                errx(EXIT_FAILURE, "line %ld: malformed pipe spec", lineno);
            do_pipe(asrelpath(name), mode, uid, gid);
        } else if (streq(type, "sock")) {
            char const fmt[] = FMT_PATH " %o %d %d";
            if (xsscanf(fmt, 4, args, name, &mode, &uid, &gid) != 0)
                errx(EXIT_FAILURE, "line %ld: malformed sock spec", lineno);
            do_sock(asrelpath(name), mode, uid, gid);
        } else {
            errx(EXIT_FAILURE, "%s: unrecognized type: %s\n", PROGNAME, type);
        }
    }

    lkl_sys_close(prog.relfd);
    lkl_umount_dev(diskid, prog.part, 0, 1000);
    lkl_sys_halt();
    close(disk.fd);

    exit(0);
}

void usage(int eval) {
    fprintf(stderr, "Usage: %s OPTION...\n"
            "\n"
            "Options:\n"
            "\t-h           Show usage and exit\n"
            "\t-v           Increase verbosity\n"
            "\t-t FSTYPE    Filesystem type (REQUIRED)\n"
            "\t-i FILE      Image file path (REQUIRED)\n"
            "\t-P NUM       Partition to operate on (0 = entire disk)\n",
            PROGNAME);
    exit(eval);
}

void options(int* argc, char** argv[]) {
    int fstypeset = 0;
    int imgfileset = 0;
    int optc = -1;

    opterr = 0; /* provide our own warning message on '?' */

    while ((optc = getopt(*argc, *argv, "t:i:P:hev")) != -1) {
        switch (optc) {
        case 'h':
            usage(EXIT_SUCCESS);
            break;

        case 'v':
            ++prog.verbosity;
            break;

        case 't':
            if (!(streq(optarg, "btrfs") ||
                  streq(optarg, "ext2") ||
                  streq(optarg, "ext3") ||
                  streq(optarg, "ext4") ||
                  streq(optarg, "vfat") ||
                  streq(optarg, "xfs")))
                errx(EXIT_FAILURE, "unknown fstype: %s", optarg);
            strlcpy_notrunc(prog.fstype, optarg, sizeof(prog.fstype));
            fstypeset = 1;
            break;

        case 'i':
            strlcpy_notrunc(prog.imgfile, optarg, sizeof(prog.imgfile));
            imgfileset = 1;
            break;

        case 'P':
            prog.part = atol(optarg);
            if (prog.part < 0 || prog.part > 128)
                errx(EXIT_FAILURE, "-P NUM must be in range [0, 128]");
            break;

        case '?':
        default:
            usage(EXIT_FAILURE);
        }
    }

    if (!(fstypeset && imgfileset))
        errx(EXIT_FAILURE, "please specify -t and -i; see -h for more information");

    *argc -= optind;
    *argv += optind;
    optind = 1;
}

void do_file(char const name[PATH_MAX],
             char const infile[PATH_MAX],
             const mode_t mode,
             const uid_t uid,
             const gid_t gid) {
    int ret = 0;
    char const* relname = asrelpath(name);

    int infd = openat(AT_FDCWD, infile, O_RDONLY);
    if (infd < 0)
        err(EXIT_FAILURE, "failed to open %s for reading", infile);

    struct stat infile_stat;
    if (fstat(infd, &infile_stat) < 0)
        err(EXIT_FAILURE, "stat infile");

    ret = posix_fadvise(infd, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (ret != 0)
        errx(EXIT_FAILURE, "failed to set usage advice: %s", strerror(ret));

    int outfd = lkl_sys_openat(prog.relfd, relname,
            LKL_O_WRONLY | LKL_O_TRUNC | LKL_O_CREAT, mode);
    if (outfd < 0)
        errx(EXIT_FAILURE, "failed to open %s for writing: %s", name, lkl_strerror(outfd));

    char cpbuf[BUFSIZ];
    ssize_t ibytes;
    size_t cpbytes = 0;
    while ((ibytes = read(infd, cpbuf, sizeof(cpbuf))) > 0) {
        lkl_sys_write(outfd, cpbuf, ibytes);
        cpbytes += ibytes;
    }

    assert(cpbytes == infile_stat.st_size);

    if (prog.verbosity > 0)
        fprintf(stderr, "copied %ld of %ld bytes from %s to %s\n", cpbytes,
                infile_stat.st_size, infile, name);

    ret = lkl_sys_fchown(outfd, uid, gid);
    if (ret < 0)
        errx(EXIT_FAILURE, "failed to chown: %s", lkl_strerror(ret));

    lkl_sys_close(outfd);
    close(infd);
}

void do_slink(char const name[PATH_MAX],
              char const target[PATH_MAX],
              const uid_t uid,
              const gid_t gid) {
    if (streq(prog.fstype, "vfat"))
        errx(EXIT_FAILURE, "entry type slink unsupported on vfat");

    int ret = 0;
    char const* relname = asrelpath(name);

    ret = lkl_sys_symlinkat(target, prog.relfd, relname);
    if (ret && ret != -LKL_EEXIST)
        errx(EXIT_FAILURE, "symlink %s -> %s failed: %s", name, target, lkl_strerror(ret));

    ret = lkl_sys_fchownat(prog.relfd, relname, uid, gid, AT_SYMLINK_NOFOLLOW);
    if (ret)
        errx(EXIT_FAILURE, "failed chown %d.%d %s: %s", uid, gid, name, lkl_strerror(ret));
}

void do_dir(char const name[PATH_MAX],
            const mode_t mode,
            const uid_t uid,
            const gid_t gid) {
    int ret = 0;
    char const* relname = asrelpath(name);

    ret = lkl_sys_mkdirat(prog.relfd, relname, mode);
    if (ret && ret != -LKL_EEXIST)
        errx(EXIT_FAILURE, "mkdir '%s': %s", name, lkl_strerror(ret));

    ret = lkl_sys_fchownat(prog.relfd, relname, uid, gid, 0);
    if (ret)
        errx(EXIT_FAILURE, "failed chown %d.%d %s: %s", uid, gid, name, lkl_strerror(ret));
}

void do_special(char const name[PATH_MAX],
                const mode_t mode,
                const uid_t uid,
                const gid_t gid,
                const char type,
                const int maj,
                const int min) {
    int ret = 0;
    char const* relname = asrelpath(name);

    int typeflag =
        type == 'c' ? LKL_S_IFCHR :
        type == 'b' ? LKL_S_IFBLK :
        type == 'p' ? LKL_S_IFIFO :
        type == 's' ? LKL_S_IFSOCK :
        LKL_S_IFREG;

    ret = lkl_sys_mknodat(prog.relfd, relname, mode | typeflag, LKL_MKDEV(maj, min));
    if (ret && ret != -LKL_EEXIST)
        errx(EXIT_FAILURE, "failed to mknod %s: %s", name, lkl_strerror(ret));

    ret = lkl_sys_fchownat(prog.relfd, relname, uid, gid, 0);
    if (ret)
        errx(EXIT_FAILURE, "failed chown %d.%d %s: %s", uid, gid, name, lkl_strerror(ret));
}
