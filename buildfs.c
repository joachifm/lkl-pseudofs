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

typedef int (*type_handler)(char*);

struct handler {
    char t[8];
    type_handler proc;
};

static int do_slink(char name[PATH_MAX], char target[PATH_MAX], int mode,
        int uid, int gid) {
    return 0;
}

static int do_dir(char name[PATH_MAX], int mode, int uid, int gid) {
    int ret;
    printf("name=%s\n", name);
    printf("mode=%d\n", mode);
    printf("uid=%d\n", uid);
    printf("gid=%d\n", gid);
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

static struct handler handlers[] = {
    { .t = "dir",
      .proc = do_dir_line,
    },
};

int main(int argc, char* argv[argc]) {
    int ret; /* program return code */

    struct lkl_disk disk = {0}; /* host disk handle */
    int disk_id;
    char mnt[PATH_MAX] = {0}; /* holds the disk mount path */
    int part = 0;

    lkl_host_ops.print = 0;

    disk.fd = open("fs.img", O_RDWR);
    if (disk.fd < 0) {
        fprintf(stderr, "failed to open fs.img: %s\n", strerror(errno));
        ret = 1;
        goto out;
    }

    disk_id = lkl_disk_add(&disk);
    if (disk_id < 0) {
        fprintf(stderr, "failed to add disk: %s\n", lkl_strerror(ret));
        ret = 1;
        goto out_close;
    }

    lkl_start_kernel(&lkl_host_ops, "mem=20M");

    if (lkl_mount_dev(disk_id, part, "ext4", 0, 0, mnt, sizeof(mnt))) {
        fprintf(stderr, "failed to mount disk: %s\n", lkl_strerror(ret));
        ret = 1;
        goto out_close;
    }

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

out_umount:
    (void)lkl_umount_dev(disk_id, part, 0, 1000);

out_close:
    close(disk.fd);

out_halt:
    lkl_sys_halt();

out:
    return ret;
}
