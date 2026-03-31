/*
 * shmdump - dump CSS shared memory regions
 *
 * usage: shmdump [offset] [length]
 *   offset: byte offset into shared memory (hex or decimal, default 0)
 *   length: bytes to dump (default 256)
 *   --bin:  output raw binary to stdout instead of hex dump to stdout
 *
 * hex dump goes to stdout (for piping/diffing), mmap info to stderr.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>

#define SHAREDMEM_IOCTL_INIT 0xc0045302

int main(int argc, char **argv)
{
	int off = 0, len = 256, bin = 0;
	int posarg = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bin") == 0) {
			bin = 1;
		} else if (argv[i][0] != '-' || (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9')) {
			if (posarg == 0)
				off = (int)strtol(argv[i], NULL, 0);
			else
				len = (int)strtol(argv[i], NULL, 0);
			posarg++;
		}
	}

	int fd = open("/dev/sharedmem", O_RDWR);
	if (fd < 0) { perror("open /dev/sharedmem"); return 1; }

	uint32_t ioc[4] = {0};
	if (ioctl(fd, SHAREDMEM_IOCTL_INIT, &ioc) < 0) {
		perror("ioctl");
		close(fd);
		return 1;
	}

	unsigned int mapsize = ioc[1] ? ioc[1] : 0x80000;
	fprintf(stderr, "shm: phys=0x%x size=0x%x\n", ioc[0], mapsize);

	void *p = mmap(NULL, mapsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

	if (off + len > (int)mapsize)
		len = (int)mapsize - off;

	uint8_t *b = (uint8_t *)p + off;

	if (bin) {
		fwrite(b, 1, (size_t)len, stdout);
	} else {
		for (int i = 0; i < len; i++) {
			if (i % 16 == 0) printf("%06x: ", off + i);
			printf("%02x ", b[i]);
			if (i % 16 == 15) printf("\n");
		}
		if (len % 16) printf("\n");
	}

	munmap(p, mapsize);
	close(fd);
	return 0;
}
