# Linux platform driver skeleton

## Structure
A platform driver defines struct platform_driver with probe and remove functions and an of_match_table.
The compatible string in the table must match the compatible property of the device tree node.

## probe and remove
probe runs when a matching device is found. It maps registers, requests clocks and interrupts, and registers the device with its subsystem.
Prefer the devm_ helpers, for example devm_ioremap_resource and devm_kzalloc. Resources they allocate are freed automatically when the device goes away.

## Registering
Use module_platform_driver to generate the init and exit code. Add MODULE_LICENSE("GPL") or the module taints the kernel.

## pinctrl
The pinctrl subsystem sets pin muxing. A device node lists pinctrl-names and pinctrl-0 for its default state.
The driver core applies the default state before probe, so most drivers do not call pinctrl functions themselves.

## Character devices
Allocate numbers with alloc_chrdev_region, add the device with cdev_add, and create the device node with device_create.
