/* Derived from
- lkl/cptofs.c
- usr/gen_init_cpio.c
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

int main(int argc, char* argv[argc]) {
    int ret; /* program return code */

    struct lkl_disk disk = {0}; /* host disk handle */
    int disk_id;
    char mnt[PATH_MAX] = {0}; /* holds the disk mount path */
    int part = 0;

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
    long lineno = 0;
    char* type;
    while (fgets(line, LINE_SIZE, spec_list)) {
        int type_idx;
        size_t slen = strlen(line);
        ++lineno;

        if (*line == '#')
            continue;

        if (!(type = strtok(line, "\t")))
            break;
    }

    (void)lkl_umount_dev(disk_id, part, 0, 1000);

out_close:
    close(disk.fd);

out_halt:
    lkl_sys_halt();

out:
    return ret;
}
