#ifndef IPC_H
#define IPC_H

int send_fd(int socket, int fd_to_send);
int recv_fd(int socket);

#endif