/* lcfs
   Copyright (C) 2021 Giuseppe Scrivano <giuseppe@scrivano.org>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE

#include "config.h"

#include "libcomposefs/lcfs-writer.h"

#include <stdio.h>
#include <linux/limits.h>
#include <string.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <getopt.h>
#include <libfsverity.h>

static void digest_to_string(const uint8_t *csum, char *buf)
{
	static const char hexchars[] = "0123456789abcdef";
	uint32_t i, j;

	for (i = 0, j = 0; i < 32; i++, j += 2) {
		uint8_t byte = csum[i];
		buf[j] = hexchars[byte >> 4];
		buf[j + 1] = hexchars[byte & 0xF];
	}
	buf[j] = '\0';
}

static void digest_to_path(const uint8_t *csum, char *buf)
{
	static const char hexchars[] = "0123456789abcdef";
	uint32_t i, j;

	for (i = 0, j = 0; i < LCFS_DIGEST_SIZE; i++, j += 2) {
		uint8_t byte = csum[i];
		if (i == 1)
			buf[j++] = '/';
		buf[j] = hexchars[byte >> 4];
		buf[j + 1] = hexchars[byte & 0xF];
	}
	buf[j] = '\0';
}

static int ensure_dir(const char *path, mode_t mode)
{
	struct stat buf;

	/* We check this ahead of time, otherwise
	   the mkdir call can fail in the read-only
	   case with EROFS instead of EEXIST on some
	   filesystems (such as NFS) */
	if (stat(path, &buf) == 0) {
		if (!S_ISDIR(buf.st_mode)) {
			errno = ENOTDIR;
			return -1;
		}

		return 0;
	}

	if (mkdir(path, mode) == -1 && errno != EEXIST)
		return -1;

	return 0;
}

static int mkdir_parents(const char *pathname, int mode)
{
	char *fn = NULL;
	char *p;

	fn = strdup(pathname);
	if (fn == NULL) {
		errno = ENOMEM;
		return -1;
	}

	p = fn;
	while (*p == '/')
		p++;

	do {
		while (*p && *p != '/')
			p++;

		if (!*p)
			break;
		*p = '\0';

		if (ensure_dir(fn, mode) != 0) {
			free(fn);
			return -1;
		}

		*p++ = '/';
		while (*p && *p == '/')
			p++;
	} while (p);

	free(fn);
	return 0;
}

static int write_to_fd(int fd, const char *content, ssize_t len)
{
	ssize_t res;

	while (len > 0) {
		res = write(fd, content, len);
		if (res < 0 && errno == EINTR)
			continue;
		if (res <= 0) {
			if (res ==
			    0) /* Unexpected short write, should not happen when writing to a file */
				errno = ENOSPC;
			return -1;
		}
		len -= res;
		content += res;
	}

	return 0;
}

