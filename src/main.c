/*
 * cowdiff - diff two files using the filesystem's extent map.
 */
#define _GNU_SOURCE
#include "cowdiff.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/fiemap.h>

static bool opt_brief, opt_stats, opt_force_binary, opt_force_text;
static bool opt_byte_offsets, opt_dump;

static void usage(FILE *f, const char *argv0)
{
	fprintf(f,
"usage: %s [options] FILE1 FILE2\n"
"\n"
"Compare two files using the filesystem's own extent map, so that data which\n"
"is physically shared -- reflinks, snapshots, deduplicated blocks -- is never\n"
"read at all.  Holes and unwritten extents are skipped rather than read as\n"
"zeros, and only ranges that are not shared get read and compared.\n"
"\n"
"options:\n"
"  -q, --brief           report only whether the files differ\n"
"      --stats           report how many bytes were actually read\n"
"      --byte-offsets    put byte offsets in hunk headers instead of line\n"
"                        numbers; skips the scan that line numbers need, at\n"
"                        the cost of output that patch(1) cannot consume\n"
"      --force-binary    treat the files as binary\n"
"      --force-text      treat them as text even if they look binary\n"
"      --dump-extents    print the extent map and exit; with one FILE this\n"
"                        is that file's map, with two it is both\n"
"  -h, --help            this message\n"
"\n"
"exit status: 0 files are identical, 1 they differ, 2 an error occurred\n",
		argv0);
}

static void dump_map(const char *path, const struct extmap *m)
{
	size_t i;

	printf("%s: %zu extents, %llu bytes\n", path, m->n,
	       (unsigned long long)m->size);
	for (i = 0; i < m->n; i++) {
		const struct ext *e = &m->v[i];

		printf("  off=%-12llu len=%-10llu phys=%-14llu %s%s%s%s\n",
		       (unsigned long long)e->off, (unsigned long long)e->len,
		       (unsigned long long)e->phys,
		       e->trusted ? "trusted" : "untrusted",
		       (e->flags & FIEMAP_EXTENT_ENCODED) ? " encoded" : "",
		       (e->flags & FIEMAP_EXTENT_UNWRITTEN) ? " unwritten" : "",
		       (e->flags & FIEMAP_EXTENT_SHARED) ? " shared" : "");
	}
}

/*
 * Open a file and read its extent map.  Both the comparison and
 * --dump-extents need exactly this, so neither owns a copy of it.
 */
static int open_map(const char *path, struct extmap *m, int *fd_out,
		    struct stat *st)
{
	int fd;

	extmap_init(m);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "cowdiff: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, st) < 0) {
		fprintf(stderr, "cowdiff: %s: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (S_ISDIR(st->st_mode)) {
		fprintf(stderr, "cowdiff: %s: is a directory\n", path);
		close(fd);
		return -1;
	}
	if (extmap_load(m, fd, (uint64_t)st->st_size) < 0) {
		fprintf(stderr, "cowdiff: %s: cannot read extent map: %s\n",
			path, strerror(errno));
		close(fd);
		return -1;
	}

	*fd_out = fd;
	return 0;
}

/* --dump-extents on a single file: no comparison, just the map. */
static int dump_one(const char *path)
{
	struct extmap m;
	struct stat st;
	int fd;

	if (open_map(path, &m, &fd, &st) < 0)
		return 2;

	dump_map(path, &m);
	close(fd);
	extmap_free(&m);
	return 0;
}

