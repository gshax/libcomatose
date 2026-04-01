/*
 * memdump - read physical memory via /dev/mem
 *
 * usage: memdump <phys_addr> [length]
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: memdump <phys_addr> [length]\n");
		return 1;
	}
	unsigned long phys = strtoul(argv[1], NULL, 0);
	int len = argc > 2 ? (int)strtol(argv[2], NULL, 0) : 256;

	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 1; }

	unsigned long page = phys & ~0xFFFUL;
	unsigned long offset = phys & 0xFFF;
	unsigned long mapsize = ((offset + len + 0xFFF) & ~0xFFF);

	void *p = mmap(NULL, mapsize, PROT_READ, MAP_SHARED, fd, page);
	if (p == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

	uint8_t *b = (uint8_t *)p + offset;
	for (int i = 0; i < len; i++) {
		if (i % 16 == 0) printf("%08lx: ", phys + i);
		printf("%02x ", b[i]);
		if (i % 16 == 15) printf("\n");
	}
	if (len % 16) printf("\n");

	munmap(p, mapsize);
	close(fd);
	return 0;
}
