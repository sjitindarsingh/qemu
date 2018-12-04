#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <error.h>
#include <string.h>

#define MIN(a,b) ((a) < (b) ? a : b)
#define MBUF_SZ (4 << 20)

void usage(char *prog_name)
{
    printf("%s: [generate|cont_after_mismatch] mem_dump_a mem_dump_b bmap_dump mem_addr mem_size [bmap_granularity (4K default) [bmap_start]]\n",
           prog_name);
}

static uint64_t get_shift(uint64_t pgsz)
{
    int page_shift = 0;

    pgsz >>= 1;
    while (pgsz) {
        pgsz >>= 1;
        page_shift++;
    }

    return page_shift;
}

static void
generate_block(uint8_t *a, uint8_t *b, uint8_t *bm, size_t count,
               uint64_t pgsz, uint64_t addr, uint64_t *dirty_count)
{
    uint64_t *a_ptr = (uint64_t *)a;
    uint64_t *b_ptr = (uint64_t *)b;
    size_t i, j;
    int pgshift = get_shift(pgsz);
    int idxshift = get_shift(sizeof(uint64_t));
    bool page_dirty = false;

    /* pages / addresses are relative to block. 'addr' is just for reporting */
    for (i = 0; i < count / sizeof(uint64_t); i++) {
        /* TODO: if page is dirty we can skip to the next and not continue scanning */
        if (((i << idxshift) % pgsz) == 0) {
            page_dirty = false;
        }
        if (a_ptr[i] != b_ptr[i]) {
            size_t page_num = (i << idxshift) >> pgshift;
            uint8_t bm_bit_pos = (page_num % 8);

            if (page_dirty == false) {
                page_dirty = true;
                bm[page_num >> 3] |= (1 << bm_bit_pos);
                *dirty_count += 1;
            }
        }
    }
}

static bool
check_block(uint8_t *a, uint8_t *b, uint8_t *bm, size_t count,
            uint64_t pgsz, uint64_t addr, uint64_t *dirty_count,
            bool exit_on_mismatch)
{
    size_t i, j, skip;
    int pgshift = get_shift(pgsz);
    int idxshift = get_shift(sizeof(uint64_t));
    bool page_dirty;
    bool mismatch = false;

    uint64_t *a_ptr = (uint64_t *)a;
    uint64_t *b_ptr = (uint64_t *)b;

    for (i = 0; i < count / sizeof(uint64_t) && (!mismatch || !exit_on_mismatch); i += skip) {
        skip = 1;
        if (((i << idxshift) % pgsz) == 0) {
            page_dirty = false;
        }
        if (a_ptr[i] != b_ptr[i]) {
            size_t page_num = (i << idxshift) >> pgshift;
            uint8_t bm_byte = bm[page_num >> 3];
            uint8_t bm_bit_pos = (page_num % 8);
            bool bm_bit;

            if (page_dirty == false) {
                page_dirty = true;
                *dirty_count += 1;
            }

            if (bm_byte == 0) {
                printf("page at addr 0x%lx is dirty, but byte 0x%lx of bitmap is clear\n",
                       (addr + (i << idxshift)),
                       ((addr + (i << idxshift)) >> pgshift) >> 3);
                mismatch = true;
            }

            bm_bit = !!(bm_byte & (1 << bm_bit_pos));
            if (bm_byte != 0 && !bm_bit) {
                printf("page at addr 0x%lx is dirty, but bit %u of byte 0x%lx (0x%x) of bitmap is clear\n",
                       (addr + (i << idxshift)),
                       bm_bit_pos,
                       ((addr + (i << idxshift)) >> pgshift) >> 3,
                       bm_byte);
                mismatch = true;
            }
        }
        if (page_dirty)
            skip = (pgsz - ((i << idxshift) % pgsz)) / sizeof(uint64_t);
    }

    return !mismatch;
}

