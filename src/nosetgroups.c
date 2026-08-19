#include <stddef.h>
typedef unsigned int gid_t;

#ifdef __cplusplus
extern "C" {
#endif

int setgroups(size_t size, const gid_t *list) {
    (void)size;
    (void)list;
    return 0;
}

int initgroups(const char *user, gid_t group) {
    (void)user;
    (void)group;
    return 0;
}

#ifdef __cplusplus
}
#endif
