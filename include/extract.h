#ifndef ASSESSMENT_AGENT_EXTRACT_H
#define ASSESSMENT_AGENT_EXTRACT_H

#include <stddef.h>

typedef enum {
	EXTRACT_OK = 0,
	EXTRACT_ERR_OPEN,
	EXTRACT_ERR_FORBIDDEN_TYPE,
	EXTRACT_ERR_PATH_TRAVERSAL,
	EXTRACT_ERR_WRITE,
	EXTRACT_ERR_INTERNAL,
} extract_status_t;

extract_status_t extract_tarball(const char *archive_path, const char *dest_dir);

int extract_path_safe(const char *path);

#endif
