/* buildfs.c - construct rootfs on a filesystem image */

/* lkl stuff inspired by lkl/cptofs.c, written by Octavian Purdila <octavian.purdila@intel.com>
 * and others.
 *
 * gen_init_cpio spec parsing lifted from usr/gen_init_cpio.c, by Jeff Garzik and others.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <lkl.h>
#include <lkl_host.h>

#define xstr(s) #s
#define str(s) xstr(s)

#define FMT_PATH "%" str(PATH_MAX) "s"

#define STREQ(S1, S2) (strcmp(S1, S2) == 0)

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

static char mnt[PATH_MAX]; /* holds the fs image mount path */
static char thesysname[PATH_MAX]; /* holds path into mounted fs image */
static char const* get_sysname(char const name[PATH_MAX]) {
    snprintf(thesysname, PATH_MAX, "%s/%s", mnt, name);
    return thesysname;
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
    char const* const sysname = get_sysname(name);

    err = lkl_sys_symlink(target, sysname);
    if (err) {
        fprintf(stderr, "unable to symlink %s -> %s: %s\n",
                name, target, lkl_strerror(err));
        goto out;
    }

    err = lkl_sys_lchown(sysname, uid, gid);
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
    char const* const sysname = get_sysname(name);

    err = lkl_sys_mkdir(sysname, mode);
    if (err) { /* err != -LKL_EEXIST to ignore already existing dir */
        fprintf(stderr, "unable to create dir '%s': %s\n", name,
                lkl_strerror(err));
        return err;
    }

    err = lkl_sys_chown(sysname, uid, gid);
    if (err < 0) {
        fprintf(stderr, "unable to set ownership: %s\n", lkl_strerror(err));
        return err;
    }

    return 0;
}

static int do_nod(char const name[PATH_MAX], mode_t mode,
        uid_t uid, gid_t gid, char type, int maj, int min) {
    int err = 0;
    char const* const sysname = get_sysname(name);

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

    err = lkl_sys_mknod(sysname, mode | typeflag, LKL_MKDEV(maj, min));
    if (err) {
        fprintf(stderr, "failed to create node %s: %s\n", name, lkl_strerror(err));
        return err;
    }

    err = lkl_sys_chown(sysname, uid, gid);
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

    int part = 0; /* 0 = whole disk, n = partition n */
    char fstype[] = "ext4";

    struct lkl_disk disk = {0}; /* host disk handle */
    int disk_id;

    disk.fd = open("fs.img", O_RDWR);
    if (disk.fd < 0) {
        fprintf(stderr, "failed to open fs.img: %s\n", strerror(errno));
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
