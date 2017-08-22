/* Copyright 2017 Folkert van Verseveld. See COPYING for details */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#include "fs.h"

void bfile_init(struct bfile *f)
{
	f->fd = -1;
	f->data = MAP_FAILED;
	f->name = NULL;
}

void bfile_close(struct bfile *f)
{
	if (f->data != MAP_FAILED)
		munmap(f->data, bfile_size(f));
	if (f->fd != -1)
		close(f->fd);
	bfile_init(f);
}

size_t bfile_size(const struct bfile *f)
{
	return f->st.st_size;
}

void bfile_print_error(FILE *f, const char *name, int code)
{
	switch (code) {
	case BFE_SUCCESS:
		fprintf(f, "Success: %s\n", name);
		break;
	case BFE_OPEN:
		fprintf(f, "Can't open %s: %s\n", name, strerror(errno));
		break;
	case BFE_ACCESS:
		fprintf(f, "Can't access %s: %s\n", name, strerror(errno));
		break;
	case BFE_MAP:
		fprintf(f, "Can't map %s: %s\n", name, strerror(errno));
		break;
	case BFE_EMPTY:
		fprintf(f, "File empty: %s\n", name);
		break;
	case BFE_READONLY:
		fputs("Operation not permitted: readonly file\n", f);
		break;
	case BFE_TRUNCATE:
		fprintf(f, "Can't truncate: %s\n", strerror(errno));
		break;
	case BFE_IO:
		fprintf(f,
			"I/O broken: %s\nGoing into readonly mode!\n",
			strerror(errno)
		);
		break;
	case BFE_SYNC:
		fprintf(f,
			"Can't sync with: %s\nJournaling may be unsupported.\n"
			"Filesystems that do not support journaling are e.g.: fat, ext, nfts\n"
			"Sync error: %s\n",
			name,
			strerror(errno)
		);
		break;
	case BFE_RESIZE:
		fputs("Truncating not permitted: file non-empty\n", f);
		break;
	default:
		fprintf(f, "Unknown error: %d\n", code);
		break;
	}
}

int bfile_snprint_error(char *str, size_t size, int code)
{
	switch (code) {
	case BFE_SUCCESS:
		return snprintf(str, size, "Success");
	case BFE_OPEN:
		return snprintf(str, size, "Can't open: %s", strerror(errno));
	case BFE_ACCESS:
		return snprintf(str, size, "Can't access: %s", strerror(errno));
	case BFE_MAP:
		return snprintf(str, size, "Can't map: %s", strerror(errno));
	case BFE_EMPTY:
		return snprintf(str, size, "File empty");
	case BFE_READONLY:
		return snprintf(str, size, "Operation not permitted: readonly file");
	case BFE_TRUNCATE:
		return snprintf(str, size, "Can't truncate: %s", strerror(errno));
	case BFE_IO:
		return snprintf(str, size, "I/O broken: %s", strerror(errno));
	case BFE_SYNC:
		return snprintf(str, size, "Sync error: %s", strerror(errno));
	case BFE_RESIZE:
		return snprintf(str, size, "Truncating not permitted: file non-empty");
	default:
		return snprintf(str, size, "Unknown error: %d", code);
	}
}

