/*******************************************************************************
Guangzhou Xingyi Electronic  Technology Co., Ltd 2021-2030. All rights reserved.
* @brief         gyro_test.c
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
#include <string.h>

#define L3G4200D_IOCTL_BASE 77
#define L3G4200D_IOCTL_SET_ENABLE	_IOW(L3G4200D_IOCTL_BASE, 2, int)
#define SENSOR_ON			1
#define SENSOR_OFF			0

int main(int argc, char *argv[])
{
	struct input_event ev = {0};
	DIR *dir = NULL;
	int gyro_x, gyro_y, gyro_z;
	int calib_x, calib_y, calib_z;
	int x_off, y_off, z_off;
	int fd = -1;
	int misc_fd = -1;
	int ret = -1;
	char buf[64] = {0};
	int flag;

	setbuf(stdout, NULL);
	/* 1.先执行校准 */
	/* 校准之前必须让开发板或手机等设备水平静止放置(Z轴垂直) */
	printf(">>> 校准之前开发板必须处于水平放置状态 <<<\n");
	printf(">>> 1秒后开始执行校准 <<<\n");
	sleep(1);

	fd = open("/sys/class/sensor_class/gyro_calibration", O_RDWR);
	if (fd < 0) {
		printf("打开gyro_calibration失败!\n");
		return -1;
	}
	ret = write(fd, "1", 1);//写1进行校准
	if (ret != 1) {	//校准失败!
		close(fd);
		printf("执行校准失败!\n");
		return -1;
	}
	usleep(100*1000);
	printf(">>> 校准完成 <<<\n");

	/* 2.读取校准值 */
	ret = read(fd, buf, sizeof(buf));
	close(fd);
	if (ret < 18) {	//读取失败
		printf("读取校准值失败!\n");
		return -1;
	}

	x_off = atoi(&buf[17]);
	y_off = atoi(strchr(buf, ',') + 2);
	z_off = atoi(strrchr(buf, ',') + 2);
	printf("offset: [%d, %d, %d]\n", x_off, y_off, z_off);

	/* 3.打开加速计设备 */
	misc_fd = open("/dev/gyrosensor", O_RDWR);
	if (misc_fd < 0) {
		printf("打开/dev/gyrosensor设备失败!\n");
		return -1;
	}

	dir = opendir("/dev/input");
	if(dir != NULL) {
		struct dirent *de = NULL;

		memset(buf, 0x0, sizeof(buf));
		while((de = readdir(dir))) {
			if(strncmp(de->d_name, "event", 5))//判断是不是eventX设备节点
				continue;
			fd = openat(dirfd(dir), de->d_name, O_RDONLY);
			if(fd < 0)      //打开失败
				continue;

			if (ioctl(fd, EVIOCGNAME(sizeof(buf) - 1), &buf) < 1)
				buf[0] = '\0';//重置为空字符串
			if (strcmp(buf, "gyro")) {//如果不是重力加速计传感器
				close(fd);
				continue;
			}
			closedir(dir);//关闭文件夹
			goto start_read;//跳转到start_read
		}

		// 没有找到加速计传感器
		closedir(dir);
		close(misc_fd);
		return -1;
	}
	else {
		close(misc_fd);
		return -1;
	}

start_read:
	//传感器启动
	flag = SENSOR_ON;
	if (ioctl(misc_fd, L3G4200D_IOCTL_SET_ENABLE, &flag) < 0) {
		printf("启动传感器失败!\n");
		close(misc_fd);
		close(fd);
		return -1;
	}

	gyro_x = gyro_y = gyro_z = 0;
	for ( ; ; ) {

		if (read(fd, &ev, sizeof(ev)) != sizeof(ev)) {
			printf("读取数据失败!\n");
			flag = SENSOR_OFF;
			ioctl(misc_fd, L3G4200D_IOCTL_SET_ENABLE, &flag);
			close(misc_fd);
			close(fd);
			return -1;
		}

		if (ev.type == EV_ABS) {
			if (ev.code == ABS_RX)//绕X轴角速度
				gyro_x = ev.value;
			else if (ev.code == ABS_RY)//绕y轴角速度
				gyro_y = ev.value;
			else if (ev.code == ABS_RZ)//绕z轴角速度
				gyro_z = ev.value;
		}
		else if (ev.type == EV_SYN) {
			if (ev.code == SYN_REPORT) {//同步事件
				// 按照+-2000dps量程来计算  以角度/每秒单位进行计算 32768/2000 = 16.384
				calib_x = gyro_x - x_off;
				calib_y = gyro_y - y_off;
				calib_z = gyro_z - z_off;
				printf("%f	%f	%f\n", calib_x / 16.384, calib_y / 16.384, calib_z / 16.384);
			}
		}

		usleep(50*1000);
	}

	flag = SENSOR_OFF;
	ioctl(misc_fd, L3G4200D_IOCTL_SET_ENABLE, &flag);//停止传感器上报数据
	close(misc_fd);
	close(fd);
	return 0;
}

