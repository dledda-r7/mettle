# LLVM/Clang Build Support

## Overview

The mettle build system supports an **optional** LLVM/Clang toolchain as an
alternative to the default musl-cross GCC. This is activated by passing
`USE_LLVM=1` to `make`. The GCC path remains the default and is entirely
unaffected.

The LLVM path replaces:

| GCC component | LLVM replacement |
|---------------|------------------|
| `{target}-gcc` | `clang --target={triple}` |
| `{target}-g++` | `clang++ --target={triple}` |
| `{target}-ar` | `llvm-ar` |
| `{target}-ranlib` | `llvm-ranlib` |
| `{target}-ld` (GNU ld) | `ld.lld` |
| `{target}-strip` | `llvm-strip` |
| `{target}-objcopy` | `llvm-objcopy` |
| `{target}-nm` | `llvm-nm` |

The musl-cross toolchain is **still required** — Clang uses its sysroot
(musl headers, CRT objects, libc.a) and GCC's `crtbeginS.o`/`crtendS.o`/`libgcc.a`.

## Currently Supported Targets

| Target | Architecture | Notes |
|--------|--------------|-------|
| `x86_64-linux-musl` | x86-64 | Baseline |
| `i486-linux-musl` | x86 32-bit | |
| `aarch64-linux-musl` | ARM64 | |
| `armv5l-linux-musleabi` | ARM 32-bit LE | ARMv5TE, soft-float |

## Prerequisites

Install the LLVM toolchain on the build host:

```bash
# Debian/Ubuntu/Kali
apt install clang lld llvm

# Fedora
dnf install clang lld llvm

# macOS
brew install llvm
```

Minimum version: LLVM 17 (tested with LLVM 18 and 21).

The musl-cross toolchain must also be present (it will be downloaded
automatically on first build, same as with GCC).

## Usage

### Single target

```bash
make TARGET=x86_64-linux-musl USE_LLVM=1
make TARGET=armv5l-linux-musleabi USE_LLVM=1
```

### All LLVM-supported targets

```bash
make llvm-all
```

### Other aggregate targets

```bash
make llvm-clean       # clean all LLVM target build dirs
make llvm-distclean   # remove all LLVM target build dirs entirely
make llvm-install     # install all LLVM targets to metasploit-framework
```

### Per-target targets

```bash
make x86_64-linux-musl.llvm-build
make aarch64-linux-musl.llvm-clean
make armv5l-linux-musleabi.llvm-install
```

### Verbose output

```bash
make TARGET=x86_64-linux-musl USE_LLVM=1 V=1
```

## Files Modified/Created

| File | Purpose |
|------|---------|
| `make/Makefile.llvm` | LLVM toolchain definitions, target validation, arch flags |
| `make/Makefile.tools` | Conditional GCC/LLVM selection via `USE_LLVM=1` |
| `make/Makefile.common` | Exports `LD`, `NM`, `STRIP`, `OBJCOPY` to env for LLVM |
| `Makefile` | `llvm-all` / `llvm-clean` / per-target `.llvm-build` targets |
| `util/elf2bin.c` | Fixed to use SYMTAB `sh_link` for strtab lookup (lld compat) |

## Architecture of the LLVM Integration

```
make TARGET=armv5l-linux-musleabi USE_LLVM=1
     │
     ├── Makefile.tools
     │     └── includes make/Makefile.llvm (instead of setting GCC vars)
     │
     ├── Makefile.llvm
     │     ├── validates target is in LLVM_SUPPORTED_TARGETS
     │     ├── detects clang, lld, llvm-ar, etc. on PATH
     │     ├── sets CC = clang --target=... --sysroot=... --gcc-toolchain=...
     │     ├── adds per-arch flags (e.g. -march=armv5te -mfloat-abi=soft)
     │     └── adds -Wno-error flags for Clang-specific diagnostics
     │
     ├── Makefile.common
     │     └── passes CC, LD, AR, etc. to dependency configure scripts via ENV
     │
     └── Makefile.mettle
           ├── builds dependencies (libpcap, mbedtls, json-c, curl, etc.)
           ├── configures and builds mettle
           └── runs elf2bin to produce .bin payload
```

