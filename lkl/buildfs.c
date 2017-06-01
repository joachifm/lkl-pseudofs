/* buildfs.c - construct rootfs on a filesystem image */

/* lkl stuff inspired by lkl/cptofs.c, written by Octavian Purdila <octavian.purdila@intel.com>
 * and others.
 *
 * gen_init_cpio spec parsing lifted from usr/gen_init_cpio.c, by Jeff Garzik and others.
 */


#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <lkl.h>
#include <lkl_host.h>

#define xstr(s) #s
#define str(s) xstr(s)

#define STREQ(S1, S2) (strcmp(S1, S2) == 0)

#define FMT_PATH "%" str(PATH_MAX) "s"

#define FSTYPE_MAX 64

static char const progname[] = "buildfs";

static int is_valid_fstype(char const* s) {
    return s &&
        (STREQ(s, "ext2") ||
         STREQ(s, "ext3") ||
         STREQ(s, "ext4") ||
         STREQ(s, "btrfs") ||
         STREQ(s, "vfat"));
}

static int xsscanf(char const* fmt, size_t nparam, char const* args, ...) {
    va_list ap;
    va_start(ap, args);
    ssize_t res = vsscanf(args, fmt, ap);
    va_end(ap);
    return res != nparam;
}

static const char* asrelpath(const char* pathname) {
    return (*pathname == '/') ? ++pathname : pathname;
}

/* dirfd to internal fs image mount path; used to openat files within the
 * image. */
static int mntfd = -EBADF;

/* Handlers */

