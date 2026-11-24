/**
 * UdevTest.c - checks Udev Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - monotonic_seconds(): monotonic seconds.
 * - list_devices(): list devices.
 * - watch_devices(): watch devices.
 * - main(): run this standalone test and return success only if its checks pass.
 */

// API DOCUMENTATION FOR THIS FILE:
// libudev contexts, device properties, enumeration and hotplug monitoring:
// https://www.freedesktop.org/software/systemd/man/latest/libudev.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#include <stdbool.h> // Include C bool, true and false.
// Observe the same block-device event API that Main uses, without mounting anything.
// Reference: https://www.freedesktop.org/software/systemd/man/latest/udev_monitor_receive_device.html
#include <libudev.h> // Include Linux device enumeration and hotplug event monitoring.
#include <poll.h> // Include bounded descriptor-readiness checks for sockets and udev.
#include <errno.h> // Include errno and symbolic system-call error codes.
#include <time.h> // Include timespec, monotonic clocks and timed sleeps.
#include <stdio.h> // Include printf/fprintf/puts console diagnostics.
#include <stdlib.h> // Include process exit, allocation and numeric conversion helpers.
#include <string.h> // Include memory/string copying, comparisons and strerror diagnostics.

static double monotonic_seconds(void) { // Finish the parameters/condition and enter the block below.
    struct timespec now; // Group the related fields in one plain data record.
    /* API: clock_gettime
     * Documentation: https://man7.org/linux/man-pages/man3/clock_gettime.3.html
     * Read the selected clock into a timespec. CLOCK_MONOTONIC measures elapsed time; seconds/nanoseconds are used to form absolute wait deadlines.
     */
    clock_gettime(CLOCK_MONOTONIC, &now); // Call clock_gettime with the arguments shown here.
    return now.tv_sec + now.tv_nsec / 1000000000.0; // Return now.tv_sec + now.tv_nsec / 1000000000.0 to the caller.
} // End this block, declaration or argument list.

static const char* text(const char* value) { // Finish the parameters/condition and enter the block below.
    if (value == NULL) return "unknown"; // Test the condition value == NULL. If true, return "unknown" to the caller.
    return value; // Return value to the caller.
} // End this block, declaration or argument list.

