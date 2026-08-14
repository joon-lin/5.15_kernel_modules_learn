/*
 * Lesson 16 用户态测试程序。
 *
 * 每个 ioctl() 都会进入内核模块的 si_ioctl16_unlocked_ioctl()。
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../si_ioctl16_uapi.h"

static void fail(const char *operation)
{
	perror(operation);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	const char *device = "/dev/si_ioctl16";
	struct si_ioctl16_data data;
	uint32_t value;
	int fd;

	if (argc > 1)
		device = argv[1];

	fd = open(device, O_RDWR);
	if (fd < 0)
		fail("open");

	value = 123;
	if (ioctl(fd, SI_IOCTL16_SET_VALUE, &value) < 0)
		fail("SI_IOCTL16_SET_VALUE");
	printf("SET_VALUE: sent value=%u\n", value);

	value = 0;
	if (ioctl(fd, SI_IOCTL16_GET_VALUE, &value) < 0)
		fail("SI_IOCTL16_GET_VALUE");
	printf("GET_VALUE: received value=%u\n", value);

	memset(&data, 0, sizeof(data));
	data.value = 456;
	strncpy(data.message, "hello from user", sizeof(data.message) - 1);
	if (ioctl(fd, SI_IOCTL16_SET_DATA, &data) < 0)
		fail("SI_IOCTL16_SET_DATA");
	printf("SET_DATA: sent value=%u message=\"%s\"\n",
	       data.value, data.message);

	memset(&data, 0, sizeof(data));
	if (ioctl(fd, SI_IOCTL16_GET_DATA, &data) < 0)
		fail("SI_IOCTL16_GET_DATA");
	printf("GET_DATA: received value=%u message=\"%s\"\n",
	       data.value, data.message);

	close(fd);
	return 0;
}
