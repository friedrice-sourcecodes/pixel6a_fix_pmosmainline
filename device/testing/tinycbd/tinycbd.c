#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IOCTL_MAGIC 'o'

struct cp_image {
	unsigned long long binary;
	uint32_t size;
	uint32_t m_offset;
	uint32_t b_offset;
	uint32_t mode;
	uint32_t len;
} __attribute__((packed));

enum cp_boot_mode {
	CP_BOOT_MODE_NORMAL = 0,
	CP_BOOT_MODE_DUMP = 1,
	CP_BOOT_RE_INIT = 2,
	CP_BOOT_MODE_SILENT = 3,
	CP_BOOT_REQ_CP_RAM_LOGGING = 5,
	CP_BOOT_MODE_MANUAL = 7,
	CP_BOOT_EXT_BAAW = 11,
};

struct boot_mode {
	enum cp_boot_mode idx;
};

struct modem_sec_req {
	uint32_t mode;
	uint32_t param2;
	uint32_t param3;
	uint32_t param4;
} __attribute__((packed));

#define IOCTL_POWER_ON			_IO(IOCTL_MAGIC, 0x19)
#define IOCTL_POWER_OFF			_IO(IOCTL_MAGIC, 0x20)
#define IOCTL_START_CP_BOOTLOADER	_IOW(IOCTL_MAGIC, 0x22, struct boot_mode)
#define IOCTL_COMPLETE_NORMAL_BOOTUP	_IO(IOCTL_MAGIC, 0x23)
#define IOCTL_GET_CP_STATUS		_IO(IOCTL_MAGIC, 0x27)
#define IOCTL_LOAD_CP_IMAGE		_IOW(IOCTL_MAGIC, 0x40, struct cp_image)
#define IOCTL_REQ_SECURITY		_IOW(IOCTL_MAGIC, 0x53, struct modem_sec_req)
#define IOCTL_SET_SPI_BOOT_MODE		_IO(IOCTL_MAGIC, 0x58)

#define TOC_ENTRY_SIZE	32
#define TOC_MAX_ENTRIES	16
#define SPI_MAX_CHUNK	(128 * 1024)
#define UDL_MAX_CHUNK	(48 * 1024)

struct toc_entry {
	char name[13];
	uint32_t offset;
	uint32_t load_addr;
	uint32_t size;
	uint32_t crc;
	uint32_t id;
};

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int toc_read(const char *path, struct toc_entry *toc, int max)
{
	uint8_t raw[TOC_ENTRY_SIZE * TOC_MAX_ENTRIES];
	FILE *f;
	int n = 0;
	int i;

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) {
		fprintf(stderr, "%s: short read of TOC\n", path);
		fclose(f);
		return -1;
	}
	fclose(f);

	if (memcmp(raw, "TOC", 3) != 0) {
		fprintf(stderr, "%s: no TOC magic\n", path);
		return -1;
	}

	for (i = 0; i < max && i < TOC_MAX_ENTRIES; i++) {
		const uint8_t *e = raw + i * TOC_ENTRY_SIZE;

		if (e[0] == 0)
			break;

		memcpy(toc[n].name, e, 12);
		toc[n].name[12] = 0;
		toc[n].offset = rd32(e + 12);
		toc[n].load_addr = rd32(e + 16);
		toc[n].size = rd32(e + 20);
		toc[n].crc = rd32(e + 24);
		toc[n].id = rd32(e + 28);
		n++;
	}

	return n;
}

static void toc_print(const struct toc_entry *toc, int n)
{
	int i;

	printf("%-10s %10s %12s %12s %10s %4s\n",
	       "name", "offset", "load_addr", "size", "crc", "id");
	for (i = 0; i < n; i++)
		printf("%-10s %10u   0x%08x %12u 0x%08x %4u\n",
		       toc[i].name, toc[i].offset, toc[i].load_addr,
		       toc[i].size, toc[i].crc, toc[i].id);
}