static int list_devices(struct udev* context) { // Finish the parameters/condition and enter the block below.
    // API: udev_enumerate_* takes a snapshot of current devices in the block subsystem.
    struct udev_enumerate* list = udev_enumerate_new(context); // Group the related fields in one plain data record.
    if (list == NULL) return 1; // Test the condition list == NULL. If true, return 1 to the caller.
    if (udev_enumerate_add_match_subsystem(list, "block") < 0 || // Test the condition udev_enumerate_add_match_subsystem(list, "block") < 0 ||.
        udev_enumerate_scan_devices(list) < 0) { // Call udev_enumerate_scan_devices with the arguments shown here.
        udev_enumerate_unref(list); // Call udev_enumerate_unref with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    struct udev_list_entry* entry; // Group the related fields in one plain data record.
    int count = 0; // Initialize count to 0.
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(list)) { // Call udev_list_entry_foreach with the arguments shown here.
        struct udev_device* device = udev_device_new_from_syspath(context, udev_list_entry_get_name(entry)); // Group the related fields in one plain data record.
        if (device == NULL) continue; // Test the condition device == NULL. If true, skip the rest of this pass and check the next loop iteration.
        /* API: udev_device_get_devnode
         * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
         * Read the block-device path, for example /dev/sda1. Check nullptr before building the FileIO request.
         */
        printf("%s  type=%s  filesystem=%s\n", text(udev_device_get_devnode(device)), // Print the diagnostic or test result for the operator.
                    text(udev_device_get_devtype(device)), // Call text with the arguments shown here.
                    /* API: udev_device_get_property_value
                     * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
                     * Read a named property such as ID_FS_TYPE. The returned string is borrowed; copy it into the mailbox request before unref.
                     */
                    text(udev_device_get_property_value(device, "ID_FS_TYPE"))); // Call text with the arguments shown here.
        count++; // Advance this index/counter for the next item.
        /* API: udev_device_unref
         * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
         * Release the event-device reference after copying the required fields. Do not keep borrowed strings after this point.
         */
        udev_device_unref(device); // Call udev_device_unref with the arguments shown here.
    } // End this block, declaration or argument list.
    udev_enumerate_unref(list); // Call udev_enumerate_unref with the arguments shown here.
    printf("PASS: libudev enumeration completed (%d block-device entries)\n", count); // Print the diagnostic or test result for the operator.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

static int watch_devices(struct udev* context, int seconds) { // Finish the parameters/condition and enter the block below.
    /* API: udev_monitor_new_from_netlink
     * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
     * Subscribe to processed udev events, filter block devices, enable reception,
     * then poll the monitor descriptor. This needs a running udev daemon.
     */
    struct udev_monitor* monitor = udev_monitor_new_from_netlink(context, "udev"); // Group the related fields in one plain data record.
    if (monitor == NULL) return 1; // Test the condition monitor == NULL. If true, return 1 to the caller.
    /* API: udev_monitor_filter_add_match_subsystem_devtype
     * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
     * Filter monitor events to the required subsystem/device type before receiving. A negative return indicates failure.
     */
    if (udev_monitor_filter_add_match_subsystem_devtype(monitor, "block", NULL) < 0 || // Test the condition udev_monitor_filter_add_match_subsystem_devtype(monitor, "block", NULL) < 0 ||.
        /* API: udev_monitor_enable_receiving
         * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
         * Enable delivery to the configured monitor. Check the return before asking for its descriptor.
         */
        udev_monitor_enable_receiving(monitor) < 0) { // Call udev_monitor_enable_receiving with the arguments shown here.
        fprintf(stderr, "FAIL: cannot enable the udev monitor\n"); // Print the diagnostic or test result for the operator.
        /* API: udev_monitor_unref
         * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
         * Release the monitor during teardown, after Main has stopped checking its descriptor.
         */
        udev_monitor_unref(monitor); // Call udev_monitor_unref with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    /* API: udev_monitor_get_fd
     * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
     * Get the monitor's file descriptor for poll(). Poll with zero timeout in Main so USB checking does not block command dispatch.
     */
    int fd = udev_monitor_get_fd(monitor); // Initialize fd to udev_monitor_get_fd(monitor).
    if (fd < 0) { // Test the condition fd < 0.
        udev_monitor_unref(monitor); // Call udev_monitor_unref with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    printf("Monitor ready for %d seconds. Insert the test USB drive; safely unmount it before removal.\n", seconds); // Print the diagnostic or test result for the operator.
    fflush(stdout); // Call fflush with the arguments shown here.
    int added = 0; // Initialize added to 0.
    int removed = 0; // Initialize removed to 0.
    bool failed = false; // Initialize failed to false.
    double deadline = monotonic_seconds() + seconds; // Initialize deadline to monotonic_seconds() + seconds.
    while (monotonic_seconds() < deadline) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        struct pollfd event = {fd, POLLIN, 0}; // Group the related fields in one plain data record.
        int ready = poll(&event, 1, 250); // Initialize ready to poll(&event, 1, 250).
        if (ready < 0) { // Test the condition ready < 0.
            if (errno == EINTR) continue; // Test the condition errno == EINTR. If true, skip the rest of this pass and check the next loop iteration.
            perror("FAIL: udev poll"); // Call perror with the arguments shown here.
            failed = true; // Set failed to true.
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) { // Test the condition event.revents & (POLLERR | POLLHUP | POLLNVAL).
            failed = true; // Set failed to true.
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
        if (ready == 0 || !(event.revents & POLLIN)) continue; // Test the condition ready == 0 || !(event.revents & POLLIN). If true, skip the rest of this pass and check the next loop iteration.
        // API: receive_device returns one event object; unref releases that object.
        struct udev_device* device = udev_monitor_receive_device(monitor); // Group the related fields in one plain data record.
        if (device == NULL) continue; // Test the condition device == NULL. If true, skip the rest of this pass and check the next loop iteration.
        /* API: udev_device_get_action
         * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
         * Read the event action string such as add/remove/change. The borrowed string may be nullptr and remains tied to this udev device.
         */
        const char* action = udev_device_get_action(device); // Initialize action to udev_device_get_action(device).
        printf("%s  %s  type=%s  filesystem=%s\n", text(action), // Print the diagnostic or test result for the operator.
                    text(udev_device_get_devnode(device)), text(udev_device_get_devtype(device)), // Call text with the arguments shown here.
                    text(udev_device_get_property_value(device, "ID_FS_TYPE"))); // Call text with the arguments shown here.
        fflush(stdout); // Call fflush with the arguments shown here.
        if (action != NULL && strcmp(action, "add") == 0) added++; // Test the condition action != NULL && strcmp(action, "add") == 0. If true, advance this index/counter for the next item.
        if (action != NULL && strcmp(action, "remove") == 0) removed++; // Test the condition action != NULL && strcmp(action, "remove") == 0. If true, advance this index/counter for the next item.
        udev_device_unref(device); // Call udev_device_unref with the arguments shown here.
    } // End this block, declaration or argument list.
    udev_monitor_unref(monitor); // Call udev_monitor_unref with the arguments shown here.
    if (failed) return 1; // Test the condition failed. If true, return 1 to the caller.
    if (added == 0 || removed == 0) { // Test the condition added == 0 || removed == 0.
        printf("INCONCLUSIVE: add=%d remove=%d. Repeat and perform both actions.\n", added, removed); // Print the diagnostic or test result for the operator.
        return 3; // Return 3 to the caller.
    } // End this block, declaration or argument list.
    puts("PASS: add/remove block events received. Check that the printed nodes belong to your USB drive."); // Call puts with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

int main(int argc, char** argv) { // Run this standalone test and return success only if its checks pass.
    bool list = argc == 2 && strcmp(argv[1], "list") == 0; // Initialize list to argc == 2 && strcmp(argv[1], "list") == 0.
    int seconds = 60; // Initialize seconds to 60.
    if (!list) { // Test the condition !list.
        if ((argc != 2 && argc != 3) || strcmp(argv[1], "watch") != 0) { // Test the condition (argc != 2 && argc != 3) || strcmp(argv[1], "watch") != 0.
            puts("Usage: UdevTest list\n       UdevTest watch [SECONDS: 1..300]"); // Call puts with the arguments shown here.
            return 2; // Return 2 to the caller.
        } // End this block, declaration or argument list.
        if (argc == 3) { // Test the condition argc == 3.
            char* end = NULL; // Initialize end to NULL.
            long value = strtol(argv[2], &end, 10); // Initialize value to strtol(argv[2], &end, 10).
            if (*argv[2] == '\0' || *end != '\0' || value < 1 || value > 300) return 2; // Test the condition *argv[2] == '\0' || *end != '\0' || value < 1 || value > 300. If true, return 2 to the caller.
            seconds = (int)(value); // Set seconds to (int)(value).
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    // API: udev_new/unref create and release the userspace udev context.
    struct udev* context = udev_new(); // Group the related fields in one plain data record.
    if (context == NULL) { // Test the condition context == NULL.
        fprintf(stderr, "FAIL: udev_new\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    int result; // Declare result for this operation.
    if (list) result = list_devices(context); // Test the condition list. If true, set result to list_devices(context).
    else result = watch_devices(context, seconds); // Follow the alternative path when the previous condition is false.
    /* API: udev_unref
     * Documentation: https://www.freedesktop.org/software/systemd/man/latest/libudev.html
     * Release the libudev context after its monitor is released.
     */
    udev_unref(context); // Call udev_unref with the arguments shown here.
    return result; // Return result to the caller.
} // End this block, declaration or argument list.
