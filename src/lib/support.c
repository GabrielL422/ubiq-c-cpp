#include <ubiq/platform/internal/support.h>

#include <errno.h>

#if defined(_WIN32)
#  include <userenv.h>
#else
#  include <pwd.h>
#  include <unistd.h>
#endif

int
ubiq_support_get_home_dir(
    char ** const _dir)
{
    char * dir;
    int err;

#if defined(_WIN32)
    HANDLE token;
    DWORD len;

    err = INT_MIN;

    token = GetCurrentProcessToken();
    len = 0;
    GetUserProfileDirectoryA(token, NULL, &len);
    if (len > 0) {
        err = -ENOMEM;
        dir = malloc(sizeof(*dir) * len);
        if (dir) {
            if (GetUserProfileDirectoryA(token, dir, &len)) {
                *_dir = dir;
                err = 0;
            } else {
                free(dir);
                err = INT_MIN;
            }
        }
    }
#else
    const struct passwd * const pw = getpwuid(geteuid());

    err = -errno;
    if (pw) {
        err = -ENOMEM;
        dir = malloc(strlen(pw->pw_dir) + 1);
        if (dir) {
            strcpy(dir, pw->pw_dir);
            *_dir = dir;
            err = 0;
        }
    }
#endif

    return err;
}
