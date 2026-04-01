/*
 * shm_decode — decode CSS shared memory element descriptors
 *
 * reads /dev/sharedmem (live) or a binary dump file and decodes the
 * element descriptor table, annotating fields as:
 *   [CSS] = CSS-allocated pointer (shm range)
 *   [ARM] = ARM process-local address (written by dfl_module_startup)
 *   [CFG] = configuration constant
 *   [CW]  = control word
 *
 * usage: shm_decode [dump_file]
 *   no args: reads live /dev/sharedmem
 *   with arg: reads raw binary dump (1MB)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define SHM_SIZE       0x100000
#define ELEM_TABLE_OFF 0xb854
#define ELEM_TABLE_MAX 368
#define ELEMS_PER_GROUP 24

/* classify a uint32 value based on its range */
static const char *classify_u32(uint32_t val, uint32_t shm_base)
{
	if (val == 0) return NULL;
	if (shm_base != 0 && val >= shm_base && val < shm_base + SHM_SIZE)
		return "CSS"; /* shm pointer */
	if (val >= 0x00010000 && val < 0x00200000)
		return "ARM"; /* process-local address (typical BSS/data range) */
	return "CFG"; /* configuration value */
}

static void dump_element(const uint8_t *shm, uint32_t elem_off,
                         int elem_idx, int group, uint32_t shm_base,
                         uint32_t next_off)
{
	uint32_t size = next_off - elem_off;
	if (size > 0x200) size = 0x200; /* cap dump size */

	const uint32_t *w = (const uint32_t *)(shm + elem_off);
	int nwords = size / 4;

	printf("  elem[%d] (g%d) shm+0x%04x (%d bytes):\n",
	       elem_idx, group, elem_off, size);

	/* control word */
	printf("    +0x%03x: 0x%08x  [CW] %s\n", 0, w[0],
	       w[0] == 0 ? "INACTIVE" :
	       (w[0] == 1 ? "ACTIVE" : "PENDING"));

	/* remaining fields */
	int arm_count = 0, css_count = 0, cfg_count = 0;
	for (int i = 1; i < nwords && i < 64; i++) {
		uint32_t val = w[i];
		const char *cls = classify_u32(val, shm_base);
		if (!cls) continue; /* skip zeros */

		uint32_t off = i * 4;
		if (strcmp(cls, "ARM") == 0) {
			printf("    +0x%03x: 0x%08x  [ARM] process addr\n",
			       off, val);
			arm_count++;
		} else if (strcmp(cls, "CSS") == 0) {
			printf("    +0x%03x: 0x%08x  [CSS] shm+0x%05x\n",
			       off, val, val - shm_base);
			css_count++;
		} else {
			/* print config values, try to show uint16 pairs */
			uint16_t lo = val & 0xffff;
			uint16_t hi = val >> 16;
			if (hi == 0)
				printf("    +0x%03x: 0x%08x  [CFG] %u\n",
				       off, val, val);
			else
				printf("    +0x%03x: 0x%08x  [CFG] u16: %u, %u\n",
				       off, val, lo, hi);
		}
	}
	printf("    summary: %d ARM addrs, %d CSS ptrs, %d config values\n",
	       arm_count, css_count, cfg_count);
}

int main(int argc, char *argv[])
{
	uint8_t *shm = NULL;
	int from_file = 0;

	if (argc > 1) {
		/* read from binary dump file */
		FILE *f = fopen(argv[1], "rb");
		if (!f) {
			/* try hex dump format */
			perror(argv[1]);
			return 1;
		}
		shm = malloc(SHM_SIZE);
		if (!shm) return 1;
		size_t n = fread(shm, 1, SHM_SIZE, f);
		fclose(f);
		if (n < SHM_SIZE) {
			fprintf(stderr, "warning: only read %zu bytes "
			        "(expected %d)\n", n, SHM_SIZE);
		}
		from_file = 1;
	} else {
		/* mmap live shared memory */
		int fd = open("/dev/sharedmem", O_RDONLY);
		if (fd < 0) { perror("/dev/sharedmem"); return 1; }
		shm = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
		if (shm == MAP_FAILED) { perror("mmap"); return 1; }
	}

	/* header */
	uint32_t *hdr = (uint32_t *)shm;
	printf("=== shared memory header ===\n");
	printf("  pool_size:   0x%08x (%u KB)\n", hdr[1], hdr[1]/1024);
	printf("  alloc_wm:    0x%08x\n", hdr[2]);
	printf("  dsp_version: 0x%08x\n", hdr[4]);
	printf("  version_tag: 0x%08x\n", hdr[5]);
	printf("  +0x18:       0x%08x\n", hdr[6]);
	printf("  +0x1c:       0x%08x\n", hdr[7]);
	printf("  +0x20:       0x%08x\n", hdr[8]);
	printf("  +0x24:       0x%08x (codec state ref)\n", hdr[9]);
	printf("  saved_alloc: 0x%08x\n", hdr[10]);
	printf("  codec_state: 0x%08x\n", hdr[11]);

	/* detect shm_base from element table */
	uint32_t first_elem_ptr = *(uint32_t *)(shm + ELEM_TABLE_OFF);
	uint32_t shm_base = 0;
	if (first_elem_ptr != 0) {
		/* elem[0] is at shm+0x30, so base = ptr - 0x30 */
		shm_base = first_elem_ptr - 0x30;
		printf("\n  shm mmap base: 0x%08x (from elem table)\n",
		       shm_base);
	} else {
		printf("\n  element table is empty!\n");
		if (!from_file) { munmap(shm, SHM_SIZE); }
		else { free(shm); }
		return 0;
	}

	/* read element table and compute offsets */
	printf("\n=== element descriptors ===\n");
	uint32_t elem_offs[ELEM_TABLE_MAX];
	int num_elems = 0;
	for (int i = 0; i < ELEM_TABLE_MAX; i++) {
		uint32_t ptr = *(uint32_t *)(shm + ELEM_TABLE_OFF + i * 4);
		if (ptr == 0) break;
		elem_offs[num_elems] = ptr - shm_base;
		num_elems++;
	}
	printf("  %d elements in %d groups\n\n",
	       num_elems, (num_elems + ELEMS_PER_GROUP - 1) / ELEMS_PER_GROUP);

	/* dump group 0 (first 24 elements) */
	int dump_count = num_elems < ELEMS_PER_GROUP ? num_elems : ELEMS_PER_GROUP;
	printf("--- group 0 ---\n");
	for (int i = 0; i < dump_count; i++) {
		uint32_t next_off;
		if (i + 1 < dump_count)
			next_off = elem_offs[i + 1];
		else
			next_off = elem_offs[i] + 0x80; /* estimate */
		dump_element(shm, elem_offs[i], i, 0, shm_base, next_off);
		printf("\n");
	}

	if (!from_file) munmap(shm, SHM_SIZE);
	else free(shm);
	return 0;
}
