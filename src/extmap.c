/*
 * extmap.c - where the data of a file actually lives.
 *
 * Everything this tool knows about which bytes can be skipped comes from
 * here, so the rules are deliberately conservative in one direction only:
 * an extent may be declared skippable only when the kernel said something
 * that proves it.  Declaring two different byte ranges equal by mistake is
 * the one error that would make the output a lie, so when in doubt the
 * answer is "read it".
 */
#define _GNU_SOURCE
#include "cowdiff.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/fiemap.h>
#include <linux/fs.h>

#if defined(__has_include)
#  if __has_include(<linux/btrfs.h>)
#    include <linux/btrfs.h>
#    define HAVE_BTRFS_H 1
#  endif
#endif

/* ---- what the flag bits mean ------------------------------------------ */

bool ext_iszero(uint32_t flags)
{
	return (flags & FIEMAP_EXTENT_UNWRITTEN) != 0;
}

bool ext_trusted(uint32_t flags)
{
	/*
	 * Each of these says the reported address is not a place where these
	 * bytes live: inline data is packed into metadata, delayed allocation
	 * has not been placed yet, and a filesystem with no native extent
	 * list (MERGED) synthesised the extent for our benefit.
	 */
	static const uint32_t untrustworthy =
		FIEMAP_EXTENT_UNKNOWN |
		FIEMAP_EXTENT_DELALLOC |
		FIEMAP_EXTENT_NOT_ALIGNED |
		FIEMAP_EXTENT_DATA_INLINE |
		FIEMAP_EXTENT_DATA_TAIL |
		FIEMAP_EXTENT_MERGED;

	/*
	 * An unwritten extent is allocated but holds no data, so it reads as
	 * zeros whatever the address holds.  Its address is therefore not a
	 * content identity -- it is handled by the all-zero path instead,
	 * which also means it never has to be read at all.
	 */
	if (ext_iszero(flags))
		return false;

	return (flags & untrustworthy) == 0;
}

/* ---- the map ---------------------------------------------------------- */

void extmap_init(struct extmap *m)
{
	memset(m, 0, sizeof *m);
}

void extmap_free(struct extmap *m)
{
	free(m->v);
	extmap_init(m);
}

static void extmap_reset(struct extmap *m)
{
	free(m->v);
	m->v = NULL;
	m->n = 0;
	m->cap = 0;
}

static int extmap_add(struct extmap *m, uint64_t off, uint64_t len,
		      uint64_t phys, uint32_t flags)
{
	if (m->n == m->cap) {
		size_t ncap = m->cap ? m->cap * 2 : 256;
		struct ext *nv = realloc(m->v, ncap * sizeof *nv);

		if (!nv)
			return -1;
		m->v = nv;
		m->cap = ncap;
	}

	m->v[m->n++] = (struct ext){
		.off = off,
		.len = len,
		.phys = phys,
		.flags = flags,
		.trusted = ext_trusted(flags),
		.zero = ext_iszero(flags),
	};
	return 0;
}

const struct ext *extmap_at(const struct extmap *m, uint64_t off)
{
	size_t lo = 0, hi = m->n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const struct ext *e = &m->v[mid];

		if (off < e->off)
			hi = mid;
		else if (off >= e->off + e->len)
			lo = mid + 1;
		else
			return e;
	}
	return NULL;
}

/* ---- discovery -------------------------------------------------------- */

#define FIEMAP_BATCH 256

