#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <time.h>

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/input/event2";
    int secs = argc > 2 ? atoi(argv[2]) : 60;
    int fd = open(dev, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    printf("reading %s for %d s\n", dev, secs);
    fflush(stdout);
    time_t end = time(NULL) + secs;
    struct input_event ev;
    while (time(NULL) < end) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) continue;
        printf("%lld type=%u code=%u value=%d\n", (long long)((ev.time.tv_sec*1000LL)+(ev.time.tv_usec/1000)), ev.type, ev.code, ev.value);
        fflush(stdout);
    }
    close(fd);
    return 0;
}


