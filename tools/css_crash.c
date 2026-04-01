/*
 * css_crash - read and decode CSS crash dump from physical memory
 *
 * when the CSS panics, platform_panic_swi saves coprocessor registers,
 * ITCM/BMP state, and ARM registers to a fixed address in CSS DRAM.
 *
 * CSS DRAM physical base: 0x4f400000 (from device tree css-dram)
 * crash dump CSS virtual:  0x023639c8
 * crash dump DRAM offset:  0x1639c8 (= 0x023639c8 - 0x02200000)
 * crash dump physical:     0x4f5639c8
 *
 * usage: css_crash [--symbols _css.elf]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define CSS_DRAM_PHYS   0x4f400000
#define CSS_DRAM_VIRT   0x02200000
#define CRASH_DUMP_VIRT 0x023639c8
#define CRASH_DUMP_OFF  (CRASH_DUMP_VIRT - CSS_DRAM_VIRT)
#define CRASH_DUMP_PHYS (CSS_DRAM_PHYS + CRASH_DUMP_OFF)

/* dump structure from platform_panic_swi:
 * [0-11]:  CP15 registers (Control, TTB0, Domain, DFSR, IFSR, FAR, ...)
 * [12+]:   ITCM copy (35 words), BMP copy (14 words), gap (4), more data
 * [end]:   saved ARM registers including panic params and LR
 */

static void *map_phys(int fd, unsigned long phys, size_t size)
{
	unsigned long page = phys & ~0xFFFUL;
	size_t mapsize = ((phys - page) + size + 0xFFF) & ~0xFFF;
	void *p = mmap(NULL, mapsize, PROT_READ, MAP_SHARED, fd, page);
	if (p == MAP_FAILED) return NULL;
	return (char *)p + (phys - page);
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 1; }

	uint32_t *dump = map_phys(fd, CRASH_DUMP_PHYS, 1024);
	if (!dump) { perror("mmap crash dump"); close(fd); return 1; }

	printf("=== CSS CRASH DUMP at phys 0x%08x ===\n\n", CRASH_DUMP_PHYS);

	/* CP15 registers */
	const char *cp15_names[] = {
		"CP15 Control", "TTB0", "Domain Access", "Data Fault Status",
		"Instr Fault Status", "Fault Address", "CP15 reg6", "CP15 reg7",
		"CP15 reg8", "Main ID", "Cache Type", "TCM Status"
	};
	printf("CP15 Registers:\n");
	for (int i = 0; i < 12; i++)
		printf("  %-20s: 0x%08x\n", cp15_names[i], dump[i]);

	printf("\n  Fault Address = 0x%08x", dump[5]);
	if (dump[5] > 0xFFFF0000)
		printf(" (near-NULL: %d)", (int32_t)dump[5]);
	printf("\n\n");

	/* scan for CSS code addresses in the full dump */
	printf("CSS code addresses in dump (potential LR/PC values):\n");
	for (int i = 0; i < 256; i++) {
		uint32_t w = dump[i];
		if (w >= 0x02000000 && w <= 0x02AFFFFF && w != 0x02000000)
			printf("  dump[%3d] (+0x%03x): 0x%08x\n", i, i*4, w);
	}

	printf("\n");

	/* look for repeated addresses (likely LR) */
	printf("Repeated addresses (likely crash location):\n");
	for (int i = 0; i < 256; i++) {
		uint32_t w = dump[i];
		if (w < 0x02000000 || w > 0x02AFFFFF) continue;
		for (int j = i+1; j < 256; j++) {
			if (dump[j] == w) {
				printf("  0x%08x (at dump[%d] and dump[%d])\n", w, i, j);
				break;
			}
		}
	}

	close(fd);
	return 0;
}
