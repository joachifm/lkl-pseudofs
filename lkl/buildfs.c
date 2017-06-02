/* buildfs.c - construct rootfs on a filesystem image */

/* lkl stuff inspired by lkl/cptofs.c, written
 * by Octavian Purdila and others.
 *
 * gen_init_cpio spec parsing initially lifted from usr/gen_init_cpio.c,
 * by Jeff Garzik and others.
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

#define FMT_PATH "%" str(PATH_MAX) "s"

#define FSTYPE_MAX 64

static char const progname[] = "buildfs";

static inline int streq(char const* s1, char const* s2) {
    return strcmp(s1, s2) == 0;
}

static int is_valid_fstype(char const* s) {
    return s &&
        (streq(s, "ext2") ||
         streq(s, "ext3") ||
         streq(s, "ext4") ||
         streq(s, "btrfs") ||
         streq(s, "vfat"));
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

/* dirfd to internal fs image mount path.  Used to manipulate
 * paths within the mounted file system image via *_at syscalls.
 * This is to avoid having to recover the "real" path by prepending
 * the internal mnt path. */
static int mntdirfd = -EBADF;

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

    int outfd = lkl_sys_openat(mntdirfd, asrelpath(name), LKL_O_WRONLY | LKL_O_TRUNC | LKL_O_CREAT, mode);
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

    err = lkl_sys_symlinkat(target, mntdirfd, asrelpath(name));
    if (err) {
        fprintf(stderr, "unable to symlink %s -> %s: %s\n",
                name, target, lkl_strerror(err));
        goto out;
    }

    err = lkl_sys_fchownat(mntdirfd, asrelpath(name), uid, gid, AT_SYMLINK_NOFOLLOW);
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

    err = lkl_sys_mkdirat(mntdirfd, asrelpath(name), mode);
    if (err) { /* err != -LKL_EEXIST to ignore already existing dir */
        fprintf(stderr, "unable to create dir '%s': %s\n", name,
                lkl_strerror(err));
        return err;
    }

    err = lkl_sys_fchownat(mntdirfd, asrelpath(name), uid, gid, 0);
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

    err = lkl_sys_mknodat(mntdirfd, asrelpath(name), mode | typeflag, LKL_MKDEV(maj, min));
    if (err) {
        fprintf(stderr, "failed to create node %s: %s\n", name, lkl_strerror(err));
        return err;
    }

    err = lkl_sys_fchownat(mntdirfd, asrelpath(name), uid, gid, 0);
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
    int verbosity = 0;

    /*
     * Parse command-line
     */

    char const opts[] = "hvP:t:i:";
    int optchar;
    while ((optchar = getopt(argc, argv, opts)) != -1) {
        switch (optchar) {
            case 'v':
                ++verbosity;
                break;
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
                printf("usage: %s OPTION...\n"
                       "\n"
                       "Options:\n"
                       "\t-v"
                       "\t-t FSTYPE\n"
                       "\t-i FILE\n"
                       "\t-P NUM\n",
                       progname);
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

    if (verbosity < 3)
        lkl_host_ops.print = 0;

    lkl_start_kernel(&lkl_host_ops, "mem=6M");

    char mnt[PATH_MAX] = {0};
    ret = lkl_mount_dev(disk_id, part, fstype, 0, 0, mnt, sizeof(mnt));
    if (ret) {
        fprintf(stderr, "failed to mount disk: %s\n", lkl_strerror(ret));
        ret = EXIT_FAILURE;
        goto out;
    }

    mntdirfd = lkl_sys_open(mnt, LKL_O_PATH | LKL_O_DIRECTORY, 0);
    if (mntdirfd < 0) {
        fprintf(stderr, "failed to open mntdirfd: %s\n", lkl_strerror(mntdirfd));
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

        if      (streq(type, "file")) {
            char infile[PATH_MAX];
            char const fmt[] = FMT_PATH " " FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 5, args, name, infile, &mode, &uid, &gid)))
                err = do_file(name, infile, mode, uid, gid);
        }
        else if (streq(type, "dir")) {
            char const fmt[] = FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 4, args, name, &mode, &uid, &gid)))
                err = do_dir(name, mode, uid, gid);
        }
        else if (streq(type, "slink")) {
            char target[PATH_MAX];
            char const fmt[] = FMT_PATH " " FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 5, args, name, target, &mode, &uid, &gid)))
                err = do_slink(name, target, mode, uid, gid);
        }
        else if (streq(type, "nod")) {
            char devtype; int maj; int min;
            char const fmt[] = FMT_PATH " %o %d %d %c %d %d";
            if (!(err = xsscanf(fmt, 7, args, name, &mode, &uid, &gid, &devtype, &maj, &min)))
                err = do_nod(name, mode, uid, gid, devtype, maj, min);
        }
        else if (streq(type, "pipe")) {
            char const fmt[] = FMT_PATH " %o %d %d";
            if (!(err = xsscanf(fmt, 4, args, name, &mode, &uid, &gid)))
                err = do_pipe(name, mode, uid, gid);
        }
        else if (streq(type, "sock")) {
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
    lkl_sys_close(mntdirfd);
    lkl_umount_dev(disk_id, part, 0, 1000);
    lkl_sys_halt();
    close(disk.fd);
    return ret;
}
