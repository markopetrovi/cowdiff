/*
 * cowdiff - a copy-on-write aware diff.
 *
 * The idea: on a filesystem that supports reflinks (btrfs, XFS), two files
 * that share data point at the *same* bytes on disk.  FIEMAP reports that
 * address, so the address can be used as a content identity: if a range of A
 * and a range of B map to the same address, those bytes are equal and need
 * never be read.
 *
 * The address identifies content, not position, so a match where the two
 * ranges sit at *different* file offsets is still a proof of equality -- and
 * the offset delta is itself the edit (an insertion or deletion).  Those
 * matches are called anchors; everything between them is a gap, and only gaps
 * are ever read.
 */
#ifndef COWDIFF_H
#define COWDIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- extents ---------------------------------------------------------- */

/*
 * One extent as returned by FIEMAP, clamped to the file size.
 *
 * `trusted` means `phys` is a valid content identity.  It is decided purely
 * from the flag bits: an extent whose address cannot be relied on (inline
 * data living in metadata, delayed allocation, a filesystem that merged
 * adjacent extents because it has no native extent list, ...) is untrusted
 * and must be read and compared like any other unknown byte range.  Note
 * that an *unwritten* extent is trustworthy in a different way: it is
 * allocated but holds no data, so it reads as zeros without touching disk.
 */
struct ext {
	uint64_t off;		/* offset in file */
	uint64_t len;
	uint64_t phys;		/* filesystem address; valid iff trusted */
	uint32_t flags;		/* FIEMAP_EXTENT_* */
	bool trusted;
	bool zero;		/* reads as zeros (unwritten) */
};

struct extmap {
	struct ext *v;
	size_t n;
	size_t cap;
	uint64_t size;
};

void extmap_init(struct extmap *m);
void extmap_free(struct extmap *m);

/*
 * Load the extent map of an already-open fd.  Never fails in a way that
 * prevents a diff: if the filesystem does not implement FIEMAP this falls
 * back to SEEK_DATA/SEEK_HOLE, and failing that to a single untrusted extent
 * spanning the file.  Returns 0 on success, -1 only on a genuine error
 * (bad fd, out of memory).
 */
int extmap_load(struct extmap *m, int fd, uint64_t size);

/* Extent containing `off`, or NULL if that offset falls in a hole. */
const struct ext *extmap_at(const struct extmap *m, uint64_t off);

/* Every byte this process has actually read, for --stats. */
extern uint64_t cowdiff_bytes_read;

/* Is this extent's address usable as a content identity? */
bool ext_trusted(uint32_t flags);

/* Does this extent read as zeros without needing a disk read? */
bool ext_iszero(uint32_t flags);

/*
 * True if both fds refer to the same filesystem, so that equal addresses
 * really do mean equal content.
 *
 * Comparing raw st_dev is not enough: btrfs gives every subvolume its own
 * anonymous device number, so a file and its snapshot -- the single most
 * important case for this tool -- would look like they were on different
 * filesystems.  So, in order:
 *
 *   1. equal st_dev, which settles it and also covers bind mounts;
 *   2. the generic FS_IOC_GETFSUUID, which is what most filesystems use;
 *   3. BTRFS_IOC_FS_INFO.  btrfs never assigns sb->s_uuid_len, so the
 *      generic ioctl above returns ENOTTY for it; this one reports the
 *      filesystem UUID and is stable across subvolumes.
 *
 * If none of those can answer the question the answer is no: claiming two
 * unrelated filesystems are the same would let coincidentally equal
 * addresses masquerade as equal content.
 */
bool same_filesystem(int fd_a, int fd_b);

/* ---- anchors ---------------------------------------------------------- */

/*
 * A range proven equal: A[a_off, a_off+len) and B[b_off, b_off+len) hold the
 * same bytes because they map to the same disk address.  a_off == b_off for
 * the ordinary case (a snapshot, a reflink copy); a_off != b_off means the
 * content sits at different offsets in the two files, i.e. an insertion or
 * deletion has happened before it.
 */
struct anchor {
	uint64_t a_off;
	uint64_t b_off;
	uint64_t len;
};

