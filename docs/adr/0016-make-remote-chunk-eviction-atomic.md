# Make Remote Chunk eviction atomic

When a Memory Provider asks a Memory Consumer to evict Backed Mode Remote Chunks, the Consumer validates the entire batch before changing any Remote Chunk, stops admitting new Remote I/O Transactions to those chunks at the eviction transition, waits for accepted transactions to release their leases, and only then invalidates and releases the mappings.

Mapping grants, eviction admission, and final mapping release are all-or-nothing batches. A malformed or stale entry rejects the complete batch without changing Remote Chunk state or accounting. Caller-owned opaque claims bind each workflow to its module, Provider handle, batch, and transition generation so delayed or duplicate completion cannot mutate a later mapping.

This trades immediate fallback to the Backing Store for guaranteed eviction progress and prevents malformed batches from leaving partially mapping, evicting, or released state. Committed Remote Chunks remain non-evictable.
