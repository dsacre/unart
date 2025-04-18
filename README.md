unart - definitely not a real UART
==================================

unart is a software UART implemented as a Linux kernel module.  
For when the pins are there, but the UART isn't.

It supports multiple independent instances, each bit-banging on a pair
of GPIO pins, and exposing a regular TTY serial device to user space.


Requirements
------------

- Linux 6.1 or later.
- A pair of GPIO pins.

The GPIO pins must be directly memory-mapped. This is usually true except when
using GPIO expanders.


Compiling and installing
------------------------

To build for your currently running kernel:
```sh
make
make modules_install
```

To cross-compile or build for a different kernel:
```sh
make KDIR=/path/to/kernel
```


Configuration by module parameters
----------------------------------

A single instance can be configured by module parameters at load time. For example:
```sh
modprobe unart gpiochip=pinctrl-bcm2711 rx_gpio=22 tx_gpio=23
```

Check `/sys/kernel/debug/gpio` for possible GPIO chips and pins.

The serial device will be called `/dev/ttyunart0`.


Configuration by device tree
----------------------------

See [this device tree overlay](example/unart-overlay.dts) for an example that
works as-is on pretty much any Raspberry Pi.

The example uses GPIO22 for RX and GPIO23 for TX. Adapt as needed, and build
the overlay with `make dtbs`.
Then copy `example/unart.dtbo` to `/boot/firmware/overlays/`, and enable the
overlay by adding `dtoverlay=unart` to `config.txt`.

You can use [udev rules like these](example/99-unart.rules) to change the
permissions of a device, or to give it a distinct name.


Performance
-----------

unart is primarily intended to be used at relatively low speeds of 9600 baud
or less.

Higher baud rates will work on most hardware, but reliability typically
becomes an issue around the 38400 mark, especially under high system load.

CPU overhead is proportional to the amount of data actually transferred, and
can become quite significant at high baud rates.
Note that all the real work happens in IRQs, making CPU load difficult to
measure directly.
