#include "ipc.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>

//  1：发送 FD (父进程用)
int send_fd(int socket, int fd_to_send) {
    struct msghdr msg = {0};
    char buf[1] = {'g'}; // 伴随的一字节普通数据
    struct iovec iov[1] = {{.iov_base = buf, .iov_len = 1}};
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    // 预留辅助数据的内存空间 (注意内存对齐)
    union {
        struct cmsghdr align;
        char cmsg_space[CMSG_SPACE(sizeof(int))];
    } u;
    msg.msg_control = u.cmsg_space;
    msg.msg_controllen = sizeof(u.cmsg_space);

    // 配置 SCM_RIGHTS 
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    return sendmsg(socket, &msg, 0);
}

//  2：接收 FD (子进程用)
int recv_fd(int socket) {
    struct msghdr msg = {0};
    char buf[1];
    struct iovec iov[1] = {{.iov_base = buf, .iov_len = 1}};
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;

    union {
        struct cmsghdr align;
        char cmsg_space[CMSG_SPACE(sizeof(int))];
    } u;
    msg.msg_control = u.cmsg_space;
    msg.msg_controllen = sizeof(u.cmsg_space);

    if (recvmsg(socket, &msg, 0) <= 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int received_fd;
        memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
        return received_fd; // 返回内核在子进程中克隆出来的新 FD！
    }
    return -1;
}