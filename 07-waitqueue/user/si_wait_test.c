#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/si_char_waitqueue"
#define BUFFER_SIZE 128

int main(void)
{
	int fd;
	pid_t child;
	char buffer[BUFFER_SIZE];
	char message[] = "data from writer\n";
	ssize_t bytes;
	int status;

	fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	child = fork();
	if (child < 0) {
		perror("fork");
		close(fd);
		return 1;
	}

	if (child == 0) {
		/* 子进程先 read；模块中没有数据，所以这里会睡眠。 */
		printf("[reader] calling read(), waiting for data...\n");
		fflush(stdout);
		bytes = read(fd, buffer, sizeof(buffer) - 1);
		if (bytes < 0) {
			perror("reader read");
			_exit(1);
		}
		buffer[bytes] = '\0';
		printf("[reader] read returned %ld, data=\"%s\"\n",
		       (long)bytes, buffer);
		close(fd);
		_exit(0);
	}

	/* 父进程等待两秒，方便观察子进程处于睡眠状态。 */
	sleep(2);
	printf("[writer] calling write(), this will wake reader\n");
	bytes = write(fd, message, sizeof(message) - 1);
	if (bytes < 0) {
		perror("writer write");
		close(fd);
		return 1;
	}

	waitpid(child, &status, 0);
	close(fd);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
