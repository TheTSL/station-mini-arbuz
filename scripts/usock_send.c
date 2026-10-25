/*
 * usock_send.c — отправка строки в Unix-сокет и чтение ответа.
 *
 *   usock_send <path> <message>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <socket-path> <message>\n", argv[0]);
        return 1;
    }
    const char *path = argv[1];
    const char *msg = argv[2];

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 2; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 3;
    }

    if (write(fd, msg, strlen(msg)) < 0)
        perror("write");

    char buf[512];
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("reply: %s\n", buf);
    } else {
        printf("no reply\n");
    }
    close(fd);
    return 0;
}