static const struct toc_entry *toc_find(const struct toc_entry *toc, int n,
					const char *name)
{
	int i;

	for (i = 0; i < n; i++)
		if (strcmp(toc[i].name, name) == 0)
			return &toc[i];

	return NULL;
}

/*
 * NV_NORM, NV_PROT and REPLAY are listed in the TOC with offset 0: they carry
 * no payload in modem.bin, only a load address. The data lives on the device:
 *
 *   NV_NORM  0x50000010  efs            nv_normal.bin
 *   NV_PROT  0x50100000  efs            nv_protected.bin
 *   REPLAY   0x50280000  modem_userdata replay_region.bin
 *
 * All three are 512 KiB, matching the sizes in the TOC.
 */
static const struct {
	const char *section;
	const char *file;
} nv_sources[] = {
	{ "NV_NORM", "nv_normal.bin" },
	{ "NV_PROT", "nv_protected.bin" },
	{ "REPLAY",  "replay_region.bin" },
};

static const char *nv_file_for(const char *section)
{
	size_t i;

	for (i = 0; i < sizeof(nv_sources) / sizeof(nv_sources[0]); i++)
		if (strcmp(nv_sources[i].section, section) == 0)
			return nv_sources[i].file;

	return NULL;
}

static int cp_status(int fd, int quiet)
{
	int st = ioctl(fd, IOCTL_GET_CP_STATUS);

	if (st < 0) {
		if (!quiet)
			fprintf(stderr, "IOCTL_GET_CP_STATUS: %s\n", strerror(errno));
		return -1;
	}
	if (!quiet)
		printf("cp status: %d\n", st);
	return st;
}

static int wait_for_status(int fd, int wanted, unsigned int timeout_sec)
{
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 200000000 };
	unsigned int attempts = timeout_sec * 5;
	unsigned int i;

	for (i = 0; i < attempts; i++) {
		int st = cp_status(fd, 1);

		if (st == wanted)
			return 0;
		if (st == 1 || st == 2 || st == 8) {
			fprintf(stderr, "CP entered crash state %d\n", st);
			return -1;
		}
		nanosleep(&delay, NULL);
	}

	fprintf(stderr, "timed out waiting for CP state %d\n", wanted);
	return -1;
}

static int udl_req_resp(int fd, uint32_t request, uint32_t expected)
{
	uint32_t response = 0;
	ssize_t n;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	n = write(fd, &request, sizeof(request));
	if (n != sizeof(request)) {
		fprintf(stderr, "UDL write 0x%08x: %s\n", request,
			n < 0 ? strerror(errno) : "short write");
		return -1;
	}
	if (poll(&pfd, 1, 10000) <= 0) {
		fprintf(stderr, "UDL timeout after 0x%08x\n", request);
		return -1;
	}
	n = read(fd, &response, sizeof(response));
	if (n != sizeof(response)) {
		fprintf(stderr, "UDL read after 0x%08x: %s\n", request,
			n < 0 ? strerror(errno) : "short read");
		return -1;
	}
	if (response != expected) {
		fprintf(stderr, "UDL response 0x%08x, expected 0x%08x\n",
			response, expected);
		return -1;
	}
	return 0;
}

struct udl_frame {
	uint16_t command;
	uint16_t length;
	uint32_t total_size;
	uint32_t offset;
	uint8_t data[];
} __attribute__((packed));

