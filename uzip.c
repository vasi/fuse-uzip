#define FUSE_USE_VERSION 26
#include <fuse.h>

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
	int fd;
	char *name;
} uzip;

static void *uzip_init(struct fuse_conn_info *conn) {
	return fuse_get_context()->private_data;
}

static void uzip_destroy(void *data) {
	uzip *u = data;
	close(u->fd);
	free(u->name);
}

static int uzip_getattr(const char *path, struct stat *st) {
	uzip *u = fuse_get_context()->private_data;
	memset(st, 0, sizeof(*st));
	if (strcmp(path, "/") == 0) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
	} else if (path[0] == '/' && strcmp(path + 1, u->name) == 0) {
		int err = fstat(u->fd, st);
		// FIXME: size
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

static struct fuse_operations uzip_ops = {
	.getattr	= uzip_getattr,
	.readdir	= uzip_readdir,
	// .open		= uzip_open,
	// .read		= uzip_read,
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

static const char uzip_suf[] = ".uzip";

static const char uzip_magic[] = "#!/bin/sh\n#V";

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
	size_t suf_pos = strlen(u.name) - strlen(uzip_suf);
	if (strcmp(uzip_suf, u.name + suf_pos) == 0)
		u.name[suf_pos] = '\0';
	
	u.fd = open(file, O_RDONLY);
	free(file);
	if (u.fd == -1) {
		perror("Opening uzip file");
		return -1;
	}
	
	char magic[sizeof(uzip_magic)]; // including version char
	if (read(u.fd, magic, sizeof(magic)) != sizeof(magic)
			|| memcmp(magic, uzip_magic, sizeof(uzip_magic) - 1) != 0) {
		fprintf(stderr, "Not a uzip file\n");
		return -1;
	}
	if (magic[sizeof(uzip_magic) - 1] != '2') {
		fprintf(stderr, "Bad uzip version\n");
		return -1;		
	}
	
	return fuse_main(args.argc, args.argv, &uzip_ops, &u);
}
