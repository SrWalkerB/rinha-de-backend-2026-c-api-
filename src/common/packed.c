/* packed.c — mmap loader for packed.bin (read-only, shared across instances). */
#define _GNU_SOURCE
#include "fraud.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

/* Layout helpers — MUST match prepare.c exactly. */
static inline size_t align64(size_t x) { return (x + 63) & ~(size_t)63; }
size_t packed_vecs8_off(void)           { return align64(sizeof(PackedHeader)); }
size_t packed_vecs16_off(uint32_t nrefs){
    /* +64 so the last vec8's 16-byte SIMD load never reads past mapped memory */
    return align64(packed_vecs8_off() + (size_t)nrefs * VDIM8 * sizeof(int8_t) + 64);
}
size_t packed_orig_off(uint32_t nrefs)  {
    return packed_vecs16_off(nrefs) + (size_t)nrefs * VDIM8 * sizeof(int16_t);
}
size_t packed_fraud_off(uint32_t nrefs) {
    return packed_orig_off(nrefs) + (size_t)nrefs * sizeof(uint32_t);
}
size_t packed_total(uint32_t nrefs) {
    return packed_fraud_off(nrefs) + (nrefs + 7) / 8;
}

int ds_open(Dataset *ds, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open packed.bin"); return -1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return -1; }
    size_t len = (size_t)st.st_size;

    void *base = mmap(NULL, len, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { perror("mmap"); return -1; }

    const PackedHeader *h = (const PackedHeader *)base;
    if (h->magic != PACKED_MAGIC || h->version != PACKED_VERSION ||
        h->vdim != VDIM || h->vlanes != VLANES || h->nbuckets != NBUCKETS) {
        fprintf(stderr, "packed.bin: bad header\n");
        munmap(base, len);
        return -1;
    }

    ds->hdr      = h;
    ds->nrefs    = h->nrefs;
    ds->vecs8    = (const int8_t   *)((const char *)base + packed_vecs8_off());
    ds->vecs16   = (const int16_t  *)((const char *)base + packed_vecs16_off(h->nrefs));
    ds->orig     = (const uint32_t *)((const char *)base + packed_orig_off(h->nrefs));
    ds->fraud    = (const uint8_t  *)((const char *)base + packed_fraud_off(h->nrefs));
    ds->map_base = base;
    ds->map_len  = len;

    /* Warm the page cache + advise kernel we'll touch it all, then pin it
       resident: the index is clean file-backed memory, so under a strict cgroup
       it is the first thing the kernel reclaims under pressure — eviction storms
       are what tank p99. mlock guarantees zero major faults after startup.
       Log-and-continue: a failed lock must degrade to old behavior, never abort. */
    madvise(base, len, MADV_WILLNEED);
    if (mlock(base, len) != 0)
        fprintf(stderr, "WARN mlock(%zu) failed: %s — index NOT pinned\n",
                len, strerror(errno));
    else
        fprintf(stderr, "mlocked %zu bytes (index pinned)\n", len);
    return 0;
}

void ds_close(Dataset *ds) {
    if (ds->map_base) { munmap(ds->map_base, ds->map_len); ds->map_base = NULL; }
}
