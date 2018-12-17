#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MIN(a,b) ((a) < (b) ? a : b)

#undef DEBUG
#ifdef DEBUG
#define pr_debug printf
#else
#define pr_debug(...) do { } while(0)
#endif

void usage(char *prog_name)
{
    printf("%s: [generate|cont_after_mismatch] mem_dump_a mem_dump_b bmap_dump mem_addr mem_size [bmap_granularity (4K default) [bmap_start]]\n",
           prog_name);
}

static bool generate_pg_dirty(uint8_t *a, uint8_t *b, uint64_t pgsz)
{
    int i;

    for (i = 0; i < pgsz; i++) {
	    if (a[i] != b[i])
		    return true;
    }

    return false;
}

void generate_bitmap(uint8_t *a, uint8_t *b, uint8_t *bm,
                     uint64_t maddr, uint64_t msize, uint64_t pgsz)
{
    uint64_t mpos;

    for (mpos = 0; mpos < msize; mpos += pgsz) {
	int64_t pgnum = mpos / pgsz;
	bool pg_dirty;

	pg_dirty = generate_pg_dirty(&a[mpos], &b[mpos], pgsz);

	bm[pgnum / 8] &= ~(1UL << (pgnum % 8));
	bm[pgnum / 8] |= (!!pg_dirty) << (pgnum % 8);
    }
}

static bool check_page(uint8_t *a, uint8_t *b, bool bm,
			uint64_t pgsz, uint64_t addr, uint64_t *dirty_count)
{
    int i;
    bool page_dirty = false;

    pr_debug("Page at 0x%lx expected to be %s\n", addr, bm ? "dirty" : "clean");

    page_dirty = !!memcmp(a, b, pgsz);
    if (!page_dirty) {
        if (bm)
            pr_debug("page at addr 0x%lx is clean, but bitmap is dirty\n", addr);
        return false;
    } else if (page_dirty && bm) {
        /* page is dirty */
        *dirty_count += 1;
        return false;
    }

    /* Page dirty and !bm */
    for (i = 0; i < pgsz; i++) {
	if (a[i] != b[i]) {
		/* page is dirty */
		*dirty_count += 1;

                printf("page at addr 0x%lx (0x%lx) is dirty, but bitmap is clean\n",
                        addr, addr + i);
                printf("a 0x%x b 0x%x bm %d\n", a[i], b[i], bm);
                return true;
	}
    }
}

bool check_bitmap(void *a, void *b, void *bm,
                  uint64_t maddr, uint64_t msize, uint64_t pgsz, bool exit_on_mismatch)
{
    uint64_t mpos, dirty_count = 0;
    bool mismatch = false;

    for (mpos = 0; (mpos < msize) && (!mismatch || !exit_on_mismatch);
		       mpos += pgsz) {
	uint64_t count = MIN(pgsz, (msize - mpos));
	int64_t ret, pg_num;
        bool pg_mismatch;
	uint8_t bmap_byte;
	bool bmap_bit;

	pg_num = mpos / pgsz;
	bmap_byte = ((uint8_t *) bm)[pg_num / 8];
	bmap_bit = !!(bmap_byte & (1UL << (pg_num % 8)));

	/* Check for mismatches */
        pg_mismatch = check_page(&(((uint8_t *) a)[mpos]), &(((uint8_t *) b)[mpos]),
				bmap_bit, count, mpos, &dirty_count);
        if (pg_mismatch) {
            mismatch = true;
            printf("inconsistent dirty bitmap within 0x%lx to 0x%lx\n", mpos, mpos + pgsz);
	    pr_debug("pg_num %lu, bmap_byte %lu (%x), bmap_bit %u (%d)\n",
			    pg_num, pg_num / 8, bmap_byte,
			    pg_num % 8, bmap_bit);
        }
    }

    if (!mismatch)
        printf("no inconsistencies found. found %lu dirty pages.\n", dirty_count);
    else
	printf("!!!mismatch!!!. found %lu dirty pages.\n", dirty_count);

    return mismatch;
}

int main(int argc, char **argv)
{
    const char *a_path, *b_path, *bm_path;
    uint64_t maddr, msize, pgsz;
    int a_fd, b_fd, bm_fd, ret;
    void *a, *b, *bm;
    bool generate = false;
    bool exit_on_mismatch = true, mismatch = false;

    if (argc < 6) {
        usage(argv[0]);
        exit(1);
    }

    if (strcmp(argv[1], "generate") == 0) {
        generate = true;
        argv = &argv[1];
        argc--;
    } else if (strcmp(argv[1], "cont_after_mismatch") == 0) {
        exit_on_mismatch = false;
        argv = &argv[1];
        argc--;
    }

    a_path = argv[1];
    b_path = argv[2];
    bm_path = argv[3];
    maddr = atol(argv[4]);
    msize = atol(argv[5]);

    if (argc > 6)
        pgsz = atol(argv[6]);
    else
        pgsz = 4096; /* This is hard coded in qemu */

    if (argc > 7)
        error(1, 0, "just use all the same starting offsets for now");

    /* Open input files */
    a_fd = open(a_path, O_RDONLY);
    if (a_fd < 0)
        error(1, errno, "failed to open %s", a_path);
    b_fd = open(b_path, O_RDONLY);
    if (b_fd < 0)
        error(1, errno, "failed to open %s", b_path);
    bm_fd = open(bm_path, generate ? O_RDWR : O_RDONLY);
    if (bm_fd < 0)
        error(1, errno, "failed to open %s", bm_path);

    /* mmap input files */
    a = mmap(NULL, msize, PROT_READ, MAP_SHARED, a_fd, maddr);
    if (a < 0)
	    error(1, errno, "failed to mmap %s", a_path);
    b = mmap(NULL, msize - maddr, PROT_READ, MAP_SHARED, b_fd, maddr);
    if (b < 0)
	    error(1, errno, "failed to mmap %s", b_path);
    bm = mmap(NULL, msize / (pgsz * 8), generate ? PROT_WRITE | PROT_READ : PROT_READ
		  , MAP_SHARED, bm_fd, (maddr / (pgsz * 8)));
    if (bm < 0)
	    error(1, errno, "failed to mmap %s", bm_path);

    if (generate) {
        generate_bitmap((uint8_t *) a, (uint8_t *) b, (uint8_t *) bm, maddr, msize, pgsz);
    } else {
	pr_debug("Checking for mismatches from 0x%lx->0x%lx\n", maddr, maddr + msize);
        mismatch = check_bitmap(a, b, bm, maddr, msize, pgsz, exit_on_mismatch);
    }

    ret = munmap(a, msize);
    if (ret)
	    error(1, errno, "failed to munmap %s\n", a_path);
    ret = munmap(b, msize);
    if (ret)
	    error(1, errno, "failed to munmap %s\n", b_path);
    ret = munmap(bm, msize / (pgsz * 8));
    if (ret)
	    error(1, errno, "failed to munmap %s\n", bm_path);
    close(a_fd);
    close(b_fd);
    close(bm_fd);

    return (mismatch) ? 1 : 0;
}