Key design points:

- **Single Clang binary** cross-compiles to all targets via `--target=`. No
  per-arch compiler download needed.
- **`--gcc-toolchain`** tells Clang where to find `crtbeginS.o`, `crtendS.o`,
  and `libgcc.a` from the musl-cross tree.
- **`--sysroot`** points to the per-target musl directory containing headers
  and `libc.a`.
- **`-fuse-ld=lld`** uses the LLVM linker instead of per-target GNU ld.
- **`--rtlib=libgcc`** uses GCC's compiler builtins (from musl-cross) since
  cross-target `compiler-rt` may not be installed.

## How to Add a New Architecture

### Step 1: Verify the sysroot exists

The musl-cross toolchain must have a sysroot for the target:

```bash
# Check for required files:
ls build/tools/musl-cross/<TARGET>/lib/crt1.o
ls build/tools/musl-cross/<TARGET>/lib/libc.a
ls build/tools/musl-cross/<TARGET>/include/stdio.h
find build/tools/musl-cross/lib/gcc/<TARGET> -name 'libgcc.a'
find build/tools/musl-cross/lib/gcc/<TARGET> -name 'crtbeginS.o'
```

### Step 2: Test Clang can compile for the target

```bash
/usr/bin/clang \
  --target=<TARGET> \
  --sysroot=build/tools/musl-cross/<TARGET> \
  --gcc-toolchain=build/tools/musl-cross \
  -fuse-ld=lld --rtlib=libgcc \
  <ARCH_FLAGS> \
  -o /dev/null -x c - <<< 'int main(){return 0;}'
```

Replace `<ARCH_FLAGS>` with any architecture-specific flags needed (see examples
below). If this exits cleanly (exit code 0), Clang supports the target.

### Step 3: Add the target to `make/Makefile.llvm`

1. Add to `LLVM_SUPPORTED_TARGETS`:

```makefile
LLVM_SUPPORTED_TARGETS = x86_64-linux-musl i486-linux-musl aarch64-linux-musl armv5l-linux-musleabi <NEW_TARGET>
```

2. Add architecture-specific flags (if needed) in the "Per-target architecture
   flags" section:

```makefile
ifneq (,$(findstring <match_string>,$(TARGET)))
    CFLAGS += <ARCH_FLAGS>
endif
```

### Step 4: Add to `Makefile` LLVM_ARCHES

```makefile
LLVM_ARCHES = x86_64-linux-musl i486-linux-musl aarch64-linux-musl armv5l-linux-musleabi <NEW_TARGET>
```

### Step 5: Build and verify

```bash
make TARGET=<NEW_TARGET> USE_LLVM=1
file build/<NEW_TARGET>/bin/mettle
file build/<NEW_TARGET>/bin/mettle.bin
```

### Architecture Flag Reference

| Target | Flags needed | Notes |
|--------|-------------|-------|
| `x86_64-linux-musl` | (none) | |
| `i486-linux-musl` | (none) | |
| `aarch64-linux-musl` | (none) | |
| `armv5l-linux-musleabi` | `-march=armv5te -mfloat-abi=soft` | ARMv5TE soft-float |
| `armv5b-linux-musleabi` | `-march=armv5te -mfloat-abi=soft -mbig-endian` | Big-endian ARM |
| `mipsel-linux-muslsf` | `-msoft-float` | MIPS little-endian soft-float |
| `mips-linux-muslsf` | `-msoft-float` | MIPS big-endian soft-float |
| `mips64-linux-muslsf` | `-msoft-float -mabi=64` | MIPS64 soft-float |
| `powerpc64le-linux-musl` | (none) | PPC64LE |
| `s390x-linux-musl` | (none) | IBM SystemZ |
| `powerpc-linux-muslsf` | `-msoft-float` | PPC32 soft-float |
| `powerpc-e500v2-linux-musl` | N/A | **Not supported** — LLVM lacks SPE |