static int send_udl_stage(int fd, const char *image, const char *nvdir,
			  const struct toc_entry *e, int start_stage,
			  int end_stage, int final_stage)
{
	const char *nvfile = nv_file_for(e->name);
	struct udl_frame *frame;
	uint32_t sent = 0;
	uint32_t stage = e->id;
	uint32_t request, response;
	char path[512];
	FILE *f;

	if (nvfile) {
		snprintf(path, sizeof(path), "%s/%s", nvdir, nvfile);
		image = path;
	}
	f = fopen(image, "rb");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", image, strerror(errno));
		return -1;
	}
	if (!nvfile && fseek(f, e->offset, SEEK_SET) != 0) {
		fprintf(stderr, "seek %s: %s\n", e->name, strerror(errno));
		fclose(f);
		return -1;
	}

	if (start_stage) {
		request = 0xa100 | (stage << 4);
		response = 0xc100 | (stage << 4);
		if (udl_req_resp(fd, request, response) != 0)
			goto fail;
	}

	frame = malloc(sizeof(*frame) + UDL_MAX_CHUNK);
	if (!frame)
		goto fail;
	while (sent < e->size) {
		uint32_t chunk = e->size - sent;
		ssize_t n;

		if (chunk > UDL_MAX_CHUNK)
			chunk = UDL_MAX_CHUNK;
		if (fread(frame->data, 1, chunk, f) != chunk) {
			fprintf(stderr, "%s: short read at %u\n", e->name, sent);
			free(frame);
			goto fail;
		}
		frame->command = (uint16_t)(0xa10b | (stage << 4));
		frame->length = (uint16_t)(chunk + 8);
		frame->total_size = e->size;
		frame->offset = sent;
		n = write(fd, frame, sizeof(*frame) + chunk);
		if (n < 0) {
			fprintf(stderr, "%s UDL data at %u: %s\n",
				e->name, sent, strerror(errno));
			free(frame);
			goto fail;
		}
		request = 0;
		{
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			if (poll(&pfd, 1, 10000) <= 0) {
				fprintf(stderr, "%s UDL data ack timeout at %u\n",
					e->name, sent);
				free(frame);
				goto fail;
			}
		}
		n = read(fd, &request, sizeof(request));
		response = 0xc10b | (stage << 4);
		if (n != sizeof(request) || request != response) {
			fprintf(stderr, "%s UDL ack 0x%08x, expected 0x%08x\n",
				e->name, request, response);
			free(frame);
			goto fail;
		}
		sent += chunk;
		printf("\r%s: %u/%u", e->name, sent, e->size);
		fflush(stdout);
	}
	free(frame);
	printf("\n");
	if (e->crc) {
		uint32_t crc_request[2] = {
			0xa301 | (stage << 4), e->crc
		};
		ssize_t n = write(fd, crc_request, sizeof(crc_request));

		if (n != sizeof(crc_request)) {
			fprintf(stderr, "%s UDL CRC write: %s\n", e->name,
				n < 0 ? strerror(errno) : "short write");
			goto fail;
		}
		request = 0;
		{
			struct pollfd pfd = { .fd = fd, .events = POLLIN };
			if (poll(&pfd, 1, 10000) <= 0) {
				fprintf(stderr, "%s UDL CRC ack timeout\n", e->name);
				goto fail;
			}
		}
		n = read(fd, &request, sizeof(request));
		response = 0xc300 | (stage << 4);
		if (n != sizeof(request) || request != response) {
			fprintf(stderr, "%s UDL CRC ack 0x%08x, expected 0x%08x\n",
				e->name, request, response);
			goto fail;
		}
	}

	if (end_stage) {
		request = (final_stage ? 0xa10d : 0xa00b) | (stage << 4);
		response = (final_stage ? 0xc10d : 0xc00b) | (stage << 4);
		if (udl_req_resp(fd, request, response) != 0)
			goto fail;
	}
	fclose(f);
	return 0;

fail:
	fclose(f);
	return -1;
}

