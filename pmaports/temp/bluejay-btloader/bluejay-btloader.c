/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define HCI_COMMAND_PKT 0x01
#define HCI_EVENT_PKT 0x04

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

static int set_uart(int fd, speed_t speed)
{
	struct termios ti;
	if (tcgetattr(fd, &ti)) return -1;
	cfmakeraw(&ti);
	ti.c_cflag |= CLOCAL | CREAD | CRTSCTS;
	ti.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
	ti.c_cflag |= CS8;
	ti.c_cc[VMIN] = 0;
	ti.c_cc[VTIME] = 1;
	if (cfsetispeed(&ti, speed) || cfsetospeed(&ti, speed)) return -1;
	if (tcsetattr(fd, TCSANOW, &ti)) return -1;
	tcflush(fd, TCIOFLUSH);
	return 0;
}

static int write_all(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int read_exact_timeout(int fd, void *data, size_t len, int timeout_ms)
{
	uint8_t *p = data;
	while (len) {
		struct pollfd fds = { .fd = fd, .events = POLLIN };
		int rc = poll(&fds, 1, timeout_ms);
		if (rc < 0 && errno == EINTR) continue;
		if (rc <= 0) return -1;
		ssize_t n = read(fd, p, len);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int read_event(int fd, uint8_t *event, size_t size, int timeout_ms)
{
	uint8_t byte, hdr[2], payload[255];
	do {
		if (read_exact_timeout(fd, &byte, 1, timeout_ms)) return -1;
	} while (byte != HCI_EVENT_PKT);
	if (read_exact_timeout(fd, hdr, sizeof(hdr), timeout_ms)) return -1;
	if (read_exact_timeout(fd, payload, hdr[1], timeout_ms)) return -1;
	if (size >= 3) {
		size_t copy = hdr[1] < size - 3 ? hdr[1] : size - 3;
		event[0] = byte; event[1] = hdr[0]; event[2] = hdr[1];
		memcpy(event + 3, payload, copy);
	}
	return 3 + hdr[1];
}

static int wait_command_complete(int fd, uint16_t opcode, int timeout_ms)
{
	uint8_t e[260];
	for (;;) {
		int n = read_event(fd, e, sizeof(e), timeout_ms);
		if (n < 0) return -1;
		if (e[1] == 0x0e && n >= 7) {
			uint16_t got = e[4] | ((uint16_t)e[5] << 8);
			if (got == opcode) {
				if (e[6]) {
					fprintf(stderr, "opcode 0x%04x rejected: 0x%02x\n", opcode, e[6]);
					return -1;
				}
				return 0;
			}
		}
	}
}

static int command(int fd, const uint8_t *cmd, size_t len, int timeout_ms)
{
	uint16_t opcode = cmd[1] | ((uint16_t)cmd[2] << 8);
	if (write_all(fd, cmd, len)) return -1;
	return wait_command_complete(fd, opcode, timeout_ms);
}

static void drain_events(int fd)
{
	uint8_t e[260];
	for (;;) {
		struct pollfd fds = { .fd = fd, .events = POLLIN };
		if (poll(&fds, 1, 0) <= 0) break;
		if (read_event(fd, e, sizeof(e), 100) < 0) break;
	}
}

static int set_controller_baud(int fd, uint32_t baud)
{
	uint8_t cmd[] = { 1, 0x18, 0xfc, 6, 0, 0, 0, 0, 0, 0 };
	cmd[6] = baud; cmd[7] = baud >> 8; cmd[8] = baud >> 16; cmd[9] = baud >> 24;
	return command(fd, cmd, sizeof(cmd), 8000);
}

static int load_hcd(int fd, const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t packet[259];
	unsigned int records = 0;
	if (!f) return -1;

	for (;;) {
		uint8_t hdr[3];
		size_t n = fread(hdr, 1, sizeof(hdr), f);
		if (!n) break;
		if (n != sizeof(hdr)) goto fail;
		packet[0] = HCI_COMMAND_PKT;
		memcpy(packet + 1, hdr, sizeof(hdr));
		if (fread(packet + 4, 1, hdr[2], f) != hdr[2]) goto fail;

		uint16_t opcode = hdr[0] | ((uint16_t)hdr[1] << 8);
		if (opcode == 0xfc4e) {
			drain_events(fd);
			if (command(fd, packet, (size_t)hdr[2] + 4, 8000)) goto fail;
		} else {
			if (write_all(fd, packet, (size_t)hdr[2] + 4)) goto fail;
			/* Google's HAL has an asynchronous reader during no-ACK writes. */
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			if (poll(&pfd, 1, 2) > 0) drain_events(fd);
		}
		records++;
		if (!(records % 256)) fprintf(stderr, "firmware records: %u\n", records);
	}
	fclose(f);
	fprintf(stderr, "firmware records complete: %u\n", records);
	return 0;
fail:
	fclose(f);
	return -1;
}

static int set_bdaddr(int fd, const char *text)
{
	unsigned int a[6];
	uint8_t cmd[] = { 1, 0x01, 0xfc, 6, 0, 0, 0, 0, 0, 0 };
	if (sscanf(text, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++) cmd[4 + i] = (uint8_t)a[5 - i];
	return command(fd, cmd, sizeof(cmd), 8000);
}

int main(int argc, char **argv)
{
	const char *tty;
	const char *fw;
	const char *addr;
	const uint8_t reset[] = { 1, 0x03, 0x0c, 0 };
	const uint8_t minidrv[] = { 1, 0x2e, 0xfc, 0 };
	const uint8_t version[] = { 1, 0x14, 0x0c, 0 };
	int fd;

	if (argc != 4) {
		fprintf(stderr, "usage: %s TTY FIRMWARE BDADDR\n", argv[0]);
		return 2;
	}
	tty = argv[1];
	fw = argv[2];
	addr = argv[3];

	fd = open(tty, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) { perror(tty); return 1; }

	fprintf(stderr, "reset at 115200\n");
	if (set_uart(fd, B115200) || command(fd, reset, sizeof(reset), 8000)) goto fail;
	fprintf(stderr, "switch controller to 3000000\n");
	if (set_controller_baud(fd, 3000000) || set_uart(fd, B3000000)) goto fail;
	fprintf(stderr, "enter MiniDrv\n");
	if (command(fd, minidrv, sizeof(minidrv), 8000)) goto fail;
	msleep(100);
	fprintf(stderr, "stream %s\n", fw);
	if (load_hcd(fd, fw)) goto fail;
	msleep(100);
	fprintf(stderr, "firmware launched; return host UART to 115200\n");
	if (set_uart(fd, B115200)) goto fail;
	fprintf(stderr, "final reset at 115200\n");
	if (command(fd, reset, sizeof(reset), 8000)) goto fail;
	fprintf(stderr, "switch running firmware to 3000000\n");
	if (set_controller_baud(fd, 3000000) || set_uart(fd, B3000000)) goto fail;
	fprintf(stderr, "verify firmware\n");
	if (command(fd, version, sizeof(version), 8000)) goto fail;
	fprintf(stderr, "program address %s\n", addr);
	if (set_bdaddr(fd, addr)) goto fail;
	fprintf(stderr, "Bluejay BCM4389 provisioning complete\n");
	close(fd);
	return 0;
fail:
	perror("Bluetooth provisioning failed");
	close(fd);
	return 1;
}
