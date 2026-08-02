# Contributing to PULP

Thank you for your interest in contributing to PULP! 

PULP is a high-throughput, native Windows C11 telemetry library. Codebase contributions must adhere to strict constraints regarding zero-allocation hot-paths, direct platform optimization, and thread safety.

---

## 1. Contributor License Agreement (CLA)

All contributors must sign the **Contributor License Agreement (CLA)** before any Pull Request can be merged. 

- When you open a PR, our automated bot (`cla-assistant`) will prompt you to review and sign the agreement via GitHub OAuth.
- For details, read our [`CLA.md`](../CLA.md).

---

## 2. Build Requirements & Prerequisites

To build and run tests locally:

- **OS:** Windows x64 (Windows Server 2016+ or Windows 10/11).
- **Compiler:** MSVC (Visual Studio 2022) with C11 support.
- **Instruction Set:** AVX2 enabled (`/arch:AVX2`).

---

## 3. Technical & Engineering Guidelines

- **Native Windows C11:** Built strictly for x64 Windows environments. We leverage native Win32 APIs, MSVC intrinsics, and OS-level primitives directly, zero cross-platform wrappers, zero abstraction tax.
- **Zero-Allocation Hot-Path:** `PulpWrite` must never perform dynamic memory allocations (`malloc`, `calloc`, `heap_alloc`) during execution.
- **Thread Safety:** Ensure all data structures accessed on hot-paths use lock-free or atomic primitives without lock contention.
- **Zero External Dependencies:** PULP relies exclusively on native Windows system APIs and the MSVC CRT.

---

## 4. Submission Checklist

Before submitting a Pull Request, ensure:

1. Code builds with **zero warnings** under Release x64 (`/arch:AVX2`).
2. You have run `LogProducer` and `PulpReader` to verify throughput and integrity.
3. No memory leaks or handle leaks were introduced.