#define BUFSIZE 8192
static int copy_file_data(int sfd, int dfd)
{
	char buffer[BUFSIZE];
	ssize_t bytes_read;

	while (true) {
		bytes_read = read(sfd, buffer, BUFSIZE);
		if (bytes_read == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (bytes_read == 0)
			break;

		if (write_to_fd(dfd, buffer, bytes_read) != 0)
			return -1;
	}

	return 0;
}

static inline void cleanup_freep(void *p)
{
	void **pp = (void **)p;
	free(*pp);
}
#define cleanup_free __attribute__((cleanup(cleanup_freep)))

static int join_paths(char **out, const char *path1, const char *path2)
{
	const char *sep = (path1[0] == '\0') ? "" : "/";
	int len = strlen(path1);

	while (len && path1[len - 1] == '/')
		len--;

	return asprintf(out, "%.*s%s%s", len, path1, sep, path2);
}

static int copy_file_with_dirs_if_needed(const char *src, const char *dst_base,
					 const char *dst, mode_t mode,
					 bool try_enable_fsverity)
{
	cleanup_free char *pathbuf = NULL;
	cleanup_free char *tmppath = NULL;
	int ret, res;
	int sfd, dfd;
	int errsv;
	struct stat statbuf;

	ret = join_paths(&pathbuf, dst_base, dst);
	if (ret < 0)
		return ret;

	ret = mkdir_parents(pathbuf, 0755);
	if (ret < 0)
		return ret;

	if (lstat(pathbuf, &statbuf) == 0)
		return 0; /* Already exists, no need to copy */

	ret = join_paths(&tmppath, dst_base, ".tmpXXXXXX");
	if (ret < 0)
		return ret;

	dfd = mkostemp(tmppath, O_CLOEXEC);
	if (dfd == -1)
		return -1;

	sfd = open(src, O_CLOEXEC | O_RDONLY);
	if (sfd == -1) {
		errsv = errno;
		unlink(tmppath);
		close(dfd);
		errno = errsv;
		return -1;
	}

	res = copy_file_data(sfd, dfd);
	if (res < 0) {
		errsv = errno;
		unlink(tmppath);
		close(sfd);
		close(dfd);
		errno = errsv;
		return res;
	}
	close(sfd);

	res = fsync(dfd);
	if (res < 0) {
		errsv = errno;
		unlink(tmppath);
		close(dfd);
		errno = errsv;
		return res;
	}
	close(dfd);

	if (try_enable_fsverity) {
		/* Try to enable fsverity */
		dfd = open(tmppath, O_CLOEXEC | O_RDONLY);
		if (dfd < 0) {
			errsv = errno;
			unlink(tmppath);
			close(dfd);
			errno = errsv;
			return res;
		}

		if (fstat(dfd, &statbuf) == 0) {
			struct libfsverity_merkle_tree_params params = {
				1,
				FS_VERITY_HASH_ALG_SHA256,
				statbuf.st_size,
				4096,
				0,
				NULL
			};

			res = libfsverity_enable(dfd, &params);
			if (res < 0) {
				/* Ignore errors, we're only trying to enable it */
			}
		}
		close(dfd);
	}

	res = rename(tmppath, pathbuf);
	if (res < 0) {
		errsv = errno;
		unlink(tmppath);
		errno = errsv;
		return res;
	}

	return 0;
}

static int fill_payload(struct lcfs_node_s *node, const char *path, size_t len,
			size_t path_start_offset, bool by_digest,
			const char *digest_store_path)
{
	cleanup_free char *tmp_path = NULL;
	const char *fname;
	int ret;

	fname = lcfs_node_get_name(node);

	if (fname) {
		ret = join_paths(&tmp_path, path, fname);
		if (ret < 0)
			return ret;
		len += ret;
		path = tmp_path;
	}

	if (lcfs_node_dirp(node)) {
		size_t i, n_children;

		n_children = lcfs_node_get_n_children(node);
		for (i = 0; i < n_children; i++) {
			struct lcfs_node_s *child =
				lcfs_node_get_child(node, i);
			ret = fill_payload(child, path, len, path_start_offset,
					   by_digest, digest_store_path);
			if (ret < 0)
				return ret;
		}
	} else if ((lcfs_node_get_mode(node) & S_IFMT) == S_IFLNK) {
		char target[PATH_MAX + 1];
		ssize_t s = readlink(path, target, sizeof(target));
		if (s < 0)
			return ret;

		target[s] = '\0';
		ret = lcfs_node_set_payload(node, target);
		if (ret < 0)
			return ret;
	} else if ((lcfs_node_get_mode(node) & S_IFMT) == S_IFREG) {
		const uint8_t *digest = NULL;

		if (by_digest)
			digest = lcfs_node_get_fsverity_digest(node);

		if (digest) { /* Zero size files don't have a digest (since they are non-backed */
			char digest_path[LCFS_DIGEST_SIZE * 2 + 2];
			digest_to_path(digest, digest_path);

			if (digest_store_path) {
				ret = copy_file_with_dirs_if_needed(
					path, digest_store_path, digest_path,
					0644, true);
				if (ret < 0)
					return ret;
			}

			ret = lcfs_node_set_payload(node, digest_path);
		} else {
			ret = lcfs_node_set_payload(node,
						    path + path_start_offset);
		}
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--use-epoch] [--skip-xattrs] [--absolute] [--by-digest] [--digest-store=path] [--print-digest] [--skip-devices] [--compute-digest] SOURCEDIR IMAGE\n",
		argv0);
}

#define OPT_ABSOLUTE 100
#define OPT_SKIP_XATTRS 102
#define OPT_USE_EPOCH 103
#define OPT_SKIP_DEVICES 104
#define OPT_COMPUTE_DIGEST 106
#define OPT_BY_DIGEST 107
#define OPT_DIGEST_STORE 108
#define OPT_PRINT_DIGEST 109

static ssize_t write_cb(void *_file, void *buf, size_t count)
{
	FILE *file = _file;

	return fwrite(buf, 1, count, file);
}