static int compare(const char *pa, const char *pb)
{
	struct extmap ma, mb;
	struct anchorlist al;
	struct deltalist dl;
	struct stat sa, sb;
	bool cmp_addr;
	int fd_a = -1, fd_b = -1;
	int rc = 2, out;

	extmap_init(&ma);
	extmap_init(&mb);
	memset(&al, 0, sizeof al);
	memset(&dl, 0, sizeof dl);

	if (open_map(pa, &ma, &fd_a, &sa) < 0 ||
	    open_map(pb, &mb, &fd_b, &sb) < 0)
		goto out;

	/* The same inode twice needs no work and no reads. */
	if (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) {
		printf("Files %s and %s are identical\n", pa, pb);
		rc = 0;
		goto out;
	}

	if (opt_dump) {
		dump_map(pa, &ma);
		dump_map(pb, &mb);
		rc = 0;
		goto out;
	}

	/*
	 * Addresses only mean something within one filesystem.  Across two
	 * of them, equal numbers are a coincidence, and trusting them would
	 * mean calling different files identical.
	 */
	cmp_addr = same_filesystem(fd_a, fd_b);

	if (anchors_find(&ma, &mb, cmp_addr, &al) < 0) {
		fprintf(stderr, "cowdiff: out of memory\n");
		goto out;
	}
	if (deltas_find(fd_a, &ma, (uint64_t)sa.st_size, fd_b, &mb,
			(uint64_t)sb.st_size, &al, &dl) < 0) {
		fprintf(stderr, "cowdiff: %s\n", strerror(errno));
		goto out;
	}

	if (dl.n == 0) {
		printf("Files %s and %s are identical\n", pa, pb);
		rc = 0;
		goto stats;
	}

	if (opt_brief) {
		printf("Files %s and %s differ\n", pa, pb);
		rc = 1;
		goto stats;
	}

	if (opt_force_binary || (!opt_force_text && (is_binary(fd_a) ||
						     is_binary(fd_b)))) {
		out = emit_binary_diff(pa, pb, &dl);
	} else {
		out = emit_text_diff(pa, pb, fd_a, fd_b, &ma, &mb, &dl,
				     opt_byte_offsets);
	}
	if (out < 0) {
		fprintf(stderr, "cowdiff: %s\n", strerror(errno));
		goto out;
	}
	rc = out ? 1 : 0;

stats:
	/*
	 * After the output, not before: text mode reads more than the
	 * comparison does, because a hunk header carries line numbers and a
	 * line number costs a pass over the file.  Reporting first would hide
	 * exactly the cost worth knowing about.
	 */
	if (opt_stats) {
		unsigned long long total = (unsigned long long)sa.st_size +
					   (unsigned long long)sb.st_size;
		unsigned long long covered = 0;
		size_t k;

		for (k = 0; k < al.n; k++)
			covered += al.v[k].len;

		fprintf(stderr,
			"cowdiff: %zu anchor%s proving %llu of %llu bytes equal (%.4f%%)\n"
			"cowdiff: read %llu of %llu bytes (%.4f%%)\n",
			al.n, al.n == 1 ? "" : "s", covered, total,
			total ? 100.0 * (double)covered / (double)total : 0.0,
			(unsigned long long)cowdiff_bytes_read, total,
			total ? 100.0 * (double)cowdiff_bytes_read / (double)total
			      : 0.0);
	}

out:
	anchors_free(&al);
	deltas_free(&dl);
	extmap_free(&ma);
	extmap_free(&mb);
	if (fd_a >= 0)
		close(fd_a);
	if (fd_b >= 0)
		close(fd_b);
	return rc;
}

int main(int argc, char **argv)
{
	const char *pa = NULL, *pb = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout, argv[0]);
			return 0;
		} else if (!strcmp(a, "-q") || !strcmp(a, "--brief")) {
			opt_brief = true;
		} else if (!strcmp(a, "--stats")) {
			opt_stats = true;
		} else if (!strcmp(a, "--force-binary")) {
			opt_force_binary = true;
		} else if (!strcmp(a, "--byte-offsets")) {
			opt_byte_offsets = true;
		} else if (!strcmp(a, "--force-text")) {
			opt_force_text = true;
		} else if (!strcmp(a, "--dump-extents")) {
			opt_dump = true;
		} else if (a[0] == '-' && a[1] != '\0') {
			fprintf(stderr, "cowdiff: unknown option %s\n", a);
			usage(stderr, argv[0]);
			return 2;
		} else if (!pa) {
			pa = a;
		} else if (!pb) {
			pb = a;
		} else {
			fprintf(stderr, "cowdiff: too many arguments\n");
			usage(stderr, argv[0]);
			return 2;
		}
	}

	/* Dumping a map needs one file; comparing needs two. */
	if (!pa || (!pb && !opt_dump)) {
		usage(stderr, argv[0]);
		return 2;
	}

	if (opt_dump && !pb)
		return dump_one(pa);

	return compare(pa, pb);
}
