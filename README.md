# scope-acquire

## Overview

**scope-acquire** is a lightweight C framework for oscilloscope trace acquisition over _VISA_.  

Provides:
- an **Acquisition Engine** that runs a user-defined `acquire` function (per acquisition attempt).
- a **writer thread** that handles trace storage asynchronously, avoiding I/O bottlenecks.  

The framework is modular: new oscilloscope models can be supported by adding drivers.  
Currently supported: **Rigol DS1000ZE**.  

Compatible with macOS and Linux (NI-VISA or other).

---

## Project Structure

```
scope-acquire/
├── engine/            # Core engine
├── scope/             # Scope abstraction + drivers
│   ├── rigol/         # Rigol DS1000ZE driver
│   └── scope.c
├── build/             # Build artifacts
├── main.c             # Example entry point (defines which driver is used)
├── example_acquire.c  # Example acquisition procedure
├── Makefile
└── README.md
```

---

## Getting Started

### 1. Prerequisites
- **C toolchain**: GCC or Clang with C11 support  
- **VISA library**:
  - macOS: [NI-VISA](https://www.ni.com/visa/)  
  - Linux: `libvisa` (distribution-specific package)

### 2. Build

```bash
# Clone repository
git clone https://github.com/eduardoffcruz/scope-acquire.git
cd scope-acquire

# Build static library + your acquisition file
make acquire=my_acquire.c
```

> The `acquire` argument must point to your implementation of the `acquire()` function.  
> The scope driver in use is selected inside `main.c`.

Build output is placed in `build/`.

### 3. Usage

```bash
./build/example_acquire --help
```

Shows usage instructions.

### 4. Example Run

```bash
./build/example_acquire   --outfile /Volumes/my-ssd/acquisition   --ntraces 1000   --batch 100   --coding 0   --channels CHAN1,CHAN2   --verbose
```

This connects to the first VISA instrument found and acquires **1000 traces**.  
The `--batch` parameter controls how many traces are written per flush by the writer thread. Omit `--outfile` to run acquisition without storing traces.

### 5. Diagnostic Mode

```bash
./build/example_acquire --diagnose
```

Prints oscilloscope configuration and supported features without running an acquisition.
