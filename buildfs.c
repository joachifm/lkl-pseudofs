/* Derived from
 *
 * + lkl/cptofs.c
 * + usr/gen_init_cpio.c
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include <lkl.h>
#include <lkl_host.h>

#define array_count(X) (sizeof(X)/sizeof((X)[0]))

#define xstr(s) #s
#define str(s) xstr(s)

static char mnt[PATH_MAX]; /* holds the disk image mount path */
static char sysname[PATH_MAX]; /* holds path into mounted disk image */

static char const* get_sysname(char const* name) {
    (void)snprintf(sysname, PATH_MAX, "%s/%s", mnt, name);
    return sysname;
}

typedef int (*type_handler)(char*);

struct handler {
    char t[8];
    type_handler proc;
};

/* Handlers */

static int do_nod(char name[PATH_MAX], int mode, int uid, int gid,
        char devtype, int maj, int min) {
    int flags = (devtype == 'b' ? LKL_S_IFBLK : LKL_S_IFCHR) | mode;
    int err = lkl_sys_mknod(get_sysname(name), flags, LKL_MKDEV(maj, min));
    if (err) {
        fprintf(stderr, "failed to create node %s: %s\n",
                name, lkl_strerror(err));
        return err;
    }
    return 0;
}

static int do_nod_line(char* args) {
    if (!args)
        return -EINVAL;

    char name[PATH_MAX];
    int mode;
    int uid;
    int gid;
    char devtype; /* 'b' or 'c' */
    int maj;
    int min;

    int ret = sscanf(args, "%" str(PATH_MAX) "s %o %d %d %c %d %d",
            name, &mode, &uid, &gid, &devtype, &maj, &min);
    if (ret != 7)
        return -EINVAL;
    if (ret < 0)
        return ret;

    return do_nod(name, mode, uid, gid, devtype, maj, min);
}


static int do_slink(char name[PATH_MAX], char target[PATH_MAX], int mode,
        int uid, int gid) {
    int err = lkl_sys_symlink(target, get_sysname(name));
    if (err) {
        fprintf(stderr, "unable to symlink %s -> %s: %s\n",
                name, target, lkl_strerror(err));
        return err;
    }
    return 0;
}

static int do_slink_line(char* args) {
    if (!args)
        return -EINVAL;

    char name[PATH_MAX];
    char target[PATH_MAX];
    int mode;
    int uid;
    int gid;

    int ret = sscanf(args, "%" str(PATH_MAX) "s %" str(PATH_MAX) "s %o %d %d",
            name, target, &mode, &uid, &gid);
    if (ret != 5)
        return -EINVAL;
    if (ret < 0)
        return ret;

    return do_slink(name, target, mode, uid, gid);
}


static int do_dir(char name[PATH_MAX], int mode, int uid, int gid) {
    int err = lkl_sys_mkdir(get_sysname(name), mode);
    if (err) { /* err != -LKL_EEXIST to ignore already existing dir */
        fprintf(stderr, "unable to create dir '%s': %s\n", name,
                lkl_strerror(err));
        return err;
    }

    return 0;
}

static int do_dir_line(char* args) {
    if (!args)
        return -EINVAL;

    char name[PATH_MAX] = {0};
    int mode = 0;
    int uid = 0;
    int gid = 0;

    int ret = sscanf(args, "%" str(PATH_MAX) "s %o %d %d", name, &mode, &uid, &gid);
    if (ret != 4)
        return -EINVAL;
    if (ret < 0)
        return ret;

    return do_dir(name, mode, uid, gid);
}

/* The handler table */

static struct handler handlers[] = {
    { .t = "dir",
      .proc = do_dir_line,
    },
    { .t = "slink",
      .proc = do_slink_line,
    },
    { .t = "nod",
      .proc = do_nod_line,
    },
};

/* Main */

int main(int argc, char* argv[argc]) {
    int ret; /* program return code */

    int part = 0;

    struct lkl_disk disk = {0}; /* host disk handle */
    int disk_id;

    lkl_host_ops.print = 0;

    /* Add disks */
    disk.fd = open("fs.img", O_RDWR);
    if (disk.fd < 0) {
        fprintf(stderr, "failed to open fs.img: %s\n", strerror(errno));
        ret = 1;
        goto out;
    }

    disk_id = lkl_disk_add(&disk);
    if (disk_id < 0) {
        fprintf(stderr, "failed to add disk: %s\n", lkl_strerror(disk_id));
        ret = 1;
        goto out_close;
    }

    /* Init kernel */
    lkl_start_kernel(&lkl_host_ops, "mem=20M");

    /* Mount image */
    if (lkl_mount_dev(disk_id, part, "ext4", 0, 0, mnt, sizeof(mnt))) {
        fprintf(stderr, "failed to mount disk: %s\n", lkl_strerror(ret));
        ret = 1;
        goto out_close;
    }

    /* Process specs */
#define LINE_SIZE (2 * PATH_MAX + 58)
    FILE* spec_list = stdin; /* the spec source */
    char line[LINE_SIZE]; /* current spec line */
    long lineno = 0; /* current line number */
    char* type; /* holds the "type" part of the spec */
    char* args; /* holds the remaining "args" part of the spec */

    while (fgets(line, LINE_SIZE, spec_list)) {
        size_t slen = strlen(line); /* record line length before further processing */
        ++lineno;

        if (*line == '#')
            continue;

        /* Parse type */
        if (!(type = strtok(line, "\t"))) {
            ret = 1;
            break;
        }

        if (*type == '\n')
            continue;

        if (slen == strlen(type))
            continue;

        /* Parse args */
        if (!(args = strtok(0, "\n"))) {
            ret = 1;
            break;
        }

        /* Dispatch on type */
        type_handler do_type = 0;

        for (size_t i = 0; i < array_count(handlers); ++i) {
            if (strcmp(type, handlers[i].t) == 0) {
                do_type = handlers[i].proc;
                break;
            }
        }

        if (!do_type) {
            fprintf(stderr, "unrecognized type: %s\n", type);
            ret = 1;
            goto out_umount;
        }

        ret = do_type(args);
    }

    /* Done */

out_umount:
    (void)lkl_umount_dev(disk_id, part, 0, 1000);

out_close:
    close(disk.fd);

out_halt:
    lkl_sys_halt();

out:
    return ret;
}
