# Infiniswap Valuable Forks Research Note

**Upstream:** [SymbioticLab/Infiniswap](https://github.com/SymbioticLab/Infiniswap)  
**Upstream tip:** `master` last push `2020-09-26` (~258★ / ~53 forks)  
**Compared against:** `SymbioticLab:master` via GitHub Compare API  
**Date of survey:** 2026-08-05

## 中文摘要

约 53 个 fork 里多数是空镜像。真正值得关注的：

| 优先级 | 仓库 | 价值 |
|--------|------|------|
| 1 | [AAMH/Infiniswap](https://github.com/AAMH/Infiniswap) | CloudLab / Linux 4.4 可运行性 + 文档（注意 `stackbd_bio_generate` 被 stub） |
| 2 | [chrisbaldwin2/tamuSRM](https://github.com/chrisbaldwin2/tamuSRM) | Linux **4.14** bio/`blk_status` 移植 |
| 3 | [juncgu/infiniswap `ecswap`](https://github.com/juncgu/infiniswap/tree/ecswap) | **作者分支**：ISA-L 纠删码容错（相对上游已分叉） |
| 4 | [yuhong-zhong/infiniswap](https://github.com/yuhong-zhong/infiniswap) | 页压缩层（LZO/LZ4） |
| 5 | [WillK13/Infiniswap_multi](https://github.com/WillK13/Infiniswap_multi) | 实验脚本（非多机架构）；另建议合入 [Wiflin](https://github.com/Wiflin/Infiniswap) 的 clone panic 修复 |

作者 [juncgu](https://github.com/juncgu/infiniswap) 的 `master` 几乎无新增，有价值的工作在 **`ecswap` / `GUI` / `infinifs`** 分支。相关非 fork 参考：[clusterfarmem/fastswap](https://github.com/clusterfarmem/fastswap)。

现代化建议吸收顺序：Wiflin bugfix → AAMH（4.4/cgroup）→ tamuSRM（4.14 API）→（可选）ecswap / 压缩 / 实验脚本。

---

## Summary

- Most GitHub forks are **empty mirrors** (same tip as upstream, or only behind). Real unique work is concentrated in a handful of repos / author branches.
- **Best practical modernization path for CloudLab / Ubuntu 16.04 / Linux 4.4:** [AAMH/Infiniswap](https://github.com/AAMH/Infiniswap) (pushed 2026-07-06) — rewritten CloudLab README, cgroup-without-LXC workflow, configfs portal sanitization, bioset/stackbd exports, and a write-completion `IS_insert_ctx` reclaim fix. Caveat: `stackbd_bio_generate()` is currently stubbed to an early `return` with the new path commented out ([`c095b7cf`](https://github.com/AAMH/Infiniswap/commit/c095b7cf)).
- **Furthest kernel API port among forks:** [chrisbaldwin2/tamuSRM](https://github.com/chrisbaldwin2/tamuSRM) (+44) — Linux **4.14** changes (`bio_clone_kmalloc`, `bio_set_dev`, `blk_status_t` / `BLK_STS_*`) plus an Emulab/CloudLab `profile.py`. Repo description says “Smart placement of Remote Memory”, but the published tree has **no placement-algorithm code** beyond stock Infiniswap.
- **Unique research features (not just ports):**
  - [yuhong-zhong/infiniswap](https://github.com/yuhong-zhong/infiniswap) — optional **compression layer** (zbud-style pool, LZO/LZ4; Columbia 2020).
  - Author [juncgu/infiniswap](https://github.com/juncgu/infiniswap) **non-`master` branches**: `ecswap` (ISA-L erasure coding), `GUI` (Grafana cluster dashboard), `infinifs` (filesystem prototype).
- User-flagged [WillK13/Infiniswap_multi](https://github.com/WillK13/Infiniswap_multi) (+10, 2025-12) is **not** a multi-server architecture fork. Real deltas: hardcode `lookup_bdev(x,0)`, CloudLab-ish setup knobs, memcached/memtier/redis experiment scripts + large result logs. Name “multi” refers to **multi-workload** experiments (memcached+redis), not multi-host protocol changes.
- Related non-fork: [clusterfarmem/fastswap](https://github.com/clusterfarmem/fastswap) (86★) is a separate far-memory RDMA swap stack (kernel 4.11 patches), useful as a modernization reference but not an Infiniswap descendant.

---

## Prioritized valuable forks

| Priority | Repo | Last push | Ahead of upstream | Key changes | Value |
|---|---|---|---|---|---|
| 1 | [AAMH/Infiniswap](https://github.com/AAMH/Infiniswap) | 2026-07-06 | **+2** | CloudLab Ubuntu 16.04/4.4 docs; configfs portal fix; bioset; write-done ctx reclaim; cgroup/memcached scripts | **High** |
| 2 | [chrisbaldwin2/tamuSRM](https://github.com/chrisbaldwin2/tamuSRM) | 2023-03-26 | **+44** | Linux 4.14 blk/bio API port; CloudLab/Emulab profile; install helpers | **High** |
| 3 | [juncgu/infiniswap `ecswap`](https://github.com/juncgu/infiniswap/tree/ecswap) | 2018-04-11 (branch) | **+4** (diverged, −13) | Erasure-coded swap (ISA-L); large `is_main.c` / `is_mq.c` rework | **High** |
| 4 | [yuhong-zhong/infiniswap](https://github.com/yuhong-zhong/infiniswap) | 2020-02-18 | **+4** | Compression layer (`comp_driver` / `comp_pool`); COMP_ENABLE / COMP_LZ4 | **High** |
| 5 | [WillK13/Infiniswap_multi](https://github.com/WillK13/Infiniswap_multi) | 2025-12-08 | **+10** | OS/setup tweaks; memtier/memcached/redis harness + results | **Medium** |
| 6 | [juncgu/infiniswap `GUI`](https://github.com/juncgu/infiniswap/tree/GUI) | ~2018-10 | **+74** (diverged) | Grafana dashboards, cluster_setup, `dashboard.c` telemetry | **Medium** |
| 7 | [Wiflin/Infiniswap](https://github.com/Wiflin/Infiniswap) | 2020-09-26 | **+1** | Fix: attach `bi_end_io` to every bio in chain (kernel panic) | **Medium** |
| 8 | [sctb512/myInfiniswap](https://github.com/sctb512/myInfiniswap) *(not a GH fork)* | 2022-06-17 | n/a (independent copy) | Same bio end_io fix + NULL guards; large `exp/` benchmark suite | **Medium** |
| 9 | [tomatolike/Infiniswap](https://github.com/tomatolike/Infiniswap) | 2020-12-22 | **+48** | Deploy/LXC/pmem scripts; daemon threshold tweaks; debug prints | **Medium–Low** |
| 10 | [juncgu/infiniswap `infinifs`](https://github.com/juncgu/infiniswap/tree/infinifs) | 2018-05-21 | **+2** (diverged) | Prototype InfiniFS kernel+userspace on remote memory | **Medium–Low** |
| 11 | [sakura0423/Infiniswap](https://github.com/sakura0423/Infiniswap) | 2020-12-24 | **+13** | Heavy `pr_info` tracing counters only | **Low** |
| 12 | [zxiangwei/Infiniswap](https://github.com/zxiangwei/Infiniswap) | 2023-05-23 | **+9** | Chinese comments only; no logic change | **Low** |

---

## Detailed notes per valuable fork

### 1. AAMH/Infiniswap — High (CloudLab / 4.4 modernization)

- **Compare:** [`SymbioticLab:master...AAMH:master`](https://github.com/SymbioticLab/Infiniswap/compare/master...AAMH:master) → `ahead_by: 2`.
- **Commits:** [`c095b7cf`](https://github.com/AAMH/Infiniswap/commit/c095b7cf) (code + docs), [`52b954ae`](https://github.com/AAMH/Infiniswap/commit/52b954ae) (README/`cgroup.sh` polish). Both titled “Document CloudLab Infiniswap setup”.
- **What changed (concrete):**
  - `README.md` rewritten as a CloudLab runbook: Ubuntu 16.04 / Linux 4.4, cgroups v1 without LXC, GRUB `cgroup_enable=memory swapaccount=1`, RDMA package list, two-node topology `m1`/`m2` ([README](https://github.com/AAMH/Infiniswap/blob/master/README.md)).
  - `setup/install.sh`: `have_lookup_bdev_patch=1`, `backup_disk="/dev/loop0"`.
  - `infiniswap_bd/is_configfs.c`: safe portal string build (`kmalloc` + strip `\n`) instead of `strcat` into a fixed buffer.
  - `infiniswap_bd/is_mq.c`: export `stackbd` / `stackbd_lower_bdev` / `is_clone_bs`; create/free `bioset` on register/unregister.
  - `infiniswap_bd/is_main.c`: `client_write_done` calls `IS_insert_ctx(ctx)` before ending the request (ctx pool reclaim). New `stackbd_bio_generate` body is **commented out** after an early `return;` — disk-fallback remapping path is incomplete.
  - Helpers: `cgroup.sh`, `memcached_memaslap_build.sh`.
- **Why it matters:** Closest living documentation + patches for bringing upstream onto a still-available CloudLab OS/kernel pairing.
- **Caveats:** Incomplete `stackbd_bio_generate`; still not a modern (≥5.x) kernel port; only 2 commits, so history is thin.

### 2. chrisbaldwin2/tamuSRM — High (Linux 4.14 API port)

- **Compare:** [`...chrisbaldwin2:master`](https://github.com/SymbioticLab/Infiniswap/compare/master...chrisbaldwin2:master) → `ahead_by: 44`.
- **Description:** “Smart placement of Remote Memory” ([repo](https://github.com/chrisbaldwin2/tamuSRM)).
- **What changed (concrete):**
  - `infiniswap_bd/is_mq.c`: for `LINUX_VERSION_CODE >= 4.14`, use `bio_clone_kmalloc` instead of `bio_clone`; use `bio_set_dev` instead of assigning `bio->bi_bdev`; `IS_queue_rq` returns `blk_status_t` / `BLK_STS_OK` / `BLK_STS_IOERR` for kernels ≥ 4.13 ([commit themes: “Add updates for kernel 4.14”, “Update to blk_status”, …](https://github.com/SymbioticLab/Infiniswap/compare/master...chrisbaldwin2:master)).
  - `infiniswap_bd/is_main.c`: `bio_clone_kmalloc` in `stackbd_bio_generate`.
  - `profile.py`: Emulab/CloudLab RSpec — two nodes, optional switch, `tamuSRM.postIB` disk image, blockstores on `/dev/sda4`.
  - `setup/bd.sh`, `setup/daemon.sh`, `setup/silly.sh`; `have_lookup_bdev_patch=1`.
- **Why it matters:** Only surveyed fork that systematically targets **4.14** block-layer API breaks — directly relevant to kernel modernization work.
- **Caveats:**
  - No “smart placement” policy code appears in the tree (only stock Infiniswap + infra).
  - `IS_queue_rq` always returns `BLK_STS_*` after the `#if`, which may be wrong for kernels between 3.19 and 4.12 if those code paths are still compiled.
  - `setup/infiniswap_bd_setup.sh` was changed to `mkswap`/`swapon` **`/dev/sda4`** instead of `/dev/infiniswap0` — looks like a local experiment mistake, not a general improvement.

### 3. juncgu/infiniswap `ecswap` — High (erasure coding / author branch)

- **Branch:** [juncgu/infiniswap/tree/ecswap](https://github.com/juncgu/infiniswap/tree/ecswap)  
- **Compare:** [`...juncgu:ecswap`](https://github.com/SymbioticLab/Infiniswap/compare/master...juncgu:ecswap) → ahead 4 / behind 13 (diverged; based on older tip).
- **Commits of note:** [`6406c987`](https://github.com/juncgu/infiniswap/commit/6406c987) “added ISA-L erasure code”; [`77ad14e7`](https://github.com/juncgu/infiniswap/commit/77ad14e7) “ECswap init”.
- **What changed:** Vendored ISA-L `infiniswap_bd/erasure_code/**`; README documents `NDATAS` / `NDISKS` / `DATASIZE_G` under `/*EC setup*/`; large rewrites of `is_main.c` (+696/−355) and `is_mq.c` (+49/−533) vs current upstream tip.
- **Why it matters:** Fault-tolerance direction beyond local backup disk; from an original author (`juncgu`).
- **Caveats:** Stale vs current `master`; experimental; ISA-L asm blobs dominate the tree.

### 4. yuhong-zhong/infiniswap — High (compression layer)

- **Compare:** [`...yuhong-zhong:master`](https://github.com/SymbioticLab/Infiniswap/compare/master...yuhong-zhong:master) → `ahead_by: 4`.
- **Commits:** “add compression layer”, “add more comments & refactor zbud_clear”, “fix README & fix compatibility with new kernel w/o compression”, “fix README format”.
- **What changed:**
  - New files: `comp_driver.c/.h`, `comp_pool.c/.h` (copyright: Hongyi Wang, Yuhong Zhong, Columbia University, 2020).
  - Wired into `Makefile.in`; hooks in `is_mq.c` / `is_main.c` / `infiniswap.h`.
  - Config knobs `COMP_ENABLE`, `COMP_LZ4` (README notes compression **only on kernel 3.13.0**).
- **Why it matters:** Only fork that adds a substantial **data-path feature** (compress-before-remote) rather than porting/scripts.
- **Caveats:** Compression path is kernel-3.13-only; old tip; needs careful rebase onto any modernized base.

### 5. WillK13/Infiniswap_multi — Medium (experiments; user example)

- **Compare:** [`...WillK13:master`](https://github.com/SymbioticLab/Infiniswap/compare/master...WillK13:master) → `ahead_by: 10`.
- **Meaningful commits:**
  - [`34b5259e`](https://github.com/WillK13/Infiniswap_multi/commit/34b5259e) “tweaks for updates OS” — build artifacts + `config.h` + `lookup_bdev` hardcode.
  - [`f3f5f733`](https://github.com/WillK13/Infiniswap_multi/commit/f3f5f733) “working memcached” — `memcached_hog.c`, experiment logs, `portal.list` IP.
  - [`b42820ce`](https://github.com/WillK13/Infiniswap_multi/commit/b42820ce) “Add memtier swap results and plotting script”.
  - Several commits with keyboard-mash messages (`gnrihuoghre`, `rhuoregh`, …) that mostly add `results/**` logs.
- **Code deltas that matter:**
  - `is_mq.c`: always `#define LOOKUP_BDEV(x) lookup_bdev((x), 0)` (drops autoconf `HAVE_LOOKUP_BDEV_PATCH` branch).
  - `setup/install.sh`: `backup_disk="/dev/vdb"`; `setup/infiniswap_bd_setup.sh` uses `/usr/local/bin/nbdxadm`; `portal.list` → CloudLab-style `128.110.96.28:9400`.
  - New harness: `experiments/run_memtier_swap_test.sh`, `run_mixed_memc_redis.sh`, plot scripts; `memcached_bench.c` / `memcached_hog.c`.
- **Why it matters:** Fresh (2025) evidence of someone successfully driving memcached/redis under Infiniswap swap; reusable benchmark scripts.
- **Caveats:** Repo is polluted with `.o`, `autom4te.cache`, `.ko`, and huge CSV/JSON logs; **no multi-daemon / multi-client protocol work** despite the name.

### 6. juncgu/infiniswap `GUI` — Medium (ops / observability)

- **Branch:** [GUI](https://github.com/juncgu/infiniswap/tree/GUI) — compare shows **+74 / −3** vs current upstream.
- Adds `infiniswap_gui/`, `cluster_setup/` (Grafana JSON dashboards, expect scripts, `setupall.sh`), and `infiniswap_bd/dashboard.c` telemetry.
- Useful if you want cluster-wide visibility; not a kernel modernization path.

### 7. Wiflin/Infiniswap — Medium (targeted bugfix)

- **Compare:** [`...Wiflin:master`](https://github.com/SymbioticLab/Infiniswap/compare/master...Wiflin:master) → `ahead_by: 1`.
- **Commit message:** “[Bugfix] Misusing variable leads to kernel panic on cloning io request.”
- **Patch in `is_mq.c` `stackbd_make_request2`:** iterate `for (; b; b = b->bi_next)` and set `bi_end_io` / `bi_private` on **every** cloned bio; upstream only set end_io on the last bio in the chain.
- Small, high-signal fix; same idea appears in [sctb512/myInfiniswap](https://github.com/sctb512/myInfiniswap).

### 8. sctb512/myInfiniswap — Medium (independent copy + experiments)

- Not a GitHub fork of SymbioticLab (`fork: false`), description: “Infiniswap with modification.”
- Core: same bio end_io chain fix as Wiflin; NULL checks on `req`/`bio`; daemon `die()` no longer `exit`s; helper `get_dst_ip`.
- Large `exp/` directory (memcached, TPCC, GAPBS, Docker/LXC CPU-rate scripts). Opaque commit messages (“update files”).

### 9. tomatolike/Infiniswap — Medium–Low (deploy experiment dump)

- **+48** commits: install scripts (`install_infinibd.sh`, `install_lxc.sh`, `install_pmem.sh`), daemon free-mem threshold commented out / lowered to 2 GB, `MAX_FREE_MEM_GB` 64, debug logging.
- Scripts contain bash bugs (e.g. `if [$# -eq 0]` missing spaces in `install_infinibd.sh`). Treat as a lab notebook, not a clean patch series.

### 10. juncgu/infiniswap `infinifs` — Medium–Low

- Adds `infinifs/kern` (super/inode/dir) and `infinifs/user` (`mkfs`, format) — filesystem prototype over remote memory ([tree](https://github.com/juncgu/infiniswap/tree/infinifs)).
- Interesting research direction; incomplete vs productizing Infiniswap swap itself.

### 11–12. sakura0423 / zxiangwei — Low

- **sakura0423 (+13):** global read/write counters and `do_gettimeofday` `pr_info` spam in `IS_request` / `IS_transfer_chunk`; minor install/`portal.list` edits.
- **zxiangwei (+9):** Chinese explanatory comments on structs/fields only ([compare](https://github.com/SymbioticLab/Infiniswap/compare/master...zxiangwei:master)).

---

## Empty / mirror forks (brief)

Identical or non-ahead vs `SymbioticLab:master` (surveyed via Compare API or push timestamp == upstream tip):

| Repo | Notes |
|---|---|
| [AK2000/Infiniswap](https://github.com/AK2000/Infiniswap) | `ahead_by: 0` despite push 2024-11-22 (likely sync/force without unique commits) |
| [blakecaldwell/Infiniswap](https://github.com/blakecaldwell/Infiniswap) | identical |
| [JackChuang/infiniswap](https://github.com/JackChuang/infiniswap) | **behind** 13 |
| [juncgu/infiniswap `master`](https://github.com/juncgu/infiniswap) | **behind** 1 on `master` (valuable work is on other branches) |
| [Zildj1an/infiniswap](https://github.com/Zildj1an/infiniswap) | behind 13 |
| Dozens of network forks | Same `pushed_at` as upstream `2020-09-26` (e.g. LSaga, gengxiandada, rishards, junglehust, …) |

Also low-signal / empty related repos:

- [r-ejilemele/Infiniswap](https://github.com/r-ejilemele/Infiniswap) — only `.gitignore` + README (2026-06).
- [a-staggs/INFINISWAP-689-Project](https://github.com/a-staggs/INFINISWAP-689-Project) — empty repository.
- [joysmariajoseph/Performance-Enhancement-in-memory-disaggregated-systems](https://github.com/joysmariajoseph/Performance-Enhancement-in-memory-disaggregated-systems) — README claims remote backup pages / fault tolerance, but tree only has unmodified `Infiniswap-master-ORIGINAL` + wrapper setup scripts.

---

## Related projects (not Infiniswap forks)

| Project | Relation | Notes |
|---|---|---|
| [clusterfarmem/fastswap](https://github.com/clusterfarmem/fastswap) | Sibling far-memory RDMA swap | Kernel **4.11** patches + farmem server; 86★; last push 2023-11. Strong modernization reference. |
| [clusterfarmem/cfm](https://github.com/clusterfarmem/cfm) | Fastswap experiment framework | Multi-job far-memory experiments. |
| [yashlala/fastswap-linux-5.16.16](https://github.com/yashlala/fastswap-linux-5.16.16) | Fastswap port | Port toward Linux 5.16 — useful if comparing “how others modernized swap-over-RDMA”. |
| [SongyuanGuan/infiniswapGUI](https://github.com/SongyuanGuan/infiniswapGUI) | GUI work absorbed into juncgu `GUI` | Historical GUI effort. |

No notable GitHub hits for a distinct “remoteswap” Infiniswap fork under that name in this survey.

---

## Sources / how compared

1. Upstream metadata: `gh api repos/SymbioticLab/Infiniswap`.
2. Fork listing: `gh api repos/SymbioticLab/Infiniswap/forks?sort=newest&per_page=100`.
3. Repo search: `gh search repos "infiniswap" --sort=updated` and `--include-forks=only`; also `fastswap`, `tamuSRM`, `Infiniswap_multi`.
4. Per-fork uniqueness:  
   `gh api repos/SymbioticLab/Infiniswap/compare/master...OWNER:BRANCH`  
   → `ahead_by`, commit subject list, and `.files[].{filename,patch}`.
5. Deep reads: raw patches for `infiniswap_bd/{is_mq.c,is_main.c,is_configfs.c,infiniswap.h}`, `infiniswap_daemon/rdma-common.c`, `setup/*`, READMEs; commit file lists via `repos/OWNER/REPO/commits/SHA`.
6. Author branches discovered via `gh api repos/juncgu/infiniswap/branches` → `GUI`, `ecswap`, `infinifs`.

**Classification rubric used**

- **High:** substantive kernel/API or data-path change, or production-quality modernization docs with matching code.
- **Medium:** useful bugfix, experiment harness, or observability; limited core impact.
- **Low:** comments / debug prints only.
- **Empty mirror:** `ahead_by == 0` (or only behind) with no unique commits.