static int send_section(int fd, const char *path, const struct toc_entry *e)
{
	uint8_t *buf;
	FILE *f;
	uint32_t sent = 0;

	if (e->size == 0) {
		fprintf(stderr, "%s: zero size\n", e->name);
		return -1;
	}

	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fseek(f, e->offset, SEEK_SET) != 0) {
		fprintf(stderr, "seek to %u: %s\n", e->offset, strerror(errno));
		fclose(f);
		return -1;
	}

	buf = malloc(SPI_MAX_CHUNK);
	if (!buf) {
		fclose(f);
		return -1;
	}

	while (sent < e->size) {
		uint32_t chunk = e->size - sent;
		struct cp_image img;

		if (chunk > SPI_MAX_CHUNK)
			chunk = SPI_MAX_CHUNK;

		if (fread(buf, 1, chunk, f) != chunk) {
			fprintf(stderr, "%s: short read at %u\n", e->name, sent);
			free(buf);
			fclose(f);
			return -1;
		}

		memset(&img, 0, sizeof(img));
		img.binary = (unsigned long long)(uintptr_t)buf;
		img.size = chunk;
		img.m_offset = e->load_addr + sent;
		img.b_offset = e->offset + sent;
		img.len = chunk;

		if (ioctl(fd, IOCTL_LOAD_CP_IMAGE, &img) < 0) {
			fprintf(stderr, "IOCTL_LOAD_CP_IMAGE at %u: %s\n",
				sent, strerror(errno));
			free(buf);
			fclose(f);
			return -1;
		}

		sent += chunk;
		printf("\r%s: %u/%u", e->name, sent, e->size);
		fflush(stdout);
	}

	printf("\n");
	free(buf);
	fclose(f);

	return 0;
}

/*
 * Send one TOC section, taking NV data from nvdir rather than from the image.
 * A section with offset 0 that is not one of those has nothing to send, and
 * reading the image at offset 0 would upload the TOC and BOOT instead.
 */
