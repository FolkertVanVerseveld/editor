/* Copyright 2017 Folkert van Verseveld. See COPYING for details */
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
	struct stat st;
	const char *name;
	int prot;
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

void bfile_init(struct bfile *f);
/*
 * open a binary file for editing.
 *
 * the specified MODE determines the permissions for the created file. if MODE
 * is zero, the file is opened in readonly mode.
 */
int bfile_open(struct bfile *f, const char *name, mode_t mode);
void bfile_close(struct bfile *f);

size_t bfile_size(const struct bfile *f);
/* operations */
int bfile_truncate(struct bfile *f, uint64_t size);
void bfile_showinfo(const struct bfile *f);
void bfile_peek(const struct bfile *f, uint64_t start, unsigned length);
/* error handling */
void bfile_print_error(FILE *f, const char *name, int code);
int bfile_snprint_error(char *str, size_t size, int code);
/* check whether the file is readable and writeable */
int bfile_is_rdwr(const struct bfile *f);
int bfile_is_rdwr2(const struct bfile *f, const char *op);
/* open mapping if not open */
int bfile_map(struct bfile *f);

#endif
