/* Module: lightning.guest_vulkan.windows_sync_fd
 * Purpose: Reject Linux sync-file handles in the native Windows Venus port.
 * Created: 2026-10-09
 * Architecture: Mesa handles its API-defined already-signaled fd sentinel
 * before these calls. Real Windows timeline waiting is the Escape transport;
 * an integer file descriptor must never fabricate a successful native wait.
 */
#ifndef QLV_WINDOWS_SYNC_FD_H
#define QLV_WINDOWS_SYNC_FD_H
#include <errno.h>
#include <stdbool.h>
static inline bool sync_valid_fd(int fd)
{
    (void)fd;
    return false;
}

static inline int sync_wait(int fd, int timeout)
{
    (void)fd;
    (void)timeout;
    errno = ENOTSUP;
    return -1;
}

#endif