## Troubleshooting

### `ld.lld: error: cannot open crtbeginS.o`

The `--gcc-toolchain` path is wrong or the musl-cross toolchain hasn't been
unpacked. Verify:

```bash
find build/tools/musl-cross/lib/gcc/<TARGET> -name 'crtbeginS.o'
```

### `error: implicit function declaration` breaks configure checks

Already handled via `-Wno-error=implicit-function-declaration` in
`Makefile.llvm`. If a new configure check fails for similar reasons, add the
corresponding `-Wno-error=<diagnostic>` flag.

### `elf2bin: Unable to locate entry point '_start_c'`

This was caused by lld ordering `.shstrtab` before `.strtab` in the section
headers. Fixed in `util/elf2bin.c` by using the SYMTAB section's `sh_link`
field to locate the correct string table (the ELF-standard approach).

### Duplicate symbol errors with lld

lld is stricter than GNU ld about duplicate/weak symbol handling. If a symbol
is defined in both mettle source and musl libc, the autoconf detection may have
failed (see implicit-function-declaration issue above). Verify the configure
check succeeded by inspecting `build/<TARGET>/mettle/config.log`.

### LLVM target not supported

Some targets have limited or no LLVM support:

- `powerpc-e500v2-linux-musl` — SPE (Signal Processing Extension) is not
  implemented in LLVM. Must remain on GCC.
- Very old ARM variants — test with the Step 2 command above.

## Adding an Architecture Not in the musl-cross Tarball

The pre-built musl-cross tarball on S3 only covers the 14 architectures listed
in the `ARCHES` file. To add a new architecture (e.g., `riscv64-linux-musl`)
that isn't in that tarball, you need to build a sysroot yourself.

### What the sysroot must contain

Clang needs these files under a sysroot directory:

```
<sysroot>/
├── include/           ← musl C library headers (stdio.h, stdlib.h, etc.)
├── lib/
│   ├── crt1.o         ← C runtime startup (from musl)
│   ├── crti.o         ← init section prologue (from musl)
│   ├── crtn.o         ← init section epilogue (from musl)
│   └── libc.a         ← musl static C library
```

Additionally, you need GCC runtime objects (unless using compiler-rt):

```
<gcc-lib-dir>/
├── crtbeginS.o        ← GCC frame init
├── crtendS.o          ← GCC frame fini
└── libgcc.a           ← compiler builtins
```

### Option A: Build sysroot with musl-cross-make (recommended)

