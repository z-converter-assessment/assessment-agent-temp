#define _POSIX_C_SOURCE 200809L

#include "installer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DECL_EMBED(sym)                       \
    extern const char _binary_##sym##_start[]; \
    extern const char _binary_##sym##_end[];

DECL_EMBED(install_sh)
DECL_EMBED(uninstall_sh)
DECL_EMBED(image_prep_sh)
DECL_EMBED(detect_os_sh)
DECL_EMBED(env_setup_sh)
DECL_EMBED(assessment_agent_service)
DECL_EMBED(assessment_agent_sysv)
DECL_EMBED(agent_env_example)

#define EMBED_PTR(sym) (_binary_##sym##_start)
#define EMBED_LEN(sym) ((size_t)(_binary_##sym##_end - _binary_##sym##_start))

static int write_blob(const char *path, const char *data, size_t len, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        fprintf(stderr, "[install] open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[install] write %s: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }
        off += (size_t)n;
    }
    if (fchmod(fd, mode) != 0) {
        fprintf(stderr, "[install] chmod %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int rmrf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char child[PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            rmrf(child);
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

static int make_bundle(char *out_path, size_t cap)
{
    char tmpl[] = "/tmp/agent-installer-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "[install] mkdtemp /tmp/agent-installer-XXXXXX: %s\n",
                strerror(errno));
        return -1;
    }

    chmod(dir, 0700);

    char sub[PATH_MAX];
    snprintf(sub, sizeof sub, "%s/lib", dir);
    if (mkdir(sub, 0700) != 0) goto fail;
    snprintf(sub, sizeof sub, "%s/systemd", dir);
    if (mkdir(sub, 0700) != 0) goto fail;
    snprintf(sub, sizeof sub, "%s/sysv", dir);
    if (mkdir(sub, 0700) != 0) goto fail;

    struct { const char *rel; const char *data; size_t len; mode_t mode; } files[] = {
        { "install.sh",                       EMBED_PTR(install_sh),               EMBED_LEN(install_sh),               0700 },
        { "uninstall.sh",                     EMBED_PTR(uninstall_sh),             EMBED_LEN(uninstall_sh),             0700 },
        { "image-prep.sh",                    EMBED_PTR(image_prep_sh),            EMBED_LEN(image_prep_sh),            0700 },
        { "lib/detect-os.sh",                 EMBED_PTR(detect_os_sh),             EMBED_LEN(detect_os_sh),             0600 },
        { "lib/env-setup.sh",                 EMBED_PTR(env_setup_sh),             EMBED_LEN(env_setup_sh),             0600 },
        { "systemd/assessment-agent.service", EMBED_PTR(assessment_agent_service), EMBED_LEN(assessment_agent_service), 0600 },
        { "systemd/agent.env.example",        EMBED_PTR(agent_env_example),        EMBED_LEN(agent_env_example),        0600 },
        { "sysv/assessment-agent",            EMBED_PTR(assessment_agent_sysv),    EMBED_LEN(assessment_agent_sysv),    0700 },
    };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dir, files[i].rel);
        if (write_blob(full, files[i].data, files[i].len, files[i].mode) != 0)
            goto fail;
    }

    if ((size_t)snprintf(out_path, cap, "%s", dir) >= cap) goto fail;
    return 0;

fail:
    {
        int saved = errno;
        rmrf(dir);
        errno = saved;
    }
    return -1;
}

static int resolve_self(char *out, size_t cap)
{
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n < 0) {
        fprintf(stderr, "[install] readlink /proc/self/exe: %s\n", strerror(errno));
        return -1;
    }
    out[n] = '\0';
    return 0;
}

static int run_sh(const char *script, const char *const *envp_extra)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[install] fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        for (size_t i = 0; envp_extra && envp_extra[i]; i += 2)
            setenv(envp_extra[i], envp_extra[i + 1], 1);
        execlp("/bin/sh", "sh", script, (char *)NULL);
        fprintf(stderr, "[install] execlp /bin/sh: %s\n", strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int installer_run_install(int image_prep)
{
    char self[PATH_MAX];
    if (resolve_self(self, sizeof self) != 0) return 1;

    char bundle[PATH_MAX];
    if (make_bundle(bundle, sizeof bundle) != 0) return 1;

    char script[PATH_MAX];
    snprintf(script, sizeof script, "%s/install.sh", bundle);

    const char *env_pairs[8];
    size_t e = 0;
    env_pairs[e++] = "INSTALLER_SELF_PATH"; env_pairs[e++] = self;
    env_pairs[e++] = "SKIP_SHA256";         env_pairs[e++] = "1";
    if (image_prep) {
        env_pairs[e++] = "IMAGE_PREP"; env_pairs[e++] = "1";
    }
    env_pairs[e] = NULL;

    int rc = run_sh(script, env_pairs);
    rmrf(bundle);
    return rc;
}

int installer_run_uninstall(int purge)
{
    char bundle[PATH_MAX];
    if (make_bundle(bundle, sizeof bundle) != 0) return 1;

    char script[PATH_MAX];
    snprintf(script, sizeof script, "%s/uninstall.sh", bundle);

    const char *env_pairs[3];
    size_t e = 0;
    if (purge) {
        env_pairs[e++] = "PURGE"; env_pairs[e++] = "1";
    }
    env_pairs[e] = NULL;

    int rc = run_sh(script, env_pairs);
    rmrf(bundle);
    return rc;
}

int installer_run_prep_image(void)
{
    char bundle[PATH_MAX];
    if (make_bundle(bundle, sizeof bundle) != 0) return 1;

    char script[PATH_MAX];
    snprintf(script, sizeof script, "%s/image-prep.sh", bundle);

    int rc = run_sh(script, NULL);
    rmrf(bundle);
    return rc;
}
