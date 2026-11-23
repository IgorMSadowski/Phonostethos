# LED mask test

Files: `LedTest.c` and `Makefile`. Build with `make`; output: `build-pi/LedTest`. This is a userspace program, not the kernel module.

Stop the real application. On the Pi, load the module built for its running kernel if the service stop unloaded it:

```sh
ls -l /dev/phonostethos_leds
# If the node is missing, load your installed matching module:
insmod /lib/modules/$(uname -r)/extra/phonostethos_leds.ko
```

Run only the test against the LED outputs:

```sh
cd /tmp/phonostethos-tests/HardwareTests/LEDs
./LedTest
echo $?
```

It displays each expected state for one second:

| Mask | Expected physical state |
|---|---|
| 0x01 | Heart only, GPIO17 |
| 0x02 | Lung only, GPIO27 |
| 0x04 | General only, GPIO24 |
| 0x08 | Recording only, GPIO22 |
| 0x0F | All four |
| 0x00 | All off |

Compare the LEDs with each printed line. Ctrl+C attempts an all-off write before exiting incomplete. An alternative device path can be supplied as the only argument.

The program uses open/write/close and sends one binary byte per mask, as the real LedDriver does. It checks that writes are accepted. It cannot automatically detect an open LED wire, reversed LED or transistor failure because this circuit has no electrical feedback.

If open fails, check the module, device path and permissions. If writes pass but an LED is wrong, inspect the GPIO/pin mapping, resistor/transistor circuit and wiring. Record the visual result separately from the program's API PASS.