static int extmap_load_fiemap(struct extmap *m, int fd)
{
	size_t fmsz = sizeof(struct fiemap) +
		      FIEMAP_BATCH * sizeof(struct fiemap_extent);
	struct fiemap *fm;
	uint64_t start = 0;
	bool first = true;
	int rc = -1;

	fm = malloc(fmsz);
	if (!fm)
		return -1;

	for (;;) {
		bool past_eof = false;
		unsigned int i;
		uint64_t next = 0;

		memset(fm, 0, fmsz);
		fm->fm_start = start;
		fm->fm_length = FIEMAP_MAX_OFFSET - start;
		/*
		 * Without SYNC the kernel reports dirty page cache as DELALLOC
		 * extents whose address means nothing -- and two files with
		 * dirty cache would both report that same meaningless address
		 * for different data, which would read as a proof of equality.
		 * That is the one direction of error this tool must never make,
		 * so pay for the writeback once.
		 */
		fm->fm_flags = first ? FIEMAP_FLAG_SYNC : 0;
		fm->fm_extent_count = FIEMAP_BATCH;

		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0)
			goto out;	/* unsupported here; not an error */

		first = false;
		if (fm->fm_mapped_extents == 0) {
			rc = 0;
			goto out;
		}

		for (i = 0; i < fm->fm_mapped_extents; i++) {
			struct fiemap_extent *e = &fm->fm_extents[i];
			uint64_t off = e->fe_logical;
			uint64_t end = off + e->fe_length;

			if (off >= m->size) {
				past_eof = true;
				break;
			}
			/*
			 * Inline data is reported as a whole block even when
			 * the file is a handful of bytes, so an extent can
			 * claim to run past the end of the file.
			 */
			if (end > m->size)
				end = m->size;
			next = end;

			if (end <= off)
				continue;
			if (extmap_add(m, off, end - off, e->fe_physical,
				       e->fe_flags) < 0)
				goto out;
		}

		if (past_eof ||
		    (fm->fm_extents[fm->fm_mapped_extents - 1].fe_flags &
		     FIEMAP_EXTENT_LAST) ||
		    next <= start) {
			rc = 0;
			goto out;
		}

		start = next;
		if (start >= m->size) {
			rc = 0;
			goto out;
		}
	}
out:
	free(fm);
	return rc;
}

/*
 * Fallback for filesystems without FIEMAP.  There are no addresses, so
 * nothing can be proven equal, but a hole is still a hole: skipping those is
 * most of the benefit on a sparse file and needs no cooperation from the
 * filesystem beyond SEEK_HOLE.
 */
static int extmap_load_seek(struct extmap *m, int fd)
{
	uint64_t off = 0;

	while (off < m->size) {
		off_t data, hole;

		data = lseek(fd, (off_t)off, SEEK_DATA);
		if (data < 0) {
			if (errno == ENXIO)
				return 0;	/* hole to end of file */
			return -1;
		}
		hole = lseek(fd, data, SEEK_HOLE);
		if (hole < 0) {
			if (errno != ENXIO)
				return -1;
			hole = (off_t)m->size;
		}
		if ((uint64_t)hole > m->size)
			hole = (off_t)m->size;
		if ((uint64_t)hole <= (uint64_t)data)
			return 0;

		if (extmap_add(m, (uint64_t)data, (uint64_t)(hole - data), 0,
			       FIEMAP_EXTENT_UNKNOWN) < 0)
			return -1;
		off = (uint64_t)hole;
	}
	return 0;
}

int extmap_load(struct extmap *m, int fd, uint64_t size)
{
	m->size = size;

	if (size == 0)
		return 0;

	if (extmap_load_fiemap(m, fd) == 0)
		return 0;	/* may legitimately be empty: all holes */

	extmap_reset(m);
	if (extmap_load_seek(m, fd) == 0)
		return 0;

	extmap_reset(m);
	/* Nothing is known, so the whole file must be read and compared. */
	return extmap_add(m, 0, size, 0, FIEMAP_EXTENT_UNKNOWN);
}

/* ---- is it the same filesystem? --------------------------------------- */

static int getfsuuid(int fd, unsigned char uuid[16], unsigned int *len)
{
#ifdef FS_IOC_GETFSUUID
	struct fsuuid2 u;

	memset(&u, 0, sizeof u);
	if (ioctl(fd, FS_IOC_GETFSUUID, &u) < 0)
		return -1;
	if (u.len == 0 || u.len > sizeof u.uuid)
		return -1;
	memcpy(uuid, u.uuid, u.len);
	*len = u.len;
	return 0;
#else
	(void)fd;
	(void)uuid;
	(void)len;
	return -1;
#endif
}

/*
 * btrfs keeps its UUID to itself: the generic ioctl above needs
 * sb->s_uuid_len, and btrfs never assigns it, so it answers ENOTTY.  This is
 * the same filesystem UUID by another route, and it is stable across
 * subvolumes, which is the whole point -- a file and its snapshot live in
 * different subvolumes yet share extents.
 */
static int get_btrfs_fsid(int fd, unsigned char fsid[16])
{
#if defined(HAVE_BTRFS_H) && defined(BTRFS_IOC_FS_INFO)
	struct btrfs_ioctl_fs_info_args fi;

	memset(&fi, 0, sizeof fi);
	if (ioctl(fd, BTRFS_IOC_FS_INFO, &fi) < 0)
		return -1;
	memcpy(fsid, fi.fsid, sizeof fi.fsid);
	return 0;
#else
	(void)fd;
	(void)fsid;
	return -1;
#endif
}