void generate_bitmap(FILE *a, FILE *b, FILE *bm,
                     uint64_t maddr, uint64_t msize, uint64_t pgsz)
{
    bool last_loop = false;
    uint64_t mpos, dirty_count = 0;
    uint8_t *a_buf, *b_buf, *bm_buf;

    mpos = maddr;
    a_buf = calloc(1, MBUF_SZ);
    b_buf = calloc(1, MBUF_SZ);
    bm_buf = calloc(1, MBUF_SZ / pgsz);

    while (!last_loop && mpos < (maddr + msize)) {
        size_t acount, bcount, bmcount, count;
        acount = fread(a_buf, 1, MBUF_SZ, a);
        if (acount < MBUF_SZ)
            last_loop = true;
        bcount = fread(b_buf, 1, MBUF_SZ, b);
        if (bcount < MBUF_SZ)
            last_loop = true;

        bmcount = MBUF_SZ / pgsz / 8;
        memset(bm_buf, 0, bmcount);

        count = MIN(MIN(acount, bcount), bmcount * pgsz * 8);
        generate_block(a_buf, b_buf, bm_buf, count, pgsz, mpos, &dirty_count);
        fwrite(bm_buf, 1, bmcount, bm);

        mpos += MBUF_SZ;
    }
}

bool check_bitmap(FILE *a, FILE *b, FILE *bm,
                  uint64_t maddr, uint64_t msize, uint64_t pgsz, bool exit_on_mismatch)
{
    bool last_loop;
    uint64_t mpos, dirty_count = 0;
    uint8_t *a_buf, *b_buf, *bm_buf;
    bool mismatch = false;

    mpos = maddr;
    a_buf = calloc(1, MBUF_SZ);
    b_buf = calloc(1, MBUF_SZ);
    bm_buf = calloc(1, MBUF_SZ / pgsz);

    while (!last_loop && mpos < (maddr + msize) && (!mismatch || !exit_on_mismatch)) {
        size_t acount, bcount, bmcount, count;
        bool block_mismatch;
        acount = fread(a_buf, 1, MBUF_SZ, a);
        if (acount < MBUF_SZ)
            last_loop = true;
        bcount = fread(b_buf, 1, MBUF_SZ, b);
        if (bcount < MBUF_SZ)
            last_loop = true;
        bmcount = fread(bm_buf, 1, MBUF_SZ / pgsz / 8, bm);
        if (bmcount < MBUF_SZ / pgsz / 8)
            last_loop = true;

        count = MIN(MIN(acount, bcount), bmcount * pgsz * 8);
        block_mismatch = !check_block(a_buf, b_buf, bm_buf, count, pgsz, mpos, &dirty_count, exit_on_mismatch);
        if (block_mismatch) {
            mismatch = true;
            printf("inconsistent dirty bitmap within 0x%lx to 0x%lx\n", mpos, mpos + MBUF_SZ);
        }
        mpos += MBUF_SZ;
    }

    if (!mismatch)
        printf("no inconsistencies found. found %lu dirty pages.\n", dirty_count);

    return !mismatch;
}


int main(int argc, char **argv)
{
    const char *a_path, *b_path, *bm_path;
    uint64_t maddr, msize, pgsz;
    FILE *a, *b, *bm;
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
        pgsz = 4096;

    if (argc > 7)
        error(1, 0, "just use all the same starting offsets for now");

    a = fopen(a_path, "rb");
    if (!a)
        error(1, errno, "failed to open %s", a_path);
    b = fopen(b_path, "rb");
    if (!b)
        error(1, errno, "failed to open %s", b_path);
    bm = fopen(bm_path, generate ? "wb" : "rb");
    if (!bm)
        error(1, errno, "failed to open %s", argv[3]);

    if (maddr != 0) {
        if (fseek(a, maddr, SEEK_SET))
            error(1, errno, "%s: seek to addr 0x%lx failed", a_path, maddr);
        if (fseek(b, maddr, SEEK_SET))
            error(1, errno, "%s: seek to addr 0x%lx failed", b_path, maddr);
        if (fseek(bm, maddr / pgsz / 8, SEEK_SET))
            error(1, errno, "%s: seek to addr 0x%lx failed", bm_path, maddr / pgsz / 8);
    }

    if (generate)
        generate_bitmap(a, b, bm, maddr, msize, pgsz);
    else
        mismatch = !check_bitmap(a, b, bm, maddr, msize, pgsz, exit_on_mismatch);

    fclose(a);
    fclose(b);
    fclose(bm);

    return (mismatch) ? 1 : 0;
}