int bfile_open(struct bfile *f, const char *name, mode_t mode)
{
	int err = 0, fd = -1, prot = PROT_READ, oflags = O_RDWR, new = 0;
	unsigned flags = 0;
	void *data = MAP_FAILED;
	size_t size;
	if (!mode) {
		oflags = O_RDONLY;
		flags |= BF_READONLY;
	}
	fd = open(name, oflags, mode);
	if (fd == -1) {
		if (!mode) {
			err = BFE_OPEN;
			goto fail;
		}
		fd = open(name, O_RDONLY, mode);
		if (fd == -1) {
			/*
			 * Permission denied or file does not exist.
			 * Try to create it or give up.
			 */
			if (!mode)
				goto err_open;
			fd = open(name, O_CREAT | O_EXCL | O_RDWR, mode);
			if (fd == -1) {
err_open:
				err = BFE_OPEN;
				goto fail;
			}
			new = 1;
		} else
			flags |= BF_READONLY;
	}
	if (fstat(fd, &f->st)) {
		err = BFE_ACCESS;
		goto fail;
	}
	if (!(flags & BF_READONLY))
		prot |= PROT_WRITE;
	if (f->st.st_size) {
		data = mmap(NULL, size = f->st.st_size, prot, MAP_SHARED, fd, 0);
		if (data == MAP_FAILED) {
			err = BFE_MAP;
			goto fail;
		}
	}
	/* opening successful, apply settings */
	f->fd = fd;
	f->flags = flags;
	f->mode = mode;
	f->name = name;
	f->data = data;
	f->prot = prot;
	err = 0;
fail:
	if (err) {
		if (new && fd != -1)
			unlink(name);
		if (fd != -1)
			close(fd);
	}
	return err;
}

int bfile_truncate(struct bfile *f, uint64_t size)
{
	if (f->flags & BF_READONLY)
		return BFE_READONLY;
	/* ignore if same */
	if (bfile_size(f) == size)
		return 0;
	if (ftruncate(f->fd, size))
		return BFE_TRUNCATE;
	size_t oldsize = bfile_size(f);
	/* close mapping if new size is 0 because mremap and mmap can't handle this */
	if (!size) {
		munmap(f->data, oldsize);
		f->data = MAP_FAILED;
		goto update_stat;
	}
	/* mremap ensures that data is kept intact even if it has been moved */
	void *map = oldsize ? mremap(f->data, bfile_size(f), size, MREMAP_MAYMOVE)
		: mmap(NULL, size, f->prot, MAP_SHARED, f->fd, 0);
	if (map == MAP_FAILED)
		return BFE_MAP;
	/* update mapping */
	f->data = map;
	if (msync(f->data, size, MS_SYNC)) {
		/*
		 * journaling may not be supported by the underlying filesystem
		 * try to revert to old state
		 */
		void *oldmap = mremap(f->data, size, oldsize, MREMAP_MAYMOVE);
		if (oldmap != MAP_FAILED) {
			f->data = oldmap;
			return BFE_SYNC;
		}
		/* give up, something is terribly broken */
		f->flags |= BF_READONLY;
		return BFE_IO;
	}
update_stat:
	if (fstat(f->fd, &f->st))
		return BFE_ACCESS;
	return 0;
}

int bfile_is_rdwr(const struct bfile *f)
{
	return (f->flags & BF_READONLY) == 0;
}

int bfile_is_rdwr2(const struct bfile *f, const char *op)
{
	if (!bfile_is_rdwr(f)) {
		fprintf(stderr, "Can't %s: readonly file\n", op);
		return 0;
	}
	return 1;
}

void bfile_showinfo(const struct bfile *f)
{
	const char *format = "%s, size: $%zX\n";
	if (!bfile_is_rdwr(f))
		format = "%s, size: $%zX (readonly)\n";
	printf(format, f->name, bfile_size(f));
}

void bfile_peek(const struct bfile *f, uint64_t start, unsigned length)
{
	unsigned row_counter = 0;
	const uint8_t *map = f->data;
	for (; length && start < bfile_size(f); ++start, --length) {
		printf(" %02" PRIX8, map[start]);
		if (++row_counter == BF_ROWMAX) {
			row_counter = 0;
			putchar('\n');
		}
	}
	for (; length; ++start, --length) {
		fputs(" ~~", stdout);
		if (++row_counter == BF_ROWMAX) {
			row_counter = 0;
			putchar('\n');
		}
	}
	if (row_counter)
		putchar('\n');
}

int bfile_map(struct bfile *f)
{
	if (!bfile_size(f)) {
		fputs("File empty\n", stderr);
		return 1;
	}
	void *map = mmap(NULL, bfile_size(f), f->prot, MAP_SHARED, f->fd, 0);
	if (map == MAP_FAILED) {
		perror("Can't map file data");
		return 1;
	}
	f->data = map;
	return 0;
}
