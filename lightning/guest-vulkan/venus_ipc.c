/* Module: lightning.guest_vulkan.venus_ipc
 * Purpose: Transfer real exported blob FDs over the host Unix renderer socket.
 * Created: 2026-10-09
 * Architecture: Thin POSIX ancillary-message ABI. Rust owns framing, partial IO,
 * descriptor lifetimes and protocol validation; this never sends an address.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
int qlv_venus_peer_pid(int socket)
{
    struct ucred credentials;
    socklen_t bytes = sizeof(credentials);

    if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &bytes) ||
        bytes != sizeof(credentials) || credentials.pid <= 0)
        return -1;
    return credentials.pid;
}

#include <sys/types.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
ssize_t qlv_venus_send_fd(int socket, const void *bytes, size_t count, int descriptor)
{
    if (!bytes || !count || descriptor < 0) {
        errno = EINVAL;
        return -1;
    }
    union {
        struct cmsghdr alignment;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct iovec vector = {(void *)bytes, count};
    struct msghdr message = {0};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(descriptor));
    memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
    ssize_t result;

    do {
        result = sendmsg(socket, &message, MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    return result;
}