static int do_file(char const name[PATH_MAX], char const infile[PATH_MAX],
        mode_t mode, uid_t uid, gid_t gid) {
    int err = 0;

    int infd = open(infile, O_RDONLY, 0);
    if (infd < 0) {
        fprintf(stderr, "failed to open infile for reading: %s\n",
                strerror(errno));
        err = -errno;
        goto out;
    }

    err = posix_fadvise(infd, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (err != 0) {
        fprintf(stderr, "failed to fadvise infile: %s\n", strerror(err));
        err = -err;
        goto out;
    }

    int outfd = lkl_sys_openat(mntfd, asrelpath(name), LKL_O_WRONLY | LKL_O_TRUNC | LKL_O_CREAT, mode);
    if (outfd < 0) {
        fprintf(stderr, "failed to open outfile for writing: %s\n",
                lkl_strerror(outfd));
        err = outfd;
        goto out;
    }

    err = lkl_sys_fchown(outfd, uid, gid);
    if (err < 0) {
        fprintf(stderr, "failed to set ownership: %s\n",
                lkl_strerror(err));
        goto out;
    }

    char cpbuf[BUFSIZ];
    ssize_t ibytes;
    while ((ibytes = read(infd, cpbuf, sizeof(cpbuf))) > 0)
        lkl_sys_write(outfd, cpbuf, ibytes);

out:
    lkl_sys_close(outfd);
    close(infd);
    return err;
}

static int do_slink(char const name[PATH_MAX],
        char const target[PATH_MAX], mode_t mode, uid_t uid, gid_t gid) {
    int err = 0;

    err = lkl_sys_symlinkat(target, mntfd, asrelpath(name));
    if (err) {
        fprintf(stderr, "unable to symlink %s -> %s: %s\n",
                name, target, lkl_strerror(err));
        goto out;
    }

    err = lkl_sys_fchownat(mntfd, asrelpath(name), uid, gid, AT_SYMLINK_NOFOLLOW);
    if (err) {
        fprintf(stderr, "unable to set ownership: %s\n",
                lkl_strerror(err));
        goto out;
    }

out:
    return err;
}

static int do_dir(char const name[PATH_MAX], mode_t mode, uid_t uid,
        gid_t gid) {
    int err = 0;

    err = lkl_sys_mkdirat(mntfd, asrelpath(name), mode);
    if (err) { /* err != -LKL_EEXIST to ignore already existing dir */
        fprintf(stderr, "unable to create dir '%s': %s\n", name,
                lkl_strerror(err));
        return err;
    }

    err = lkl_sys_fchownat(mntfd, asrelpath(name), uid, gid, 0);
    if (err < 0) {
        fprintf(stderr, "unable to set ownership: %s\n", lkl_strerror(err));
        return err;
    }

    return 0;
}

static int do_nod(char const name[PATH_MAX], mode_t mode,
        uid_t uid, gid_t gid, char type, int maj, int min) {
    int err = 0;

    int typeflag = LKL_S_IFREG;
    switch (type) {
        case 'c':
            typeflag = LKL_S_IFCHR;
            break;
        case 'b':
            typeflag = LKL_S_IFBLK;
            break;
        case 's':
            typeflag = LKL_S_IFSOCK;
            break;
        case 'p':
            typeflag = LKL_S_IFIFO;
            break;
        case 'r':
            typeflag = LKL_S_IFREG;
            break;
    }

    err = lkl_sys_mknodat(mntfd, asrelpath(name), mode | typeflag, LKL_MKDEV(maj, min));
    if (err) {
        fprintf(stderr, "failed to create node %s: %s\n", name, lkl_strerror(err));
        return err;
    }

    err = lkl_sys_fchownat(mntfd, asrelpath(name), uid, gid, 0);
    if (err) {
        fprintf(stderr, "failed to set owner %s: %s\n", name, lkl_strerror(err));
        return err;
    }

    return 0;
}

static int do_sock(char const name[PATH_MAX], mode_t mode, uid_t uid, gid_t gid) {
    return do_nod(name, mode, uid, gid, 's', 0, 0);
}

static int do_pipe(char const name[PATH_MAX], mode_t mode, uid_t uid, gid_t gid) {
    return do_nod(name, mode, uid, gid, 'p', 0, 0);
}

/* Main */

int main(int argc, char* argv[argc]) {
    int ret; /* program return code */

    int part = 0;
    char fstype[FSTYPE_MAX] = {0};
    char imgpath[PATH_MAX] = {0};

    /*
     * Parse command-line
     */

    char const opts[] = "hP:t:i:";
    int optchar;
    while ((optchar = getopt(argc, argv, opts)) != -1) {
        switch (optchar) {
            case 't':
                strncpy(fstype, optarg, sizeof(fstype));
                break;
            case 'i':
                strncpy(imgpath, optarg, sizeof(imgpath));
                break;
            case 'P':
                part = atoi(optarg);
                break;
            case 'h':
                printf("usage: %s [-t FSTYPE, -i FILE, -P NUM]\n", progname);
                return EXIT_SUCCESS;
            case '?':
                break;
            default:
                printf("?? getopt returned %o ??\n", optchar);
        }
    }

    /*
     * Validate inputs
     */

    if (!fstype || strlen(fstype) == 0) {
        fprintf(stderr, "please specify a fs type\n");
        return 1;
    }
    if (!is_valid_fstype(fstype)) {
        fprintf(stderr, "invalid fstype: %s\n", fstype);
        return 1;
    }

    if (part < 0 || part > 128) {
        fprintf(stderr, "partition must be in [0,128]!\n");
        return 1;
    }

    if (!imgpath || strlen(imgpath) == 0) {
        fprintf(stderr, "please specify a disk image path\n");
        return 1;
    }
    if (access(imgpath, O_RDWR) < 0) {
        fprintf(stderr, "unable to read/write image path '%s': %s\n",
                imgpath, strerror(errno));
        return 1;
    }

    if (part == 0) {
        fprintf(stderr, "NOTICE: operating on entire disk\n");
    }

    /*
     * Process
     */

    struct lkl_disk disk = {0}; /* host disk handle */
    int disk_id;

    disk.fd = open(imgpath, O_RDWR);
    if (disk.fd < 0) {
        fprintf(stderr, "failed to open %s for writing: %s\n", imgpath, strerror(errno));
        ret = EXIT_FAILURE;
        goto out;
    }

    disk_id = lkl_disk_add(&disk);
    if (disk_id < 0) {
        fprintf(stderr, "failed to add disk: %s\n", lkl_strerror(disk_id));
        ret = EXIT_FAILURE;
        goto out;
    }

    lkl_host_ops.print = 0;
    lkl_start_kernel(&lkl_host_ops, "mem=6M");

    char mnt[PATH_MAX] = {0};
    ret = lkl_mount_dev(disk_id, part, fstype, 0, 0, mnt, sizeof(mnt));
    if (ret) {
        fprintf(stderr, "failed to mount disk: %s\n", lkl_strerror(ret));
        ret = EXIT_FAILURE;
        goto out;
    }

    mntfd = lkl_sys_open(mnt, LKL_O_PATH | LKL_O_DIRECTORY, 0);
    if (mntfd < 0) {
        fprintf(stderr, "failed to open mntfd: %s\n", lkl_strerror(mntfd));
        ret = EXIT_FAILURE;
        goto out;
    }

    FILE* spec_list = stdin; /* the spec source */
    char line[2 * PATH_MAX + 64]; /* current spec line */
    long lineno = 0; /* current line number */
    char* type; /* holds the "type" part of the spec */
    char* args; /* holds the remaining "args" part of the spec */

    while (fgets(line, sizeof(line), spec_list)) {
        size_t slen = strlen(line);
        ++lineno;

        if (*line == '#')
            continue;

        if (!(type = strtok(line, " \t"))) {
            fprintf(stderr, "%ld: expected tab separator\n", lineno);
            continue;
        }

        if (*type == '\n' || slen == strlen(type)) /* blank line */
            continue;

        if (!(args = strtok(0, "\n"))) {
            fprintf(stderr, "%ld: expected args\n", lineno);
            continue;
        }

        int err = 0;
        char name[PATH_MAX];
        mode_t mode;
        uid_t uid;
        gid_t gid;

        if      (STREQ(type, "file")) {
            char infile[PATH_MAX];
            char const fmt[] = FMT_PATH " " FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 5, args, name, infile, &mode, &uid, &gid)))
                err = do_file(name, infile, mode, uid, gid);
        }
        else if (STREQ(type, "dir")) {
            char const fmt[] = FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 4, args, name, &mode, &uid, &gid)))
                err = do_dir(name, mode, uid, gid);
        }
        else if (STREQ(type, "slink")) {
            char target[PATH_MAX];
            char const fmt[] = FMT_PATH " " FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 5, args, name, target, &mode, &uid, &gid)))
                err = do_slink(name, target, mode, uid, gid);
        }
        else if (STREQ(type, "nod")) {
            char devtype; int maj; int min;
            char const fmt[] = FMT_PATH " %o %d %d %c %d %d";
            if (!(err = xsscanf(fmt, 7, args, name, &mode, &uid, &gid, &devtype, &maj, &min)))
                err = do_nod(name, mode, uid, gid, devtype, maj, min);
        }
        else if (STREQ(type, "pipe")) {
            char const fmt[] = FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 4, args, name, &mode, &uid, &gid)))
                err = do_pipe(name, mode, uid, gid);
        }
        else if (STREQ(type, "sock")) {
            char const fmt[] = FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 4, args, name, &mode, &uid, &gid)))
                err = do_sock(name, mode, uid, gid);
        }
        else {
            err = 1;
        }

        if (err)
            fprintf(stderr, "%ld: some error\n", lineno);
    }

out:
    lkl_sys_close(mntfd);
    lkl_umount_dev(disk_id, part, 0, 1000);
    lkl_sys_halt();
    close(disk.fd);
    return ret;
}
