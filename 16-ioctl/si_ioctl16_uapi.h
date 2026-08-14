#ifndef SI_IOCTL16_UAPI_H
#define SI_IOCTL16_UAPI_H

/* 这个头文件同时被内核模块和用户态程序包含，定义两边共用的 ABI。 */
#include <linux/ioctl.h>
#include <linux/types.h>

#define SI_IOCTL16_MAGIC 'S'
#define SI_IOCTL16_MESSAGE_SIZE 32

struct si_ioctl16_data {
	__u32 value;
	char message[SI_IOCTL16_MESSAGE_SIZE];
};

/*
 * _IOW/_IOR 的方向是从用户空间角度描述：
 * _IOW：用户把数据写给内核；
 * _IOR：用户从内核读取数据。
 */
#define SI_IOCTL16_SET_VALUE \
	_IOW(SI_IOCTL16_MAGIC, 1, __u32)
#define SI_IOCTL16_GET_VALUE \
	_IOR(SI_IOCTL16_MAGIC, 2, __u32)
#define SI_IOCTL16_SET_DATA \
	_IOW(SI_IOCTL16_MAGIC, 3, struct si_ioctl16_data)
#define SI_IOCTL16_GET_DATA \
	_IOR(SI_IOCTL16_MAGIC, 4, struct si_ioctl16_data)

#endif
