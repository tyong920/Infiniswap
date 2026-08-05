# Drain the Consumer for upgrades

The Memory Consumer does not support live module upgrades: tooling first verifies available RAM and alternate swap capacity, then explicitly disables the Infiniswap swap, drains in-flight I/O, unloads the old module, installs the new package, and restores the device, with rollback tested before release. Backed Mode Providers may be upgraded one at a time through normal fallback and eviction; Remote-Only Mode requires the affected device to be stopped and recreated until cross-Provider migration exists.
