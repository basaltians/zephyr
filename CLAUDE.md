# CLAUDE.md

## Context

This is the upstream Zephyr RTOS source tree, checked out as a module inside a larger west workspace rooted at `~/AES67`. The workspace's manifest/application repo (`aes67`, a sibling directory) builds firmware for Basalte products implementing **AES67** — an audio-over-IP networking standard built on RTP/RTCP media transport, PTP clock synchronization, and SAP session announcements.

Treat this directory as vendored upstream Zephyr: avoid adding product- or application-specific code here. Application code, custom boards, and drivers for AES67 products live in the `aes67` (and `bzf`) repos alongside this one.

## Code style

- Follow Zephyr's standard C style, enforced by the `.clang-format` at the repo root (`ColumnLimit: 100` — keep lines to 100 columns). Format code accordingly before finalizing changes.
- Keep comments minimal — only add one where the code's intent genuinely isn't clear from naming/structure (e.g. a non-obvious workaround or hardware quirk). Don't narrate what the code already says.

## Building

- The Zephyr SDK is already installed and configured correctly on this machine — assume its presence, don't attempt to install or reconfigure it.
- **Always do a pristine build.** Never reuse or incrementally rebuild an existing build directory — pass `-p` (or `-p always`) on every `west build` invocation:

  ```shell
  west build -p -b <board> <app-path>
  ```

- The same applies when running tests/samples via `twister`:

  ```shell
  west twister -p <platform> -T <test-or-sample-dir>
  ```

## About Zephyr RTOS

Zephyr is a small, scalable, real-time operating system (RTOS) for connected, resource-constrained devices, hosted by the Linux Foundation. Key things to know when working in this tree:

- **Architectures & boards**: supports ARM Cortex-M/A/R, x86, RISC-V, Xtensa, ARC, and SPARC across hundreds of boards.
- **west**: the meta-tool used for multi-repository management, building, flashing, and debugging (`west build`, `west flash`, `west debug`, `west twister`, etc.).
- **Devicetree**: hardware topology and configuration is described in `.dts`/`.dtsi`/`.overlay` files under `dts/` and per-board directories, compiled at build time.
- **Kconfig**: compile-time feature/configuration selection (`Kconfig` files throughout the tree, set via `prj.conf`, board `.conf` fragments, etc.).
- **Build system**: CMake-based, driven by `west build`/`cmake` + `ninja`.
- **Key subsystems**: kernel (scheduler, threads, IPC), `drivers/` (peripheral driver framework), networking stack (`subsys/net`), Bluetooth, file systems, USB, sensor framework, and more.
- **Testing**: `twister` is Zephyr's test runner — it builds (and optionally executes, on QEMU/native_sim/real hardware) tests and samples across boards and configurations; used both locally and in CI.
- **Docs**: https://docs.zephyrproject.org
