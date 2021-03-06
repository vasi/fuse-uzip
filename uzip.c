#define FUSE_USE_VERSION 26
#include <fuse.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zlib.h>
#include <lzma.h>

static const char *uzip_sufs[] = { ".uzip", ".ulzma" };
static const char uzip_magic[] = "#!/bin/sh\n#";
static const size_t magic_len = sizeof(uzip_magic) - 1;
static const size_t uzip_header_offset = 128;

typedef struct {
	uint32_t blocksize;
	uint32_t blocks;
} uzip_header;

typedef int (*uzip_decomp)(char *in, int ilen, char *out, int olen);

typedef struct {
	int fd;
	char *name;
	uzip_header header;
	uint64_t *offsets;
	uzip_decomp decomp;
} uzip;

static uint64_t uzip_ntohll(uint64_t n) {
	return ((uint64_t)ntohl(n) << 32) | (uint32_t)ntohl(n >> 32);
}

static void *uzip_init(struct fuse_conn_info *conn) {
	return fuse_get_context()->private_data;
}

static void uzip_destroy(void *data) {
	uzip *u = data;
	close(u->fd);
	free(u->name);
	free(u->offsets);
}

static int uzip_getattr(const char *path, struct stat *st) {
	uzip *u = fuse_get_context()->private_data;
	memset(st, 0, sizeof(*st));
	if (strcmp(path, "/") == 0) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
	} else if (path[0] == '/' && strcmp(path + 1, u->name) == 0) {
		int err = fstat(u->fd, st);
		st->st_size = u->header.blocks * u->header.blocksize;
		st->st_blksize = u->header.blocksize;
		return err;
	} else {
		return -ENOENT;
	}
	return 0;
}

static int uzip_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi) {
	if (strcmp(path, "/") != 0)
		return -ENOENT;
	
	uzip *u = fuse_get_context()->private_data;
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	filler(buf, u->name, NULL, 0);
	return 0;
}

static int uzip_open(const char *path, struct fuse_file_info *fi) {
	uzip *u = fuse_get_context()->private_data;
	if (path[0] != '/' || strcmp(path + 1, u->name) != 0)
		return -ENOENT;
	if ((fi->flags & O_RDWR) || (fi->flags & O_WRONLY))
		return -EACCES;
	return 0;
}

static int zlib_decomp(char *in, int ilen, char *out, int olen) {
	uLongf dlen = olen;
	int err = uncompress((Bytef*)out, &dlen, (Bytef*)in, ilen);
	if (err != Z_OK)
		return 0;
	return (int)dlen;
}

static int lzma_decomp(char *in, int ilen, char *out, int olen) {
	uint64_t memlimit = UINT64_MAX;
	size_t ipos = 0, opos = 0;
	lzma_ret err = lzma_stream_buffer_decode(&memlimit, 0, NULL,
		(uint8_t*)in, &ipos, ilen, (uint8_t*)out, &opos, olen);
	if (err != LZMA_OK || ipos != ilen)
		return 0;
	return opos;
}

static char *uzip_block(uzip *u, size_t n) {
	if (n > u->header.blocks) {
		fprintf(stderr, "Invalid block\n");
		return NULL;
	}
	
	size_t size = u->offsets[n+1] - u->offsets[n];
	size_t osize = u->header.blocksize;
	char *comp = NULL, *ucomp = NULL;
	if (!(ucomp = calloc(1, osize))) {
		fprintf(stderr, "Can't allocate block memory\n");
		goto error;
	}
	if (size == 0)
		return ucomp;
	
	if (!(comp = malloc(size))) {
		fprintf(stderr, "Can't allocate block data\n");
		goto error;
	}
	if (pread(u->fd, comp, size, u->offsets[n]) != size) {
		perror("Reading block");
		goto error;
	}
	
	
	if (u->decomp(comp, size, ucomp, osize) != osize) {
		fprintf(stderr, "Decompression error\n");
		goto error;
	}
	
	free(comp);
	return ucomp;
	
error:
	free(comp);
	free(ucomp);
	return NULL;
}

