# Bound Provider failure handling

Every remote operation has a configurable Provider Failure Deadline, defaulting to two seconds and constrained to 0.5 through 30 seconds. At the deadline, Backed Mode falls back to its valid Backing Store without returning an application-visible I/O error, while Remote-Only Mode enters Remote-Lost; late RDMA completions are ignored through request-generation tracking so a block request is never completed twice. The initial production SLO requires p99 transition to the applicable state within two seconds rather than permitting infinite RDMA retries.
