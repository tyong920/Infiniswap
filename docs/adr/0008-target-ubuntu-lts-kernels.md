# Target Ubuntu LTS GA kernels

Production support targets Ubuntu 22.04 with its Linux 5.15 GA kernel and Ubuntu 24.04 with its Linux 6.8 GA kernel rather than every upstream kernel after 5.15. The `ty-gpu-02` Linux 5.15 plus MLNX_OFED 5.8 tuple and an approved Ubuntu 24.04 Linux 6.8 plus inbox-RDMA tuple each require compile, VM, and physical-RDMA validation; obsolete Linux 3.x/4.x conditionals, Autotools output, legacy `nbdxadm`, and unsafe setup scripts are removed after their required behavior is captured in tests.
