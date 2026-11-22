/*******************************************************************************
Guangzhou Xingyi Electronic  Technology Co., Ltd 2021-2030. All rights reserved.
* @brief         ap3216c_test.c
* @author        正点原子Linux团队
* @date          2023-06-26
* @link          http://www.openedv.com/forum.php
********************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <dirent.h>
#include <sys/select.h>
#include <string.h>

#define SENSOR_ON               1
#define SENSOR_OFF              0

#define LIGHTSENSOR_IOCTL_MAGIC                                 'l'
#define LIGHTSENSOR_IOCTL_GET_ENABLED                   _IOR(LIGHTSENSOR_IOCTL_MAGIC, 1, int *)
#define LIGHTSENSOR_IOCTL_ENABLE                                        _IOW(LIGHTSENSOR_IOCTL_MAGIC, 2, int *)

#define PSENSOR_IOCTL_MAGIC                             'p'
#define PSENSOR_IOCTL_GET_ENABLED               _IOR(PSENSOR_IOCTL_MAGIC, 1, int *)
#define PSENSOR_IOCTL_ENABLE                            _IOW(PSENSOR_IOCTL_MAGIC, 2, int *)

static int in_fd  = -1;
static int in_fd1  = -1;
static int misc_fd = -1;
static int misc_fd1 = -1;

int main()
{
    struct input_event ev = {0};
    fd_set rdfds;
    DIR *dir = NULL;
    int flag;

    setbuf(stdout, NULL);
    printf("===  ap3216c testing  ===");
    dir = opendir("/dev/input");
    if(dir != NULL) {
        struct dirent *de = NULL;
        char name[64];
        int fd;

        while((de = readdir(dir))) {
            if(strncmp(de->d_name, "event", 5))
                continue;

            fd = openat(dirfd(dir), de->d_name, O_RDONLY);
            if(fd < 0)
                continue;

            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1)
                name[0] = '\0';
            if (strcmp(name, "lightsensor-level")) {
                close(fd);
                continue;
            }

            in_fd = fd;
            break;
        }

        closedir(dir);
    }

    dir = opendir("/dev/input");
    if (dir != NULL) {
        struct dirent *de = NULL;
        char name[64];
        int fd;

        while((de = readdir(dir))) {
            if(strncmp(de->d_name, "event", 5))
                continue;

            fd = openat(dirfd(dir), de->d_name, O_RDONLY);
            if(fd < 0)
                continue;

            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1)
                name[0] = '\0';
            if (strcmp(name, "proximity")) {
                close(fd);
                continue;
            }

            in_fd1 = fd;
            break;
        }

        closedir(dir);
    }

    if (in_fd == -1)
        goto err_out;

    if (in_fd1 == -1)
        goto err_out;

    misc_fd = open("/dev/lightsensor", O_WRONLY);
    misc_fd1 = open("/dev/psensor", O_WRONLY);
    if (misc_fd < 0) {
        close(in_fd);
        goto err_out;
    }

    if (misc_fd1 < 0) {
        close(in_fd1);
        goto err_out;
    }


    flag = SENSOR_ON;
    ioctl(misc_fd, LIGHTSENSOR_IOCTL_ENABLE, &flag);
    ioctl(misc_fd1, PSENSOR_IOCTL_ENABLE, &flag);

    for ( ; ; ) {
        FD_ZERO(&rdfds);
        FD_SET(in_fd, &rdfds);
        FD_SET(in_fd1, &rdfds);

        if (select(in_fd1 + 1, &rdfds , NULL, NULL, NULL) < 0) {
            printf("read failed!\n");
            close(in_fd);
            close(misc_fd);
            close(in_fd1);
            close(misc_fd1);
            goto err_out;
        }

        if (FD_ISSET(in_fd, &rdfds)) {
            read(in_fd, &ev, sizeof(ev));
        }

        if (ev.type == EV_ABS && ev.code == ABS_MISC)
            printf("光强度等级0～7：当前为%d\n", ev.value);

        if (FD_ISSET(in_fd1, &rdfds)) {
            read(in_fd1, &ev, sizeof(ev));
        }

        if (ev.type == EV_ABS && ev.code == ABS_DISTANCE) {
            if (ev.value == 1)
                printf("距离状态：接近\n");
            else
                printf("距离状态：远离\n");
        }
    }

    return 0;

err_out:
    flag = SENSOR_OFF;
    ioctl(misc_fd, LIGHTSENSOR_IOCTL_ENABLE, &flag);
    ioctl(misc_fd, PSENSOR_IOCTL_ENABLE, &flag);
    printf("===  end of ap3216c test  ====\n");
    return -1;
}