static int send_toc_section(int fd, const char *image, const char *nvdir,
			    const struct toc_entry *e)
{
	const char *nvfile = nv_file_for(e->name);
	struct toc_entry nv;
	struct stat st;
	char path[512];

	if (!nvfile) {
		if (e->offset == 0) {
			fprintf(stderr,
				"%s: offset 0 and no NV source, refusing\n",
				e->name);
			return -1;
		}
		return send_section(fd, image, e);
	}

	snprintf(path, sizeof(path), "%s/%s", nvdir, nvfile);
	if (stat(path, &st) != 0) {
		fprintf(stderr, "%s: %s: %s\n", e->name, path, strerror(errno));
		return -1;
	}
	if ((uint32_t)st.st_size != e->size) {
		fprintf(stderr, "%s: %s is %lld bytes, TOC says %u\n",
			e->name, path, (long long)st.st_size, e->size);
		return -1;
	}

	nv = *e;
	nv.offset = 0;

	printf("%s: from %s\n", e->name, path);

	return send_section(fd, path, &nv);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-d node] [-i modem.bin] [-n nvdir] <command>\n"
		"\n"
		"  toc            list the sections in the image\n"
		"  boot           upload BOOT and start the CP bootloader\n"
		"  boot-all       boot CP, upload MAIN/VSS/APM/NV/REPLAY, finish handshake\n"
		"  upload <NAME>  upload one section by name\n"
		"  status         query CP status\n"
		"  poweron        IOCTL_POWER_ON\n"
		"  poweroff       IOCTL_POWER_OFF\n"
		"\n"
		"NV_NORM, NV_PROT and REPLAY come from nvdir, not from the image:\n"
		"  nv_normal.bin and nv_protected.bin from the efs partition,\n"
		"  replay_region.bin from modem_userdata.\n"
		"\n"
		"defaults: -d /dev/umts_boot0  -i ./modem.bin\n"
		"          -n /mnt/nv\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *node = "/dev/umts_boot0";
	const char *image = "./modem.bin";
	const char *nvdir = "/mnt/nv";
	struct toc_entry toc[TOC_MAX_ENTRIES];
	const char *cmd;
	int ntoc;
	int fd;
	int opt;
	int rc = 1;

	while ((opt = getopt(argc, argv, "d:i:n:h")) != -1) {
		switch (opt) {
		case 'd':
			node = optarg;
			break;
		case 'i':
			image = optarg;
			break;
		case 'n':
			nvdir = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return 1;
	}
	cmd = argv[optind];

	ntoc = toc_read(image, toc, TOC_MAX_ENTRIES);
	if (ntoc < 0)
		return 1;

	if (strcmp(cmd, "toc") == 0) {
		toc_print(toc, ntoc);
		return 0;
	}

	fd = open(node, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", node, strerror(errno));
		return 1;
	}

	if (strcmp(cmd, "status") == 0) {
		int st = cp_status(fd, 0);
		rc = (st < 0);
	} else if (strcmp(cmd, "poweron") == 0) {
		rc = ioctl(fd, IOCTL_POWER_ON) < 0;
		if (rc)
			fprintf(stderr, "IOCTL_POWER_ON: %s\n", strerror(errno));
	} else if (strcmp(cmd, "poweroff") == 0) {
		rc = ioctl(fd, IOCTL_POWER_OFF) < 0;
		if (rc)
			fprintf(stderr, "IOCTL_POWER_OFF: %s\n", strerror(errno));
	} else if (strcmp(cmd, "upload") == 0) {
		const struct toc_entry *e;

		if (optind + 1 >= argc) {
			usage(argv[0]);
			goto out;
		}
		e = toc_find(toc, ntoc, argv[optind + 1]);
		if (!e) {
			fprintf(stderr, "no section named %s\n", argv[optind + 1]);
			goto out;
		}
		rc = send_toc_section(fd, image, nvdir, e) != 0;
	} else if (strcmp(cmd, "boot") == 0 || strcmp(cmd, "boot-all") == 0) {
		const struct toc_entry *e = toc_find(toc, ntoc, "BOOT");
		struct boot_mode mode = { .idx = CP_BOOT_MODE_NORMAL };
		static const char *const stages[] = {
			"MAIN", "VSS", "APM", "NV_NORM", "NV_PROT", "REPLAY",
			"GVERSION"
		};
		size_t i;

		if (!e) {
			fprintf(stderr, "no BOOT section in %s\n", image);
			goto out;
		}

		if (ioctl(fd, IOCTL_SET_SPI_BOOT_MODE) < 0 && errno != ENOTTY) {
			fprintf(stderr, "IOCTL_SET_SPI_BOOT_MODE: %s\n", strerror(errno));
			goto out;
		}

		if (ioctl(fd, IOCTL_POWER_ON) < 0) {
			fprintf(stderr, "IOCTL_POWER_ON: %s\n", strerror(errno));
			goto out;
		}
		printf("cp powered on\n");

		if (send_section(fd, image, e) != 0)
			goto out;

		if (ioctl(fd, IOCTL_START_CP_BOOTLOADER, &mode) < 0) {
			fprintf(stderr, "IOCTL_START_CP_BOOTLOADER: %s\n",
				strerror(errno));
			goto out;
		}
		printf("bootloader started\n");

		if (strcmp(cmd, "boot") == 0) {
			rc = 0;
			goto out;
		}

		for (i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
			e = toc_find(toc, ntoc, stages[i]);
			if (!e) {
				fprintf(stderr, "required section %s is missing\n", stages[i]);
				goto out;
			}
			printf("uploading stage %s\n", stages[i]);
			if (send_udl_stage(fd, image, nvdir, e,
					i == 0 || toc_find(toc, ntoc, stages[i - 1])->id != e->id,
					i + 1 == sizeof(stages) / sizeof(stages[0]) ||
						toc_find(toc, ntoc, stages[i + 1])->id != e->id,
					i + 1 == sizeof(stages) / sizeof(stages[0])) != 0)
				goto out;
		}

		printf("waiting for CP initialization handshake\n");
		if (ioctl(fd, IOCTL_COMPLETE_NORMAL_BOOTUP) < 0) {
			fprintf(stderr, "IOCTL_COMPLETE_NORMAL_BOOTUP: %s\n",
				strerror(errno));
			goto out;
		}
		if (wait_for_status(fd, 4, 10) != 0)
			goto out;
		printf("CP is online\n");
		rc = 0;
	} else {
		usage(argv[0]);
	}

out:
	close(fd);
	return rc;
}
