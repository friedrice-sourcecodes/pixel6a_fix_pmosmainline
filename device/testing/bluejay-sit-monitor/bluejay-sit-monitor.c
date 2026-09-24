#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void stop(int signo)
{
	(void)signo;
	running = 0;
}

static void dump(const uint8_t *buf, size_t len)
{
	struct timespec ts;
	size_t i;

	clock_gettime(CLOCK_REALTIME, &ts);
	printf("%lld.%03ld RX %zu", (long long)ts.tv_sec,
	    ts.tv_nsec / 1000000, len);
	for (i = 0; i < len; i++)
		printf(" %02x", buf[i]);
	putchar('\n');
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *device = argc > 1 ? argv[1] : "/dev/umts_ipc0";
	uint8_t buf[65536];
	struct pollfd pfd;
	ssize_t count;
	int fd;

	signal(SIGINT, stop);
	signal(SIGTERM, stop);

	fd = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", device, strerror(errno));
		return 1;
	}

	fprintf(stderr, "monitoring %s read-only; no modem commands will be sent\n",
	    device);
	pfd.fd = fd;
	pfd.events = POLLIN;
	while (running) {
		int rc = poll(&pfd, 1, 1000);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}
		if (rc == 0)
			continue;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "device event: 0x%x\n", pfd.revents);
			break;
		}
		count = read(fd, buf, sizeof(buf));
		if (count > 0)
			dump(buf, (size_t)count);
		else if (count < 0 && errno != EAGAIN && errno != EINTR) {
			fprintf(stderr, "read: %s\n", strerror(errno));
			break;
		}
	}

	close(fd);
	return 0;
}
