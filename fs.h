#ifndef FS_H
#define FS_H

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

/* binary file */
struct bfile {
	int fd;
	mode_t mode;
	unsigned flags;
	void *data;
	size_t size;
	struct stat st;
	const char *name;
};

#define BF_READONLY 1
#define BF_ROWMAX 16

/* binary file error table */
#define BFE_SUCCESS  0
#define BFE_OPEN     1
#define BFE_ACCESS   2
#define BFE_MAP      3
/* file must be non-empty because mmap can't map 0 bytes */
#define BFE_EMPTY    4
#define BFE_READONLY 5
#define BFE_TRUNCATE 6
#define BFE_IO       7
#define BFE_SYNC     8
#define BFE_RESIZE   9

/*
 * open a binary file for editing. if SIZE is non-zero, it is implied that the
 * file must be either:
 * - created exclusively and truncated to that size.
 * - empty when it is opened and truncated to that size.
 *
 * the specified MODE determines the permissions for the created file. if SIZE
 * is zero, the specified name must exist. if MODE is zero, the file is opened
 * in readonly mode.
 */
int bfile_open(struct bfile *f, const char *name, mode_t mode, uint64_t size);
void bfile_close(struct bfile *f);
void bfile_init(struct bfile *f);
int bfile_truncate(struct bfile *f, uint64_t size);
void bfile_print_error(FILE *f, const char *name, int code);
int bfile_snprint_error(char *err, size_t errsz, int code);

#endif
