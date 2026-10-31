# Linux commands for embedded boards

## Kernel messages and modules
dmesg shows kernel messages, use dmesg -w to follow them. lsmod lists loaded modules.
modprobe loads a module with its dependencies, insmod loads a single file, rmmod removes a module.

## I2C
Install i2c-tools. i2cdetect -y 1 scans bus 1 for devices. i2cget and i2cset read and write registers.

## GPIO
The old sysfs GPIO interface is deprecated. Use libgpiod: gpiodetect lists chips, gpioinfo shows lines, gpioset sets an output.

## Services
systemctl status NAME shows a service, systemctl enable --now NAME starts it now and at boot, journalctl -u NAME -f follows its log.

## Disk and memory
free -h shows memory, df -h shows disk space, lsblk lists block devices.
