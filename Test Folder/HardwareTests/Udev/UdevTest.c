#include <stdbool.h>
// Observe the same block-device event API that Main uses, without mounting anything.
// Reference: https://www.freedesktop.org/software/systemd/man/latest/udev_monitor_receive_device.html
#include <libudev.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static const char* text(const char* value) {
    if (value == NULL) return "unknown";
    return value;
}

static int list_devices(struct udev* context) {
    // API: udev_enumerate_* takes a snapshot of current devices in the block subsystem.
    struct udev_enumerate* list = udev_enumerate_new(context);
    if (list == NULL) return 1;
    if (udev_enumerate_add_match_subsystem(list, "block") < 0 ||
        udev_enumerate_scan_devices(list) < 0) {
        udev_enumerate_unref(list);
        return 1;
    }
    struct udev_list_entry* entry;
    int count = 0;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(list)) {
        struct udev_device* device = udev_device_new_from_syspath(context, udev_list_entry_get_name(entry));
        if (device == NULL) continue;
        printf("%s  type=%s  filesystem=%s\n", text(udev_device_get_devnode(device)),
                    text(udev_device_get_devtype(device)),
                    text(udev_device_get_property_value(device, "ID_FS_TYPE")));
        count++;
        udev_device_unref(device);
    }
    udev_enumerate_unref(list);
    printf("PASS: libudev enumeration completed (%d block-device entries)\n", count);
    return 0;
}

static int watch_devices(struct udev* context, int seconds) {
    /* API: udev_monitor_new_from_netlink
     * Subscribe to processed udev events, filter block devices, enable reception,
     * then poll the monitor descriptor. This needs a running udev daemon.
     */
    struct udev_monitor* monitor = udev_monitor_new_from_netlink(context, "udev");
    if (monitor == NULL) return 1;
    if (udev_monitor_filter_add_match_subsystem_devtype(monitor, "block", NULL) < 0 ||
        udev_monitor_enable_receiving(monitor) < 0) {
        fprintf(stderr, "FAIL: cannot enable the udev monitor\n");
        udev_monitor_unref(monitor);
        return 1;
    }
    int fd = udev_monitor_get_fd(monitor);
    if (fd < 0) {
        udev_monitor_unref(monitor);
        return 1;
    }
    printf("Monitor ready for %d seconds. Insert the test USB drive; safely unmount it before removal.\n", seconds);
    fflush(stdout);
    int added = 0;
    int removed = 0;
    bool failed = false;
    double deadline = monotonic_seconds() + seconds;
    while (monotonic_seconds() < deadline) {
        struct pollfd event = {fd, POLLIN, 0};
        int ready = poll(&event, 1, 250);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("FAIL: udev poll");
            failed = true;
            break;
        }
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            failed = true;
            break;
        }
        if (ready == 0 || !(event.revents & POLLIN)) continue;
        // API: receive_device returns one event object; unref releases that object.
        struct udev_device* device = udev_monitor_receive_device(monitor);
        if (device == NULL) continue;
        const char* action = udev_device_get_action(device);
        printf("%s  %s  type=%s  filesystem=%s\n", text(action),
                    text(udev_device_get_devnode(device)), text(udev_device_get_devtype(device)),
                    text(udev_device_get_property_value(device, "ID_FS_TYPE")));
        fflush(stdout);
        if (action != NULL && strcmp(action, "add") == 0) added++;
        if (action != NULL && strcmp(action, "remove") == 0) removed++;
        udev_device_unref(device);
    }
    udev_monitor_unref(monitor);
    if (failed) return 1;
    if (added == 0 || removed == 0) {
        printf("INCONCLUSIVE: add=%d remove=%d. Repeat and perform both actions.\n", added, removed);
        return 3;
    }
    puts("PASS: add/remove block events received. Check that the printed nodes belong to your USB drive.");
    return 0;
}

int main(int argc, char** argv) {
    bool list = argc == 2 && strcmp(argv[1], "list") == 0;
    int seconds = 60;
    if (!list) {
        if ((argc != 2 && argc != 3) || strcmp(argv[1], "watch") != 0) {
            puts("Usage: UdevTest list\n       UdevTest watch [SECONDS: 1..300]");
            return 2;
        }
        if (argc == 3) {
            char* end = NULL;
            long value = strtol(argv[2], &end, 10);
            if (*argv[2] == '\0' || *end != '\0' || value < 1 || value > 300) return 2;
            seconds = (int)(value);
        }
    }
    // API: udev_new/unref create and release the userspace udev context.
    struct udev* context = udev_new();
    if (context == NULL) {
        fprintf(stderr, "FAIL: udev_new\n");
        return 1;
    }
    int result;
    if (list) result = list_devices(context);
    else result = watch_devices(context, seconds);
    udev_unref(context);
    return result;
}
