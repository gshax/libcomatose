/*
 * shm_watch — monitor shared memory element state changes in real time
 *
 * polls specific shared memory locations at high frequency and logs
 * when values change. useful for catching transient state transitions
 * during the module readiness cascade.
 *
 * usage: shm_watch [duration_seconds]
 *   default: 10 seconds
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

#define SHM_SIZE 0x100000

struct watch_point {
	const char *name;
	uint32_t offset;
	uint32_t last_value;
};

static struct watch_point watches[] = {
	/* elem[0] codec config */
	{ "e0.cw",       0x0030, 0 },
	{ "e0.config",   0x0034, 0 },

	/* elem[6] encoder */
	{ "e6.cw",       0x0534, 0 },
	{ "e6.+4",       0x0538, 0 },
	{ "e6.+8",       0x053c, 0 },

	/* elem[21] decoder */
	{ "e21.cw",      0x0b54, 0 },
	{ "e21.+4",      0x0b58, 0 },
	{ "e21.+8",      0x0b5c, 0 },

	/* elem[22] buffer — watch for dual-buffer differentiation */
	{ "e22.cw",      0x0b80, 0 },
	{ "e22.+4",      0x0b84, 0 },
	{ "e22.+8",      0x0b88, 0 },
	{ "e22.+c",      0x0b8c, 0 },

	/* elem[12] buffer manager */
	{ "e12.cw",      0x06ec, 0 },
	{ "e12.+88",     0x0774, 0 },  /* audio flag in stock */
	{ "e12.+8c",     0x0778, 0 },  /* buffer ptr that shifts */
	{ "e12.+90",     0x077c, 0 },  /* buffer ptr that shifts */

	/* audio buffer region */
	{ "audio.0",     0xbe14, 0 },
	{ "audio.4",     0xbe18, 0 },
	{ "audio.8",     0xbe1c, 0 },
	{ "audio.20",    0xbe34, 0 },

	/* session active flag (shm+0x30 = elem[0].cw, already tracked) */

	/* ring buffer health */
	/* these offsets depend on alloc_wm — filled in at runtime */
	{ "rb.write",    0, 0 },  /* filled in at runtime */
	{ "rb.read",     0, 0 },  /* filled in at runtime */

	{ NULL, 0, 0 },
};

int main(int argc, char *argv[])
{
	int duration = argc > 1 ? atoi(argv[1]) : 10;

	int fd = open("/dev/sharedmem", O_RDONLY);
	if (fd < 0) { perror("/dev/sharedmem"); return 1; }

	uint8_t *shm = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) { perror("mmap"); return 1; }

	/* set up ring buffer watch points from saved alloc (where BGSC put
	 * the ring buffer header, at shm+0x28) */
	uint32_t rb_hdr = *(uint32_t *)(shm + 0x28);
	for (struct watch_point *w = watches; w->name; w++) {
		if (strcmp(w->name, "rb.write") == 0)
			w->offset = rb_hdr + 0x08;
		if (strcmp(w->name, "rb.read") == 0)
			w->offset = rb_hdr + 0x0c;
	}

	/* initialize last values */
	for (struct watch_point *w = watches; w->name; w++) {
		if (w->offset > 0 && w->offset < SHM_SIZE)
			w->last_value = *(volatile uint32_t *)(shm + w->offset);
	}

	fprintf(stderr, "shm_watch: monitoring %d seconds (poll ~1ms)\n", duration);
	fprintf(stderr, "shm_watch: rb_hdr=0x%x\n", rb_hdr);

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);

	int changes = 0;
	while (1) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		double elapsed = (now.tv_sec - start.tv_sec) +
		                 (now.tv_nsec - start.tv_nsec) / 1e9;
		if (elapsed >= duration)
			break;

		for (struct watch_point *w = watches; w->name; w++) {
			if (w->offset == 0 || w->offset >= SHM_SIZE)
				continue;
			uint32_t val = *(volatile uint32_t *)(shm + w->offset);
			if (val != w->last_value) {
				printf("[%7.3f] %-12s shm+0x%04x: 0x%08x -> 0x%08x\n",
				       elapsed, w->name, w->offset,
				       w->last_value, val);
				w->last_value = val;
				changes++;
			}
		}

		usleep(1000);  /* 1ms poll */
	}

	fprintf(stderr, "shm_watch: done, %d changes detected\n", changes);
	munmap(shm, SHM_SIZE);
	close(fd);
	return 0;
}
