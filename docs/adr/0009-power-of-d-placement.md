# Preserve power-of-d placement with mode-specific allocation

Remote Chunks use Power-of-d Placement with a runtime-configurable sample size, defaulting to two healthy Memory Providers; deterministic seeds are allowed only for reproducible tests. Backed Mode acquires a Remote Chunk lazily when its address range's runtime-configurable, read/write-weighted activity score crosses the Hot Range threshold. Remote-Only Mode reserves and maps its full advertised capacity before activation because it has no fallback when capacity is unavailable.
