/**
 * @file extract.h
 * @brief Hardened tar extraction (libarchive) — TP1 policy.
 *
 * Allowed entry types : regular file, directory.
 * Rejected entry types: symlink, hardlink, char device, block device,
 *                       FIFO, socket — these can escape the extraction
 *                       sandbox or alias paths under the agent's control.
 * Path policy         : entries containing `..` components or absolute
 *                       paths (leading '/') are rejected.
 * Permission policy   : mode is masked to `0777` (setuid / setgid /
 *                       sticky bits stripped). Ownership of every created
 *                       file is forced to the current process uid/gid;
 *                       the archive's uid/gid fields are ignored.
 */

#ifndef ASSESSMENT_AGENT_EXTRACT_H
#define ASSESSMENT_AGENT_EXTRACT_H

#include <stddef.h>

typedef enum {
	EXTRACT_OK = 0,
	EXTRACT_ERR_OPEN,            /* libarchive open / format unsupported */
	EXTRACT_ERR_FORBIDDEN_TYPE,  /* symlink, hardlink, device, FIFO, socket */
	EXTRACT_ERR_PATH_TRAVERSAL,  /* `..` component or absolute path */
	EXTRACT_ERR_WRITE,           /* mkdir / open / write error during extraction */
	EXTRACT_ERR_INTERNAL,
} extract_status_t;

/**
 * @brief Extract @p archive_path into @p dest_dir under TP1 policy.
 *
 * @p dest_dir must already exist and be writable by the current process.
 */
extract_status_t extract_tarball(const char *archive_path, const char *dest_dir);

/**
 * @brief Return 1 if @p path is safe for relative extraction under a sandbox.
 *
 * Rejects: absolute paths (`/foo`), `..` components, empty paths.
 * Accepts: `foo`, `foo/bar`, `./foo` (the leading `./` is logically a no-op
 *          but harmless; libarchive normalizes it).
 *
 * Exposed for direct unit testing of the traversal guard.
 */
int extract_path_safe(const char *path);

#endif