bool same_filesystem(int fd_a, int fd_b)
{
	struct stat sa, sb;
	unsigned char ua[16], ub[16];
	unsigned int la = 0, lb = 0;

	if (fstat(fd_a, &sa) < 0 || fstat(fd_b, &sb) < 0)
		return false;

	/* Same superblock.  This also covers bind mounts. */
	if (sa.st_dev == sb.st_dev)
		return true;

	/* The generic route: ext4, XFS, tmpfs, erofs, bcachefs, ... */
	if (getfsuuid(fd_a, ua, &la) == 0 && getfsuuid(fd_b, ub, &lb) == 0)
		return la == lb && memcmp(ua, ub, la) == 0;

	/* btrfs: one filesystem, many subvolumes, many device numbers. */
	if (get_btrfs_fsid(fd_a, ua) == 0 && get_btrfs_fsid(fd_b, ub) == 0)
		return memcmp(ua, ub, sizeof ua) == 0;

	/*
	 * Unresolved.  Answering no only costs the address optimisation;
	 * answering yes wrongly could call two different files identical.
	 */
	return false;
}

/* ---- small io helpers ------------------------------------------------- */

int iobuf_init(struct iobuf *b, size_t cap)
{
	b->p = malloc(cap);
	b->zero = calloc(1, cap);
	if (!b->p || !b->zero) {
		free(b->p);
		free(b->zero);
		memset(b, 0, sizeof *b);
		return -1;
	}
	b->cap = cap;
	return 0;
}

void iobuf_free(struct iobuf *b)
{
	free(b->p);
	free(b->zero);
	memset(b, 0, sizeof *b);
}

uint64_t cowdiff_bytes_read;

/*
 * A test hook, and only that: pretend the storage under a range is damaged so
 * the unreadable-block path can be exercised without a damaged disk.  No
 * filesystem can be asked to return EIO on demand, and a test for this
 * behaviour that needs a failing drive is a test nobody runs.
 *
 * Set COWDIFF_EIO_AT="offset:length" (C syntax, so 0x... works).  Nothing but
 * tests/run.sh sets it.
 */
static bool faulted(uint64_t off, size_t len)
{
	static bool ready;
	static uint64_t at, end;
	const char *s;
	char *p;

	if (!ready) {
		ready = true;
		s = getenv("COWDIFF_EIO_AT");
		if (s && *s) {
			at = strtoull(s, &p, 0);
			if (*p == ':') {
				end = at + strtoull(p + 1, NULL, 0);
			} else {
				at = end = 0;
			}
		}
	}
	return end > at && off < end && at < off + len;
}

int pread_full(int fd, void *buf, size_t len, uint64_t off)
{
	unsigned char *p = buf;

	if (faulted(off, len)) {
		errno = EIO;
		return -1;
	}

	while (len > 0) {
		ssize_t r = pread(fd, p, len, (off_t)off);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;	/* errno says why: EIO is a bad block */
		}
		if (r == 0) {
			/* Short read: the range asked for runs past the end, so
			 * the caller mis-sized it.  Distinct from EIO on
			 * purpose -- one is a damaged file, the other a bug. */
			errno = ERANGE;
			return -1;
		}
		cowdiff_bytes_read += (uint64_t)r;
		p += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

int range_is_zero(int fd, const struct extmap *m, uint64_t off, uint64_t len,
		  struct iobuf *scratch, bool *out)
{
	*out = true;

	while (len > 0) {
		const struct ext *e = extmap_at(m, off);
		uint64_t chunk = len;

		/*
		 * A hole reads as zeros, and so does an unwritten extent --
		 * which is exactly why unwritten extents are not treated as
		 * content identities.  Neither needs to be read.
		 */
		if (!e || e->zero) {
			uint64_t skip = e ? e->off + e->len - off : len;

			if (skip > chunk)
				skip = chunk;
			off += skip;
			len -= skip;
			continue;
		}

		if (chunk > scratch->cap)
			chunk = scratch->cap;
		if (chunk > e->off + e->len - off)
			chunk = e->off + e->len - off;
		if (chunk == 0)
			break;

		if (pread_full(fd, scratch->p, chunk, off) < 0) {
			/* A bad block is not zeros, and it is not something
			 * that can be shown to be zeros either.  Say so rather
			 * than failing the run; the caller reports it. */
			if (errno == EIO)
				return 1;
			return -1;
		}
		if (memcmp(scratch->p, scratch->zero, chunk) != 0) {
			*out = false;
			return 0;
		}
		off += chunk;
		len -= chunk;
	}
	return 0;
}
