#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/si_char"
#define BUFFER_SIZE 128

int main(void)
{
	int fd;
	ssize_t bytes;

	/*
	 * 这是一个字符数组，里面保存要写给驱动的字符串。
	 * sizeof(message) 会包含最后的 '\0'，所以写入时减 1。
	 */
	char message[] = "hello from user space\n";

	/* read() 会把驱动返回的数据写入这个数组。 */
	char buffer[BUFFER_SIZE];

	printf("[user] 1. call open(\"%s\", O_RDWR)\n", DEVICE_PATH);
	fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	printf("[user]    open returned fd=%d\n", fd);

	printf("[user] 2. call write(fd, message, %zu)\n",
	       sizeof(message) - 1);
	bytes = write(fd, message, sizeof(message) - 1);
	if (bytes < 0) {
		perror("write");
		close(fd);
		return 1;
	}
	printf("[user]    write returned %ld\n", (long)bytes);

	printf("[user] 3. call read(fd, buffer, %d)\n", BUFFER_SIZE - 1);
	bytes = read(fd, buffer, BUFFER_SIZE - 1);
	if (bytes < 0) {
		perror("read");
		close(fd);
		return 1;
	}

	/* read() 返回实际读到的字节数，在末尾补上字符串结束符。 */
	buffer[bytes] = '\0';
	printf("[user]    read returned %ld, data=\"%s\"\n",
	       (long)bytes, buffer);

	printf("[user] 4. call read() again to get EOF\n");
	bytes = read(fd, buffer, BUFFER_SIZE - 1);
	if (bytes < 0) {
		perror("read");
		close(fd);
		return 1;
	}
	printf("[user]    second read returned %ld (0 means EOF)\n",
	       (long)bytes);

	printf("[user] 5. call close(fd)\n");
	if (close(fd) < 0) {
		perror("close");
		return 1;
	}

	return 0;
}
