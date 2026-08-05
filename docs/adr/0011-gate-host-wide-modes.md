# Gate host-wide modes by workload recoverability

Infiniswap is a host-wide swap implementation because standard Linux swap cannot route a particular swap device to one cgroup. Production packages default to Backed Mode with the Strict Policy. Remote-Only Mode is production-supported only on a Remote-Only-Eligible Host, where every swap-eligible workload can recover after the device enters Remote-Lost; mixed-recoverability hosts must use Backed Mode unless a future replicated remote mode changes that failure contract.
