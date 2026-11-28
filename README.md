# xcompat

An experimental native Xbox Series X|S (Scarlett) compatibility layer for 64-bit Windows.

xcompat uses native x86-64 execution and Windows NT facilities instead of CPU emulation. The long-term objective is to reproduce the Scarlett execution environment while translating incompatible operating-system and runtime behavior to Windows.

> Early research prototype. Commercial title execution is not implemented.

## Current state

Development currently has three working foundations: the native PE execution spine, the shared user-mode kernel MMU, and the `xmem.dll` memory surface.

### Native loader

The loader currently:

- Creates an isolated native guest with synchronized process reflection.
- Maps PE32+ images through NT image sections.
- Applies base relocations when an image cannot use its preferred address.
- Stages normal imports, delay imports, and export-forwarder dependencies before reflection.
- Resolves Windows API-set contracts and binds named, ordinal, forwarded, normal, and delay imports.
- Installs process metadata, x64 unwind tables, static TLS, DLL entry points, and thread notifications.
- Executes controlled PE32+ titles and returns their process exit code.

The controlled fixture path reaches `xmem.dll` and `xcptkrn.dll`; Scarlett title-specific process, graphics, storage, audio, and input behavior remains unimplemented. The known Scarlett runtime surface is tracked in the [first-glance implementation checklist](./docs/notes/01-first-glance.md).

### Shared kernel MMU

`xcptkrn.dll` now contains an internal 64 KiB-page MMU with:

- Pagefile-backed physical pages identified by opaque PFNs.
- NT placeholder reservations, splitting, coalescing, mapping, and unmapping.
- Shared backing and aliases across multiple virtual addresses.
- Persistent backing contents across unmap/remap cycles.
- Deferred backing destruction when a released PFN still has active mappings.
- Synchronized reservation and physical-page state.

The MMU has 17 regression tests and 6 deterministic stress tests registered through CTest. A synthetic Release benchmark covers allocation, reservation, mapping, aliasing, complete lifecycles, sparse mappings, and concurrent scaling. See the [first MMU benchmark report](./docs/notes/03-first-mmu-benchmark.md).

`xmem.dll` now routes 64 KiB physical-page allocation, reservation, mapping, aliasing, persistence, and release through the shared kernel MMU. Its default allocator honors encoded alignment, title allocation hooks are used when present, ordinary virtual allocations use the host VM, and retail tools-memory and host-backed working-set behavior are exposed through the public API. Kernel initialization, scheduling, and object-management layers remain incomplete.

## Architecture

Since Scarlett binaries are packaged in a PE32+ format, the binaries are relatively
compatible with Windows. This allows xcompat to load the binary in memory space similar
to how Windows loads an executable.

xcompat starts by creating a guest process in Windows and maps the Scarlett executable
into its memory space. The host process is responsible for managing the guest process,
loading modules, applying relocations, and resolving imported symbols.


![Loader Architecture](./docs/assets/xcompat-loader.drawio.svg)

Where possible, imported symbols are resolved directly to native Windows APIs. The planned
xcompat runtime will provide incompatible or Scarlett-specific interfaces and translate
their platform behavior into equivalent Windows functionality.

Since Scarlett is targeted to run on an x86-64 processor, guest code can execute natively
without CPU emulation. This allows xcompat to focus primarily on compatibility at the
operating system, runtime, and hardware interface levels.

## Repository layout

- `project/xcompat/` — native loader library.
- `project/xcompat-cli/` — command-line loader prototype.
- `project/runtime/kernel/` — shared user-mode kernel and MMU (`xcptkrn.dll`).
- `project/runtime/xmem/` — Scarlett memory API surface backed by the shared MMU (`xmem.dll`).
- `tests/runtime/kernel/` — native MMU regression tests, stress tests, and benchmark.
- `tests/runtime/xmem/` — public xmem API regression tests.
- `docs/` — implementation notes, benchmark results, and development logs.

## Building

Requirements:

- 64-bit Windows.
- CMake 4.0 or newer.
- Ninja.
- MSVC or Clang-CL in an initialized x64 developer environment.
- NASM for Hydra's assembly sources.

Configure and build Release:

```console
cmake --preset x64-Release -DBUILD_TESTING=ON
cmake --build build/x64-Release
```

Debug uses the `x86_64-Debug` configure preset.

`xcptkrn.dll` and `xmem.dll` use their real DLL CRT process-attach path when manually mapped. The obsolete unfinished `xcompat::krn::initialize()` entry point has been removed.

## Testing

Build and execute the MMU suite:

```console
cmake --build build/x64-Release --target xcompat-kernel-mmu-tests
ctest --test-dir build/x64-Release -L mmu --output-on-failure
```

CTest labels separate the 17 ordinary regression cases (`regression`) from the 6 deterministic stress cases (`stress`). The suite exercises physical-page lifetime, reservation rounding, real mapped-memory access, aliasing and persistence, placeholder subdivision and release, failure contracts, concurrency, large batches, repeated lifecycles, and sparse mappings.

The `xmem` label contains five public-API regression cases covering aligned allocations, contiguous and scatter physical mappings, aliases and remapping, virtual allocations, working-set budget transfer, and invalid parameters:

```console
cmake --build build/x64-Release --target xcompat-xmem-tests
ctest --test-dir build/x64-Release -L xmem --output-on-failure
```

The `loader` label covers PE validation, guest phase contracts, exact IAT protection restoration, named/ordinal/forwarded/delay binding, unresolved-symbol categories, and import cycles. The `guest` label executes controlled entry, constructor, TLS, exception, secondary-thread, stack, `xmem.dll`, and kernel-MMU milestones plus the real CLI:

```console
cmake --build build/x64-Release --target xcompat-cli xcompat-guest-tests
ctest --test-dir build/x64-Release -L loader --output-on-failure
ctest --test-dir build/x64-Release -L guest --output-on-failure
```

## Benchmarking

Build and run the synthetic MMU benchmark:

```console
cmake --build build/x64-Release --target xcompat-kernel-mmu-benchmark
build/x64-Release/runtime/xcompat-kernel-mmu-benchmark.exe > mmu-benchmark.csv
```

`--quick` selects a short smoke run. `--reservations` runs only the virtual reservation workload. Output is CSV suitable for plotting.

The first measured Release baseline reached approximately 469,000 physical page allocation/free operations per second with 64-page batches, 2.9 µs per map/unmap cycle, and 15.1 µs per complete single-page lifecycle on an AMD Ryzen 7 7730U. Concurrent complete lifecycles scale negatively in the current implementation; the benchmark report contains the methodology, graphs, and isolated reservation reruns.

## Known gaps

- Scarlett process, object, scheduling, storage, graphics, audio, and input services.
- Title-derived Scarlett ABI contracts beyond the controlled runtime fixtures.
- Stable broader kernel runtime ABI beyond the MMU surface.

## Development notes

- [First-glance implementation checklist](./docs/notes/01-first-glance.md)
- [Loader phases](./docs/notes/02-xcl-loader-phases.md)
- [First MMU benchmark](./docs/notes/03-first-mmu-benchmark.md)
- [Building the PE loader](./docs/devlog/01-building-the-pe-loader.md)

## Disclaimer

xcompat is an independent project and is not affiliated with or endorsed by Microsoft or
Xbox.
