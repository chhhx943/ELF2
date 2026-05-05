#include "relay_alarm.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#define DEVICE_NAME "/dev/relay"
#define RELAY_IOC_MAGIC 'R'
#define SET_RELAY_ON_IO _IO(RELAY_IOC_MAGIC, 1)
#define SET_RELAY_OFF_IO _IO(RELAY_IOC_MAGIC, 0)

int relay_alarm_set(int alarm_on) {
    int fd;
    int cmd = alarm_on ? SET_RELAY_ON_IO : SET_RELAY_OFF_IO;

    fd = open(DEVICE_NAME,O_RDWR);
    if(fd < 0) {
        perror("Failed to open device");
        return -1;
    }

    if(ioctl(fd, cmd) < 0) {
        perror(alarm_on ? "Failed to turn relay on" : "Failed to turn relay off");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int relay_alarm_on(void) {
    return relay_alarm_set(1);
}

int relay_alarm_off(void) {
    return relay_alarm_set(0);
}
