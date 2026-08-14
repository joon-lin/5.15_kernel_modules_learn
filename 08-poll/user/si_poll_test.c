#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEVICE_PATH "/dev/si_char_poll"
#define BUFFER_SIZE 128

int main(void)
{
	int fd;
	int ret;
	pid_t child;
	char buffer[BUFFER_SIZE];
	char message[] = "data from poll writer\n";
	ssize_t bytes;
	struct pollfd pfd;
	int status;

	/* O_NONBLOCK 让真正的 read() 不会因为竞态再次睡眠。 */
	fd = open(DEVICE_PATH, O_RDWR | O_NONBLOCK);
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
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		printf("[reader] calling poll(), waiting for POLLIN...\n");
		fflush(stdout);
		ret = poll(&pfd, 1, 5000);
		if (ret < 0) {
			perror("poll");
			close(fd);
			_exit(1);
		}
		if (ret == 0) {
			printf("[reader] poll timeout\n");
			close(fd);
			_exit(0);
		}

		printf("[reader] poll returned %d, revents=0x%x\n",
		       ret, pfd.revents);
		if (pfd.revents & POLLIN) {
			bytes = read(fd, buffer, sizeof(buffer) - 1);
			if (bytes < 0) {
				perror("read");
				close(fd);
				_exit(1);
			}
			buffer[bytes] = '\0';
			printf("[reader] read returned %ld, data=\"%s\"\n",
			       (long)bytes, buffer);
		}

		close(fd);
		_exit(0);
	}

	/* 父进程先等待两秒，让子进程真正进入 poll()。 */
	sleep(2);
	printf("[writer] calling write(), this makes fd readable\n");
	bytes = write(fd, message, sizeof(message) - 1);
	if (bytes < 0) {
		perror("write");
		close(fd);
		return 1;
	}

	waitpid(child, &status, 0);
	close(fd);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
