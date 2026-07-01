#define _POSIX_C_SOURCE 200809L

#include "extract.h"

#include <archive.h>
#include <archive_entry.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int extract_path_safe(const char *path)
{
	if (!path || !*path) return 0;
	if (path[0] == '/')  return 0;

	const char *p = path;
	while (*p) {

		while (p[0] == '.' && p[1] == '/') p += 2;
		if (p[0] == '.' && p[1] == '.' &&
		    (p[2] == '\0' || p[2] == '/'))         return 0;

		while (*p && *p != '/') p++;
		if (*p == '/') p++;
	}
	return 1;
}

static int entry_type_allowed(mode_t filetype)
{
	return filetype == AE_IFREG || filetype == AE_IFDIR;
}

extract_status_t extract_tarball(const char *archive_path, const char *dest_dir)
{
	if (!archive_path || !dest_dir) return EXTRACT_ERR_INTERNAL;

	struct archive *a = archive_read_new();
	struct archive *w = archive_write_disk_new();
	if (!a || !w) {
		if (a) archive_read_free(a);
		if (w) archive_write_free(w);
		return EXTRACT_ERR_INTERNAL;
	}

	int wflags = ARCHIVE_EXTRACT_TIME
	           | ARCHIVE_EXTRACT_NO_OVERWRITE
	           | ARCHIVE_EXTRACT_SECURE_NODOTDOT
	           | ARCHIVE_EXTRACT_SECURE_SYMLINKS
	           | ARCHIVE_EXTRACT_NO_AUTODIR;
	archive_write_disk_set_options(w, wflags);
	archive_write_disk_set_standard_lookup(w);

	archive_read_support_format_tar(a);
	archive_read_support_filter_gzip(a);
	archive_read_support_filter_bzip2(a);
	archive_read_support_filter_xz(a);
	archive_read_support_filter_zstd(a);

	if (archive_read_open_filename(a, archive_path, 10240) != ARCHIVE_OK) {
		archive_read_free(a);
		archive_write_free(w);
		return EXTRACT_ERR_OPEN;
	}

	extract_status_t rc = EXTRACT_OK;
	uid_t my_uid = getuid();
	gid_t my_gid = getgid();

	struct archive_entry *entry = NULL;
	while (1) {
		int r = archive_read_next_header(a, &entry);
		if (r == ARCHIVE_EOF) break;
		if (r < ARCHIVE_WARN) { rc = EXTRACT_ERR_OPEN; break; }

		const char *raw_path = archive_entry_pathname(entry);
		mode_t       ftype   = archive_entry_filetype(entry);
		mode_t       perm    = archive_entry_perm(entry);

		if (!extract_path_safe(raw_path)) { rc = EXTRACT_ERR_PATH_TRAVERSAL; break; }
		if (!entry_type_allowed(ftype))   { rc = EXTRACT_ERR_FORBIDDEN_TYPE; break; }

		if (archive_entry_hardlink(entry) != NULL) { rc = EXTRACT_ERR_FORBIDDEN_TYPE; break; }
		if (archive_entry_symlink(entry)  != NULL) { rc = EXTRACT_ERR_FORBIDDEN_TYPE; break; }

		archive_entry_set_perm(entry, perm & 0777);
		archive_entry_set_uid(entry, (la_int64_t)my_uid);
		archive_entry_set_gid(entry, (la_int64_t)my_gid);

		size_t dlen = strlen(dest_dir);
		size_t plen = strlen(raw_path);
		size_t need = dlen + 1 + plen + 1;
		char  *full = (char *)malloc(need);
		if (!full) { rc = EXTRACT_ERR_INTERNAL; break; }
		snprintf(full, need, "%s/%s", dest_dir, raw_path);
		archive_entry_set_pathname(entry, full);
		free(full);

		if (archive_write_header(w, entry) != ARCHIVE_OK) {
			rc = EXTRACT_ERR_WRITE; break;
		}

		if (ftype == AE_IFREG) {
			const void *buf = NULL;
			size_t size = 0;
			la_int64_t offset = 0;
			while ((r = archive_read_data_block(a, &buf, &size, &offset)) == ARCHIVE_OK) {
				if (archive_write_data_block(w, buf, size, offset) < 0) {
					rc = EXTRACT_ERR_WRITE; break;
				}
			}
			if (rc != EXTRACT_OK) break;
			if (r != ARCHIVE_EOF && r < ARCHIVE_WARN) { rc = EXTRACT_ERR_WRITE; break; }
		}

		if (archive_write_finish_entry(w) < ARCHIVE_WARN) {
			rc = EXTRACT_ERR_WRITE; break;
		}
	}

	archive_read_close(a);
	archive_read_free(a);
	archive_write_close(w);
	archive_write_free(w);
	return rc;
}
