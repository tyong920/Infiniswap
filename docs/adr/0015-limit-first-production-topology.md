# Limit the first production topology

The first production topology certifies one Memory Consumer using multiple Memory Providers from a versioned static Provider Directory. Each Provider connection uses one explicit RDMA Rail with NUMA-aware queue and buffer placement; `ty-gpu-02` initially uses the configured `mlx5_ib2` path. Multi-rail aggregation, automatic network discovery, and multiple production Consumers sharing one Provider remain outside the first GA until their ordering, isolation, revocation, and failure behavior receive dedicated tests.
