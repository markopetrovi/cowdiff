/*
 * probe - test fixture helper for cowdiff.
 *
 * Two jobs that the main tool deliberately does not do:
 *   dump  FILE                      print the FIEMAP extent map, raw
 *   clone SRC DST SRCOFF LEN DSTOFF clone a byte range between files
 *
 * "clone" exists because btrfs has no FALLOC_FL_INSERT_RANGE, so the only way
 * to build a *shifted* share (B's tail pointing at A's extents, but at file
 * offsets differing by the insert size) is to insert the bytes the ordinary
 * way and then re-clone the tail from the original.  That is the construct
 * that exercises cowdiff's shifted-anchor path.
 *
 * btrfs requires sector-aligned offsets for clone, so callers must insert a
 * whole block for the shifted tail to be re-sharable.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/fiemap.h>
#include <linux/fs.h>

static const char *flag_names[] = {
	[0]  = "last", [1]  = "unknown", [2]  = "delalloc", [3]  = "encoded",
	[7]  = "encrypted", [8]  = "not_aligned", [9] = "inline",
	[10] = "tail", [11] = "unwritten", [12] = "merged", [13] = "shared",
};

static void print_flags(unsigned int flags)
{
	int first = 1;

	for (int b = 0; b < 32; b++) {
		if (!(flags & (1u << b)))
			continue;
		const char *name = (b < 14 && flag_names[b]) ? flag_names[b] : NULL;
		if (name)
			printf("%s%s", first ? "" : ",", name);
		else
			printf("%sbit%d", first ? "" : ",", b);
		first = 0;
	}
	if (first)
		printf("-");
}

static int cmd_dump(const char *path)
{
	struct stat st;
	unsigned int n = 256;
	struct fiemap *fm;
	int fd, first = 1;
	unsigned long long start = 0, last = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "fstat: %s\n", strerror(errno));
		return 1;
	}

	fm = calloc(1, sizeof(*fm) + n * sizeof(struct fiemap_extent));
	if (!fm) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	printf("%s: size=%llu\n", path, (unsigned long long)st.st_size);

	for (;;) {
		unsigned int i;

		memset(fm, 0, sizeof(*fm) + n * sizeof(struct fiemap_extent));
		fm->fm_start = start;
		fm->fm_length = FIEMAP_MAX_OFFSET;
		fm->fm_flags = first ? FIEMAP_FLAG_SYNC : 0;
		fm->fm_extent_count = n;

		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
			fprintf(stderr, "FS_IOC_FIEMAP: %s\n", strerror(errno));
			return 1;
		}
		first = 0;
		if (fm->fm_mapped_extents == 0)
			break;

		for (i = 0; i < fm->fm_mapped_extents; i++) {
			struct fiemap_extent *e = &fm->fm_extents[i];

			printf("  logical=%-12llu phys=%-12llu len=%-10llu flags=",
			       (unsigned long long)e->fe_logical,
			       (unsigned long long)e->fe_physical,
			       (unsigned long long)e->fe_length);
			print_flags(e->fe_flags);
			printf("\n");
			last = e->fe_logical + e->fe_length;
		}
		if (fm->fm_extents[fm->fm_mapped_extents - 1].fe_flags & FIEMAP_EXTENT_LAST)
			break;
		if (last <= start)
			break;	/* no forward progress; avoid spinning */
		start = last;
		if (start >= (unsigned long long)st.st_size)
			break;
	}
	close(fd);
	free(fm);
	return 0;
}

static int cmd_clone(const char *src, const char *dst,
		     unsigned long long src_off, unsigned long long len,
		     unsigned long long dst_off)
{
	struct file_clone_range r;
	int sfd, dfd, ret = 0;

	sfd = open(src, O_RDONLY);
	if (sfd < 0) {
		fprintf(stderr, "open %s: %s\n", src, strerror(errno));
		return 1;
	}
	dfd = open(dst, O_WRONLY);
	if (dfd < 0) {
		fprintf(stderr, "open %s: %s\n", dst, strerror(errno));
		close(sfd);
		return 1;
	}

	memset(&r, 0, sizeof(r));
	r.src_fd = sfd;
	r.src_offset = src_off;
	r.src_length = len;
	r.dest_offset = dst_off;

	if (ioctl(dfd, FICLONERANGE, &r) < 0) {
		fprintf(stderr, "FICLONERANGE %s[%llu,+%llu] -> %s[%llu]: %s\n",
			src, src_off, len, dst, dst_off, strerror(errno));
		ret = 1;
	} else {
		printf("cloned %s[%llu,+%llu] -> %s[%llu]\n",
		       src, src_off, len, dst, dst_off);
	}

	close(sfd);
	close(dfd);
	return ret;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s dump FILE\n"
		"       %s clone SRC DST SRCOFF LEN DSTOFF\n", argv0, argv0);
}

int main(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "dump"))
		return cmd_dump(argv[2]);
	if (argc >= 7 && !strcmp(argv[1], "clone"))
		return cmd_clone(argv[2], argv[3], strtoull(argv[4], NULL, 0),
				 strtoull(argv[5], NULL, 0), strtoull(argv[6], NULL, 0));
	usage(argv[0]);
	return 2;
}