int main(int argc, char **argv)
{
	const struct option longopts[] = {
		{
			name: "absolute",
			has_arg: no_argument,
			flag: NULL,
			val: OPT_ABSOLUTE
		},
		{
			name: "skip-xattrs",
			has_arg: no_argument,
			flag: NULL,
			val: OPT_SKIP_XATTRS
		},
		{
			name: "skip-devices",
			has_arg: no_argument,
			flag: NULL,
			val: OPT_SKIP_DEVICES
		},
		{
			name: "use-epoch",
			has_arg: no_argument,
			flag: NULL,
			val: OPT_USE_EPOCH
		},
		{
			name: "compute-digest",
			has_arg: no_argument,
			flag: NULL,
			val: OPT_COMPUTE_DIGEST
		},
		{
			name: "by-digest",
			has_arg: no_argument,
			flag: NULL,
			val: OPT_BY_DIGEST
		},
		{
			name: "digest-store",
			has_arg: required_argument,
			flag: NULL,
			val: OPT_DIGEST_STORE
		},
		{
			name: "print-digest",
			has_arg: no_argument,
			flag: NULL,
			val: OPT_PRINT_DIGEST
		},
		{},
	};
	const char *bin = argv[0];
	int buildflags = 0;
	bool absolute_path = false;
	bool by_digest = false;
	bool print_digest = false;
	struct lcfs_node_s *root;
	const char *out = NULL;
	const char *dir_path = NULL;
	const char *digest_store_path = NULL;
	size_t path_start_offset;
	cleanup_free char *pathbuf = NULL;
	uint8_t digest[LCFS_DIGEST_SIZE];
	int opt;
	FILE *out_file;
	char *failed_path;

	while ((opt = getopt_long(argc, argv, ":CR", longopts, NULL)) != -1) {
		switch (opt) {
		case OPT_USE_EPOCH:
			buildflags |= LCFS_BUILD_USE_EPOCH;
			break;
		case OPT_SKIP_XATTRS:
			buildflags |= LCFS_BUILD_SKIP_XATTRS;
			break;
		case OPT_SKIP_DEVICES:
			buildflags |= LCFS_BUILD_SKIP_DEVICES;
			break;
		case OPT_COMPUTE_DIGEST:
			buildflags |= LCFS_BUILD_COMPUTE_DIGEST;
			break;
		case OPT_ABSOLUTE:
			absolute_path = true;
			break;
		case OPT_BY_DIGEST:
			by_digest = true;
			break;
		case OPT_DIGEST_STORE:
			digest_store_path = optarg;
			break;
		case OPT_PRINT_DIGEST:
			print_digest = true;
			break;
		case ':':
			fprintf(stderr, "option needs a value\n");
			exit(EXIT_FAILURE);
		default:
			usage(bin);
			exit(1);
		}
	}

	argv += optind;
	argc -= optind;

	if (argc < 1) {
		fprintf(stderr, "No source path specified\n");
		usage(bin);
		exit(1);
	}

	if (argc != 2) {
		fprintf(stderr, "No destination path specified\n");
		usage(bin);
		exit(1);
	}

	dir_path = argv[0];
	out = argv[1];

	if (digest_store_path != NULL)
		by_digest = true; /* implied */

	if (by_digest)
		buildflags |= LCFS_BUILD_COMPUTE_DIGEST; /* implied */

	if (absolute_path && by_digest)
		error(EXIT_FAILURE, 0,
		      "Can't specify both --absolute and --by-digest");

	if (strcmp(out, "-") == 0) {
		if (isatty(1))
			error(EXIT_FAILURE, 0,
			      "stdout is a tty.  Refusing to use it");
		out_file = stdout;
	} else {
		out_file = fopen(out, "w");
		if (out_file == NULL)
			error(EXIT_FAILURE, errno,
			      "failed to open output file");
	}

	root = lcfs_build(AT_FDCWD, dir_path, "", buildflags, &failed_path);
	if (root == NULL)
		error(EXIT_FAILURE, errno, "error accessing %s", failed_path);

	if (absolute_path) {
		cleanup_free char *cwd_cleanup = NULL;
		cleanup_free char *tmp_pathbuf = NULL;
		cleanup_free char *absolute_prefix = NULL;
		char *cwd = "";
		int r;

		if (dir_path[0] != '/') {
			cwd = cwd_cleanup = get_current_dir_name();
			if (cwd == NULL)
				error(EXIT_FAILURE, errno,
				      "retrieve current working directory");
		}
		r = join_paths(&tmp_pathbuf, cwd, dir_path);
		if (r < 0)
			error(EXIT_FAILURE, errno, "compute directory path");

		absolute_prefix = canonicalize_file_name(tmp_pathbuf);
		if (absolute_prefix == NULL)
			error(EXIT_FAILURE, errno, "failed to canonicalize %s",
			      tmp_pathbuf);

		r = join_paths(&pathbuf, absolute_prefix, "");
		if (r < 0)
			error(EXIT_FAILURE, errno, "compute directory path");
		path_start_offset = 0;
	} else {
		if (dir_path[0] == '\0')
			error(EXIT_FAILURE, 0, "invalid directory path");
		path_start_offset = join_paths(&pathbuf, dir_path, "");
		if (path_start_offset < 0)
			error(EXIT_FAILURE, errno, "compute directory path");
	}
	fill_payload(root, pathbuf, strlen(pathbuf), path_start_offset,
		     by_digest, digest_store_path);

	if (lcfs_write_to(root, out_file, write_cb,
			  print_digest ? digest : NULL) < 0)
		error(EXIT_FAILURE, errno, "cannot write to stdout");

	if (print_digest) {
		char digest_str[LCFS_DIGEST_SIZE * 2 + 1] = { 0 };
		digest_to_string(digest, digest_str);
		printf("%s\n", digest_str);
	}

	lcfs_node_unref(root);
	return 0;
}
