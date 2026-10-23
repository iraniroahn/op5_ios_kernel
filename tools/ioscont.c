/*
 * ioscont: the iOS root as a container file inside Android's userdata
 * (PORT-OP5.md "Keeping Android"). Runs as root on the phone (Android or
 * TWRP); static aarch64 binary, see tools/build-ioscont.sh.
 *
 *   ioscont check DIR         write a 64 MiB test file in DIR, read its
 *                             blocks back raw from the partition, compare
 *   ioscont map FILE          print FILE's extents (partition byte offsets)
 *   ioscont alloc FILE SIZE   preallocate FILE (SIZE in GiB) and print the map
 *
 * Read-only towards the partition: the raw block device is only opened
 * O_RDONLY. Writes go through the filesystem, into the named file only.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define TEST_SIZE	(64u << 20)
#define MAX_EXT		4096
#define EXT4_MAGIC	0xef53
#define F2FS_MAGIC	0xf2f52010

static const char *userdata_dev(void)
{
	static const char *c[] = {
		"/dev/block/bootdevice/by-name/userdata", "/dev/block/by-name/userdata", NULL,
	};
	if (getenv("IOSCONT_DEV"))	/* testing: the device holding the filesystem */
		return getenv("IOSCONT_DEV");
	for (int i = 0; c[i]; i++)
		if (access(c[i], R_OK) == 0)
			return c[i];
	return NULL;
}

static struct fiemap *get_map(int fd)
{
	size_t sz = sizeof(struct fiemap) + MAX_EXT * sizeof(struct fiemap_extent);
	struct fiemap *fm = calloc(1, sz);
	fm->fm_length = FIEMAP_MAX_OFFSET;
	fm->fm_flags = FIEMAP_FLAG_SYNC;
	fm->fm_extent_count = MAX_EXT;
	if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
		perror("FIEMAP");
		exit(1);
	}
	return fm;
}

static int print_map(int fd)
{
	struct fiemap *fm = get_map(fd);
	int bad = 0;
	printf("# extents: %u (logical physical length, bytes; physical is from the start of userdata)\n",
	       fm->fm_mapped_extents);
	for (unsigned i = 0; i < fm->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &fm->fm_extents[i];
		printf("extent 0x%llx 0x%llx 0x%llx flags 0x%x\n", (unsigned long long)e->fe_logical,
		       (unsigned long long)e->fe_physical, (unsigned long long)e->fe_length, e->fe_flags);
		if (e->fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_ENCODED |
				   FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED |
				   FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_DATA_TAIL))
			bad = 1;
	}
	if (fm->fm_mapped_extents == MAX_EXT)
		printf("# WARNING: extent table full, map truncated\n");
	free(fm);
	return bad;
}

static void fs_info(const char *path)
{
	struct statfs sf;
	if (statfs(path, &sf) == 0)
		printf("filesystem: %s, free %llu MiB\n",
		       (unsigned long)sf.f_type == EXT4_MAGIC ? "ext4" :
		       (unsigned long)sf.f_type == F2FS_MAGIC ? "f2fs (needs pinning)" : "other",
		       (unsigned long long)sf.f_bavail * sf.f_bsize >> 20);
}

static int cmd_check(const char *dir)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/ioscont-test.bin", dir);
	fs_info(dir);
	const char *dev = userdata_dev();
	if (!dev) {
		fprintf(stderr, "userdata block device not found\n");
		return 1;
	}

	/* pattern: every 8 bytes = offset ^ magic, so a misplaced block is also caught */
	uint64_t *buf;
	if (posix_memalign((void **)&buf, 4096, TEST_SIZE))
		return 1;
	for (size_t i = 0; i < TEST_SIZE / 8; i++)
		buf[i] = (i * 8) ^ 0x694f53636f6e74ULL;
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, buf, TEST_SIZE) != TEST_SIZE || fsync(fd)) {
		perror(path);
		return 1;
	}
	printf("test file: %s\n", path);
	int flagged = print_map(fd);

	int raw = open(dev, O_RDONLY | O_DIRECT);
	if (raw < 0) {
		perror(dev);
		return 1;
	}
	struct fiemap *fm = get_map(fd);
	uint8_t *rb;
	if (posix_memalign((void **)&rb, 4096, 1 << 20))
		return 1;
	uint64_t checked = 0, mismatched = 0;
	for (unsigned i = 0; i < fm->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &fm->fm_extents[i];
		for (uint64_t off = 0; off < e->fe_length; off += 1 << 20) {
			uint64_t n = e->fe_length - off < (1 << 20) ? e->fe_length - off : (1 << 20);
			uint64_t lo = e->fe_logical + off;
			if (lo >= TEST_SIZE)
				break;
			if (lo + n > TEST_SIZE)
				n = TEST_SIZE - lo;
			if (pread(raw, rb, (n + 4095) & ~4095ULL, e->fe_physical + off) < (ssize_t)n) {
				perror("raw read");
				return 1;
			}
			checked += n;
			if (memcmp(rb, (uint8_t *)buf + lo, n))
				mismatched += n;
		}
	}
	close(raw);
	close(fd);
	unlink(path);
	printf("raw check: %llu of %u bytes compared, %llu mismatched\n",
	       (unsigned long long)checked, TEST_SIZE, (unsigned long long)mismatched);
	if (checked == TEST_SIZE && mismatched == 0 && !flagged) {
		printf("RESULT: OK, files in %s are stored as plain blocks: a container works here\n", dir);
		return 0;
	}
	printf("RESULT: NO, %s\n", mismatched ? "blocks on disk differ (encrypted directory?)"
					     : "extents incomplete or flagged");
	return 2;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "check"))
		return cmd_check(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "map")) {
		int fd = open(argv[2], O_RDONLY);
		if (fd < 0) {
			perror(argv[2]);
			return 1;
		}
		return print_map(fd) ? 2 : 0;
	}
	if (argc == 4 && !strcmp(argv[1], "alloc")) {
		off_t size = (off_t)strtoull(argv[3], NULL, 0) << 30;
		int fd = open(argv[2], O_RDWR | O_CREAT, 0600);
		if (fd < 0 || fallocate(fd, 0, 0, size)) {
			perror(argv[2]);
			return 1;
		}
		fs_info(argv[2]);
		return print_map(fd) ? 2 : 0;
	}
	fprintf(stderr, "usage: ioscont check DIR | map FILE | alloc FILE GIB\n");
	return 1;
}