static int uzip_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi) {
	uzip *u = fuse_get_context()->private_data;
	if (path[0] != '/' || strcmp(path + 1, u->name) != 0)
		return -ENOENT;
	
	size_t read = 0;
	while (size > 0) {
		size_t block = offset / u->header.blocksize;
		if (block >= u->header.blocks)
			break;
		
		size_t off = offset % u->header.blocksize;
		size_t to_read = u->header.blocksize - off;
		if (size < to_read)
			to_read = size;
		
		size_t csize = u->offsets[block+1] - u->offsets[block];
		if (csize > 0) {
			char *data = uzip_block(u, block);
			if (!data)
				return -EIO;
			memcpy(buf + read, data + off, to_read);
		}
		
		read += to_read;
		offset += to_read;
		size -= to_read;
	}
	return read;
}

static struct fuse_operations uzip_ops = {
	.getattr	= uzip_getattr,
	.readdir	= uzip_readdir,
	.open		= uzip_open,
	.read		= uzip_read,
	.init		= uzip_init,
	.destroy	= uzip_destroy,
};

static int uzip_opt_proc(void *data, const char *arg, int key,
		struct fuse_args *outargs) {
	char **file = (char**)data;
	if (key == FUSE_OPT_KEY_NONOPT) {
		if (*file)
			return 1; // mountpoint
		*file = strdup(arg);
		return 0;
	}
	return 1;
}

int main(int argc, char *argv[]) {
	char *file = NULL;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	fuse_opt_parse(&args, &file, NULL, uzip_opt_proc);
	
	if (!file) {
		fprintf(stderr, "No uzip file given\n");
		return -1;
	}
	
	uzip u;
	u.name = strdup(basename(file));
	for (int i = 0; i < sizeof(uzip_sufs) / sizeof(uzip_sufs[0]); ++i) {
		size_t suf_pos = strlen(u.name) - strlen(uzip_sufs[i]);
		if (strcmp(uzip_sufs[i], u.name + suf_pos) == 0) {
			u.name[suf_pos] = '\0';
			break;
		}
	}
	
	u.fd = open(file, O_RDONLY);
	free(file);
	if (u.fd == -1) {
		perror("Opening uzip file");
		return -1;
	}
	
	char header[magic_len + 2];
	if (read(u.fd, header, sizeof(header)) != sizeof(header)
			|| memcmp(header, uzip_magic, magic_len) != 0) {
		fprintf(stderr, "Not a uzip file\n");
		return -1;
	}
	
	char type = header[magic_len];
	int version = header[magic_len + 1];
	int min_vers = 2;
	switch (type) {
		case 'V':
			u.decomp = &zlib_decomp;
			break;
		case 'L':
			min_vers = 3;
			u.decomp = &lzma_decomp;
			break;
		default:
			fprintf(stderr, "Bad uzip type\n");
			return -1;
	}
	if (version > '9' || (version - '0') < min_vers) {
		fprintf(stderr, "Bad uzip version\n");
		return -1;
	}
	
	if (lseek(u.fd, uzip_header_offset, SEEK_SET) == -1) {
		perror("Seeking to header");
		return -1;
	}
	if (read(u.fd, &u.header, sizeof(u.header)) != sizeof(u.header)) {
		perror("Reading header");
		return -1;		
	}
	u.header.blocksize = ntohl(u.header.blocksize);
	u.header.blocks = ntohl(u.header.blocks);
	size_t offsize = (u.header.blocks + 1) * sizeof(uint64_t);
	if (!(u.offsets = malloc(offsize))) {
		fprintf(stderr, "Out of memory allocating blocks\n");
		return -1;
	}
	if (read(u.fd, u.offsets, offsize) != offsize) {
		perror("Reading offsets");
		return -1;		
	}
	for (size_t i = 0; i <= u.header.blocks; ++i)
		u.offsets[i] = uzip_ntohll(u.offsets[i]);
	
	return fuse_main(args.argc, args.argv, &uzip_ops, &u);
}
