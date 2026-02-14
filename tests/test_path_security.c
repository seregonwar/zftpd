#include "ftp_path.h"
#include "ftp_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int mkdir_p(const char *path)
{
    if (mkdir(path, 0700) == 0) {
        return 0;
    }
    return -1;
}

int main(void)
{
    char tmpl[] = "/tmp/zftpd-test-XXXXXX";
    char *base = mkdtemp(tmpl);
    if (base == NULL) {
        return 2;
    }

    char root[FTP_PATH_MAX];
    (void)snprintf(root, sizeof(root), "%s/root", base);
    if (mkdir_p(root) != 0) {
        return 2;
    }

    char sub[FTP_PATH_MAX];
    (void)snprintf(sub, sizeof(sub), "%s/sub", root);
    if (mkdir_p(sub) != 0) {
        return 2;
    }

    char linkp[FTP_PATH_MAX];
    (void)snprintf(linkp, sizeof(linkp), "%s/out", root);
    (void)symlink("/", linkp);

    ftp_session_t s;
    memset(&s, 0, sizeof(s));
    char root_real[FTP_PATH_MAX];
    if (realpath(root, root_real) == NULL) {
        return 2;
    }
    (void)snprintf(s.root_path, sizeof(s.root_path), "%s", root_real);
    (void)snprintf(s.cwd, sizeof(s.cwd), "%s", root_real);

    char out[FTP_PATH_MAX];

    ftp_error_t ok = ftp_path_resolve(&s, "sub", out, sizeof(out));
    if (ok != FTP_OK) {
        return 3;
    }
    if (ftp_path_is_within_root(out, s.root_path) != 1) {
        return 4;
    }

    ftp_error_t t1 = ftp_path_resolve(&s, "../", out, sizeof(out));
    if (t1 != FTP_ERR_PATH_INVALID) {
        return 5;
    }

    ftp_error_t t2 = ftp_path_resolve(&s, "out/etc", out, sizeof(out));
    if (t2 != FTP_ERR_PATH_INVALID) {
        return 6;
    }

    unlink(linkp);
    rmdir(sub);
    rmdir(root);
    rmdir(base);
    return 0;
}
