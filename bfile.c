#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "fs.h"

void bfile_close(struct bfile *f)
{
	if (f->data != MAP_FAILED) {
		munmap(f->data, f->size);
		f->data = MAP_FAILED;
	}
	if (f->fd != -1) {
		close(f->fd);
		f->fd = -1;
	}
}

void bfile_init(struct bfile *f)
{
	f->fd = -1;
	f->data = MAP_FAILED;
	f->name = NULL;
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

int bfile_snprint_error(char *err, size_t errsz, int code)
{
	switch (code) {
	case BFE_SUCCESS:
		return snprintf(err, errsz, "Success");
	case BFE_OPEN:
		return snprintf(err, errsz, "Can't open: %s", strerror(errno));
	case BFE_ACCESS:
		return snprintf(err, errsz, "Can't access: %s", strerror(errno));
	case BFE_MAP:
		return snprintf(err, errsz, "Can't map: %s", strerror(errno));
	case BFE_EMPTY:
		return snprintf(err, errsz, "File empty");
	case BFE_READONLY:
		return snprintf(err, errsz, "Operation not permitted: readonly file");
	case BFE_TRUNCATE:
		return snprintf(err, errsz, "Can't truncate: %s", strerror(errno));
	case BFE_IO:
		return snprintf(err, errsz, "I/O broken: %s", strerror(errno));
	case BFE_SYNC:
		return snprintf(err, errsz, "Sync error: %s", strerror(errno));
	case BFE_RESIZE:
		return snprintf(err, errsz, "Truncating not permitted: file non-empty");
	default:
		return snprintf(err, errsz, "Unknown error: %d", code);
	}
}

int bfile_open(struct bfile *f, const char *name, mode_t mode, uint64_t fsize)
{
	int err = 0, fd = -1, prot = PROT_READ;
	int oflags = O_RDWR, excl = 0;
	unsigned flags = 0;
	void *data;
	size_t size;
	if (fsize)
		oflags |= O_CREAT | O_EXCL;
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
		if (fsize) {
			fd = open(name, O_RDWR, mode);
			if (fd == -1) {
				err = BFE_OPEN;
				goto fail;
			}
			if (fstat(fd, &f->st)) {
				err = BFE_ACCESS;
				goto fail;
			}
			if (f->st.st_size) {
				err = BFE_RESIZE;
				goto fail;
			}
			goto resize;
		}
		fd = open(name, O_RDONLY, mode);
		if (fd == -1) {
			err = BFE_OPEN;
			goto fail;
		}
		flags |= BF_READONLY;
	}
	if (mode && fsize)
		excl = 1;
resize:
	/* resize file if it has been created or if the file is empty */
	if (mode && fsize && ftruncate(fd, fsize)) {
		err = BFE_TRUNCATE;
		goto fail;
	}
	if (fstat(fd, &f->st)) {
		err = BFE_ACCESS;
		goto fail;
	}
	if (!f->st.st_size) {
		err = BFE_EMPTY;
		goto fail;
	}
	if (!(flags & BF_READONLY))
		prot |= PROT_WRITE;
	data = mmap(NULL, size = f->st.st_size, prot, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		err = BFE_MAP;
		goto fail;
	}
	f->fd = fd;
	f->flags = flags;
	f->mode = mode;
	f->name = name;
	f->data = data;
	f->size = size;
	err = 0;
fail:
	if (err) {
		if (excl && fd != -1)
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
	if (f->size == size)
		return 0;
	if (ftruncate(f->fd, size))
		return BFE_TRUNCATE;
	size_t oldsize = f->size;
	/* mremap ensures that data is kept intact even if it has been moved */
	void *map = mremap(f->data, f->size, size, MREMAP_MAYMOVE);
	if (map == MAP_FAILED)
		return BFE_MAP;
	/* update mapping */
	f->data = map;
	f->size = size;
	if (msync(f->data, size, MS_SYNC)) {
		/*
		 * journaling may not be supported by the underlying filesystem
		 * try to revert to old state
		 */
		void *oldmap = mremap(f->data, size, oldsize, MREMAP_MAYMOVE);
		if (oldmap != MAP_FAILED) {
			f->data = oldmap;
			f->size = oldsize;
			return BFE_SYNC;
		}
		/* give up, something is terribly broken */
		f->flags |= BF_READONLY;
		return BFE_IO;
	}
	if (fstat(f->fd, &f->st))
		return BFE_ACCESS;
	return 0;
}