struct anchorlist {
	struct anchor *v;
	size_t n;
	size_t cap;
};

void anchors_free(struct anchorlist *al);

/*
 * Find anchors by intersecting the two files' physical address ranges.
 * Only meaningful when both maps carry trusted addresses from the same
 * filesystem; otherwise the result is empty and the caller compares
 * everything.  Returns 0 on success, -1 on out of memory.
 */
int anchors_find(const struct extmap *a, const struct extmap *b,
		 bool addresses_comparable, struct anchorlist *out);

/* ---- differences ------------------------------------------------------ */

/*
 * One edit: A[a_off, a_off+a_len) was replaced by B[b_off, b_off+b_len).
 * Either length may be zero, meaning a pure insertion or deletion.  For an
 * unaligned substitution both lengths cover the same byte span at the same
 * offsets, so the common case has a_off == b_off.
 */
struct delta {
	uint64_t a_off, a_len;
	uint64_t b_off, b_len;
};

struct deltalist {
	struct delta *v;
	size_t n;
	size_t cap;
};

void deltas_free(struct deltalist *dl);

/*
 * Walk the gaps between anchors and resolve each one into deltas, reading
 * only what cannot be proven equal.  `fd_a`/`fd_b` must be open for reading.
 * Returns 0 on success, -1 on error.
 */
int deltas_find(int fd_a, const struct extmap *a, uint64_t size_a,
		int fd_b, const struct extmap *b, uint64_t size_b,
		const struct anchorlist *al, struct deltalist *out);

/* ---- io --------------------------------------------------------------- */

struct iobuf {
	unsigned char *p;
	unsigned char *zero;	/* cap zero bytes, so all-zero tests are a memcmp */
	size_t cap;
};

int iobuf_init(struct iobuf *b, size_t cap);
void iobuf_free(struct iobuf *b);

/* Read exactly len bytes at off; returns 0, or -1 on short read or error. */
int pread_full(int fd, void *buf, size_t len, uint64_t off);

/* Fill buf with len zero bytes' worth of knowledge: read a range and report
 * whether every byte was zero.  Reads in chunks so it never allocates. */
int range_is_zero(int fd, const struct extmap *m, uint64_t off, uint64_t len,
		  struct iobuf *scratch, bool *out);

/* ---- output ----------------------------------------------------------- */

int emit_binary_diff(const char *path_a, const char *path_b,
		     const struct deltalist *dl);
int emit_text_diff(const char *path_a, const char *path_b, int fd_a, int fd_b,
		   const struct extmap *ma, const struct extmap *mb,
		   const struct deltalist *dl);

/* Is this file binary?  Follows diff(1): a NUL byte in the first block. */
bool is_binary(int fd);

/* ---- line diff -------------------------------------------------------- */

/* One line, including its trailing newline when it has one. */
struct lline {
	size_t boff;		/* offset within the lineset buffer */
	uint32_t len;
	uint64_t hash;
};

struct lineset {
	unsigned char *buf;
	size_t buflen;
	struct lline *v;
	size_t n;
};

/* A[alo,ahi) replaced by B[blo,bhi), in line numbers. */
struct edit {
	size_t alo, ahi;
	size_t blo, bhi;
};

struct editlist {
	struct edit *v;
	size_t n;
	size_t cap;
};

/* Read [off, off+len) and split it into lines. */
int lineset_build(struct lineset *ls, int fd, uint64_t off, uint64_t len);
void lineset_free(struct lineset *ls);

/*
 * Line diff of two ranges.  Exact while the ranges are small, which is the
 * case that matters -- a gap between anchors is usually a handful of lines --
 * and anchor-seeking when they are not.
 */
int linediff(const struct lineset *a, const struct lineset *b,
	     struct editlist *out);
void edits_free(struct editlist *el);

/* Round a byte offset out to the next line boundary in the given file. */
int line_end(int fd, uint64_t off, uint64_t limit, uint64_t *out);
/* Round a byte offset back to the start of its line. */
int line_start(int fd, uint64_t off, uint64_t *out);
/* Number of newlines in [0, off); used for hunk headers. */
int count_lines(int fd, uint64_t off, uint64_t *out);

#endif /* COWDIFF_H */
