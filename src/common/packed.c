/* packed.c — mmap loader for packed.bin (read-only, shared across instances).
 * v11: int16 point-major (re-rank) + int8 dim-pair-interleaved SoA (coarse). */
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

    const char *cb = (const char *)base;
    ds->hdr      = h;
    ds->nrefs    = h->nrefs;
    ds->clust_pt_off = (const uint32_t *)(cb + h->clust_pt_off_off);
    ds->centroids    = (const int16_t  *)(cb + h->centroids_off);
    ds->vecs16   = (const int16_t  *)(cb + h->vecs16_off);
    ds->vecs8    = (const int8_t   *)(cb + h->vecs8_off);
    ds->orig     = (const uint32_t *)(cb + h->orig_off);
    ds->fraud    = (const uint8_t  *)(cb + h->fraud_off);
    ds->map_base = base;
    ds->map_len  = len;

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