[musl-cross-make](https://github.com/richfelker/musl-cross-make) is the
standard tool for building musl-targeting cross-toolchains.

```bash
git clone https://github.com/richfelker/musl-cross-make
cd musl-cross-make

# Configure for your target
cat > config.mak << 'EOF'
TARGET = riscv64-linux-musl
OUTPUT = $(CURDIR)/output
EOF

# Build (downloads GCC, binutils, musl source, compiles everything)
make -j$(nproc)
make install
```

This produces a complete toolchain under `output/`. Extract what mettle needs:

```bash
METTLE=/path/to/mettle
TARGET=riscv64-linux-musl

# Copy sysroot (headers + CRT + libc)
mkdir -p $METTLE/build/tools/musl-cross/$TARGET
cp -a output/$TARGET/include $METTLE/build/tools/musl-cross/$TARGET/
cp -a output/$TARGET/lib     $METTLE/build/tools/musl-cross/$TARGET/

# Copy GCC runtime (libgcc.a, crtbeginS.o, crtendS.o)
GCC_VER=$(ls output/lib/gcc/$TARGET/)
mkdir -p $METTLE/build/tools/musl-cross/lib/gcc/$TARGET/$GCC_VER
cp output/lib/gcc/$TARGET/$GCC_VER/{libgcc.a,crtbegin*.o,crtend*.o} \
   $METTLE/build/tools/musl-cross/lib/gcc/$TARGET/$GCC_VER/
```

### Option B: Build musl from source + use compiler-rt (no GCC dependency)

This eliminates the need for libgcc entirely but requires cross compiler-rt.

```bash
# 1. Build musl from source
git clone https://git.musl-libc.org/cgit/musl
cd musl

# Use Clang as the compiler for musl itself
CC="clang --target=riscv64-linux-musl" \
./configure --prefix=/opt/sysroots/riscv64-linux-musl --target=riscv64
make -j$(nproc)
make install

# 2. Install compiler-rt builtins for riscv64
# On Debian/Ubuntu:
apt install libclang-rt-21-dev-riscv64-cross

# Or build from source:
cmake -S llvm-project/compiler-rt -B build-rt \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_C_COMPILER_TARGET=riscv64-linux-musl \
  -DCMAKE_SYSROOT=/opt/sysroots/riscv64-linux-musl \
  -DCOMPILER_RT_BUILD_BUILTINS=ON \
  -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DCMAKE_INSTALL_PREFIX=/usr/lib/clang/21
cmake --build build-rt
cmake --install build-rt
```

Then in `Makefile.llvm`, add the target without `--gcc-toolchain` and use
`--rtlib=compiler-rt` instead. The existing logic already handles this:
if `libgcc.a` is not found under the musl-cross tree, it falls back to
compiler-rt automatically.

### Option C: Use the RISC-V GNU Toolchain

The official RISC-V Foundation toolchain can also produce musl sysroots:

```bash
git clone https://github.com/riscv-collab/riscv-gnu-toolchain
cd riscv-gnu-toolchain
./configure --prefix=/opt/riscv --with-arch=rv64gc --with-abi=lp64d
make musl -j$(nproc)
```

Extract the sysroot from `/opt/riscv/sysroot/` using the same approach as
Option A.

### Additional requirement: libreflect assembly

The `libreflect` library uses per-architecture inline assembly for reflective
code execution. If the target doesn't have an `arch_jump.h`, libreflect falls
back to `memfd_create`/`execveat` (functional but less stealthy).

To add native reflective loading support, create:

```
libreflect/arch/linux/<host_cpu>/arch_jump.h
```

Where `<host_cpu>` is the value autoconf resolves for `$host_cpu` (e.g.,
`riscv64`). The file needs a single macro:

```c
#ifndef ARCH_JUMP_H
#define ARCH_JUMP_H

#define JUMP_WITH_STACK(jump_addr, jump_stack) \
        __asm__ volatile ( \
                        "mv sp, %[stack]\n" \
                        "jr %[entry]" \
                        : /* no outputs */ \
                        : [stack] "r" (jump_stack), [entry] "r" (jump_addr) \
                        : "memory" \
                        )

#endif
```

The assembly must:
1. Set the stack pointer register to `jump_stack`
2. Jump (not call) to `jump_addr`

If this file is absent, `configure` detects it and uses the `memfd_create`
fallback — so this step is **optional** for initial bring-up.

### Integration steps (after sysroot is in place)

Once the sysroot files are under `build/tools/musl-cross/<TARGET>/`, follow
the standard steps:

1. Verify Clang can compile:
   ```bash
   clang --target=riscv64-linux-musl \
     --sysroot=build/tools/musl-cross/riscv64-linux-musl \
     --gcc-toolchain=build/tools/musl-cross \
     -fuse-ld=lld --rtlib=libgcc \
     -o /dev/null -x c - <<< 'int main(){return 0;}'
   ```

2. Add to `LLVM_SUPPORTED_TARGETS` in `make/Makefile.llvm`
3. Add to `LLVM_ARCHES` in `Makefile`
4. Add arch-specific CFLAGS if needed (riscv64 needs none for the default
   rv64gc/lp64d configuration)
5. Build: `make TARGET=riscv64-linux-musl USE_LLVM=1`
6. Add to `ARCHES` file if it should also be built with the GCC path
