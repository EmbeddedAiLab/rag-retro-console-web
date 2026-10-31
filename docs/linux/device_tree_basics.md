# Device tree basics

## What it is
A device tree describes the hardware to the Linux kernel: CPUs, memory, buses and peripherals.
The source is a .dts file, includes are .dtsi files, and the compiler dtc produces a binary .dtb.

## Nodes and properties
A node has a name, an optional unit address and properties, for example i2c@fe5a0000 with status = "okay".
The compatible property selects the driver. The reg property gives the address. Setting status = "disabled" turns a node off.

## Overlays
A device tree overlay changes the base tree without rebuilding it, for example to enable an I2C device on a header.

## Inspecting at runtime
The running tree is visible under /proc/device-tree and /sys/firmware/devicetree/base.
Convert it back to source with: dtc -I fs -O dts /proc/device-tree
