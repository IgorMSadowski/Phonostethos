# udev / USB insertion-removal test

Files: `UdevTest.c` and `Makefile`. Build with `make`; output: `build-pi/UdevTest`.

This observes block-device events through the same libudev API used by Main. It does not mount, format, write or eject anything. A running udev daemon is required for processed udev events.

On the Pi:

```sh
cd /tmp/phonostethos-tests/HardwareTests/Udev
./UdevTest list
./UdevTest watch 60
```

During the 60-second watch:

1. Insert the test USB drive and look for `add` lines for its disk/partition, such as /dev/sda and /dev/sda1. One insertion may create several events.
2. In a second Pi terminal, check `mount | grep '/media/usb_storage'`.
3. Safely unmount it with `umount /media/usb_storage` when the standalone test has no application using it. If the real application is running, use GUI Eject instead.
4. Remove the drive and look for matching `remove` lines. Wait for the watch to finish.

Expected: PASS after both add and remove block events are observed. Confirm the printed device nodes belong to your USB drive. Unknown filesystem properties on a removal event are normal; the device information may already have disappeared.

No events / only one action -> INCONCLUSIVE (exit 3), not PASS. Repeat with both actions. If there are still no events, check that udevd/eudev is running and the kernel detects the USB device. `list` passing only proves current-device enumeration.

APIs: udev_new creates a context; udev_enumerate scans current devices; udev_monitor_new_from_netlink/filter/enable subscribes to events; poll waits for readable data; receive_device returns an event object; unref releases it.

API reference: https://www.freedesktop.org/software/systemd/man/latest/udev_monitor_receive_device.html
