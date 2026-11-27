# XCL loader phases after PE mapping

Research snapshot: [`xcl-dev/xcl@f9a0cb8`](https://github.com/xcl-dev/xcl/tree/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda), 2026-11-27.

XCL does not launch the Scarlett executable through the Windows PE loader. The launcher starts a second copy of `xcl.exe`, makes the command line look like the title's command line, and passes a `boot.json` path through `XCL_HOST`. That host process manually maps the title and executes the title's native x64 entry point inside the XCL process. A packaged game DLL follows nearly the same mapping path, but gets TLS process-attach callbacks and `DllMain`; the main executable instead gets a dedicated game thread and its PE entry point.

## Phase 1: launcher preparation

Before the host maps anything, the launcher:

1. Resolves the game root, executable, title name, and `xcl.toml`.
2. Creates the title data directories: `logs`, `cache`, `overlay`, `saves`, `pls`, and `temp`.
3. Reads `MicrosoftGame.Config`, applies configuration/device overrides, and removes old TLS-carrier files.
4. Writes `<data>/cache/boot.json` with the executable path, read-only game root, writable data root, and resolved configuration.
5. Rotates the previous log and report.
6. Inspects the PE imports, runs GPU/toolchain preflight checks, and rejects a CPU missing the Series Zen 2 instruction-set baseline unless explicitly overridden.
7. Calls `CreateProcessW` on `xcl.exe` itself. The new process inherits `XCL_HOST=<boot.json>`, uses the game root as its current directory, and receives a command line beginning with the game executable path.

Source: [`launcher.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/launcher.rs#L300-L499), [`main.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/main.rs#L59-L70).

## Phase 2: host runtime setup

The second `xcl.exe` sees `XCL_HOST` and enters `host_main`. It prepares the compatibility environment before title code can run:

1. Loads `boot.json`; starts file/stderr logging and panic logging.
2. Stores global boot/configuration state and performs any configured pre-launch shader preparation.
3. Starts optional activity, thread-dump, and graphics-statistics workers.
4. Builds the stub-return database and HLE registry. The registry routes names such as `kernelx.dll`, `xgameruntime.dll`, `d3d12_xs.dll`, `xg_xs.dll`, `xcrdapi.dll`, `acphal.dll`, `xapu.dll`, `chad.dll`, `gameinput.dll`, and relevant API-set patterns.
5. Sets the TLS-carrier cache and DLL search order: executable directory first when distinct, then game root.
6. Creates the VFS. Console mounts are translated to host directories; the game root is read-only; writes use the per-title overlay; deletions become whiteouts.
7. Installs process-wide Win32 file hooks, the `RtlPcToFileHeader` hook, unhandled-crash/termination handlers, and graphics backend preparation.
8. Marks the process as XCL host mode, then calls `load_main`.

The VFS hooks cover common file opens, attributes, free-space queries, directory operations, read/copy/move/replace, and enumeration. They hook `KERNELBASE`/`KERNEL32`, so CRT-mediated file access is intercepted too. Direct `Nt*` file syscalls remain a documented gap.

Source: [`host.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/host.rs#L1-L177), [`hle/mod.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/hle/mod.rs#L82-L232), [`vfs_hooks.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/vfs_hooks.rs#L1182-L1240).

## Phase 3: reserve and copy the PE image

`map_image_ex` performs the mechanical image mapping:

1. Reads the PE file and parses it as an x64 PE image.
2. Reserves and commits `SizeOfImage` at the preferred image base as read/write memory.
3. If that address is occupied, allocates elsewhere only when the image has relocations; otherwise loading fails.
4. Copies PE headers and each section's raw bytes to its virtual address. Uninitialized section tails remain zero-filled because `VirtualAlloc` returned zeroed pages.
5. Applies `DIR64` and `HIGHLOW` base relocations when the actual base differs. Unsupported relocation kinds are warned about.
6. Builds an owned export table and a `MappedModule` record containing name, path, base, size, entry RVA, DLL flag, TLS callbacks, initialization state, and stack reserve.
7. Registers the module before resolving imports. This makes recursive/cyclic game-DLL imports terminate instead of mapping the same image repeatedly. The main executable is also recorded as the process's logical main module.

At this point the bytes exist in memory, but the image is not ready to execute.

Source: [`loader.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/loader.rs#L58-L166), [`modules.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/modules.rs#L1-L105).

## Phase 4: resolve and patch every import

XCL parses both the normal import directory and delay-import directory. Delay imports are resolved eagerly rather than waiting for their first call.

For each imported symbol, the resolver uses this practical order:

1. Configured alias/route: HLE module, `@system`, or `@stub`.
2. Routed HLE export; then cross-HLE lookup; then the routed module's permitted Windows fallback DLLs.
3. An already mapped game DLL.
4. A matching DLL in the configured game search directories. XCL recursively maps and initializes it.
5. A real Windows DLL through `LoadLibraryW` and `GetProcAddress`.
6. A known HLE function imported from an unexpected library.
7. An generated logging stub when no implementation exists.

Named and ordinal imports are supported. Forwarded exports are followed recursively up to eight levels. A forwarder can land in another game DLL, an HLE module, an API-set/system DLL, or a stub. Each resolved address is written into the image's IAT. For delay imports, XCL also fills the delay-load module-handle slot. Every resolution is recorded for `report.json`; optional `XCL_TRACE_IMPORTS` wrappers can trace calls and results.

A packaged DLL discovered during this phase runs through all mapping phases recursively. After that DLL is complete, XCL runs its TLS callbacks with `DLL_PROCESS_ATTACH`, calls `DllMain(base, DLL_PROCESS_ATTACH, nullptr)`, and marks it initialized. If `DllMain` returns `FALSE`, XCL removes it from the module registry so a later load retries it; its allocation is intentionally retained because executed initialization code may have left threads or callbacks referring to it.

Source: [`resolver.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/resolver.rs#L1-L347), [`loader.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/loader.rs#L167-L197), [`loader.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/loader.rs#L249-L311).

## Phase 5: make static TLS work

A manually mapped image is absent from the Windows loader's TLS bookkeeping. XCL compensates with one generated "TLS carrier" DLL per mapped image that has a TLS directory:

1. Builds a tiny ordinary DLL whose TLS template is at least as large as the title image's initialized data plus zero-fill area.
2. Writes it into the XCL cache and loads it with Windows `LoadLibraryW`.
3. Lets Windows allocate a genuine static TLS slot for that carrier on existing and future threads.
4. Writes the carrier's index into the mapped image's `_tls_index` and copies the image's TLS template into each existing thread's block.
5. Records the mapped image's TLS callback array.
6. For the main executable, swaps the title into TLS slot 0 and moves XCL's own TLS to the carrier slot. This supports `/GA` code that directly assumes the process executable owns slot 0.
7. XCL's own TLS callback initializes and swaps blocks for every later thread before forwarding thread notifications to mapped images.

Source: [`tls.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/tls.rs#L1-L23), [`tls.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/tls.rs#L245-L419), [`tls.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/tls.rs#L449-L503).

## Phase 6: finalize memory and exception behavior

After imports and TLS are ready, XCL:

1. Makes PE headers read-only.
2. Converts section characteristics to final Windows page protections: read, write, execute, or combinations.
3. Flushes the process instruction cache.
4. Registers the exception directory's runtime-function table with `RtlAddFunctionTable`, enabling x64 unwind through mapped code.
5. Uses the global `RtlPcToFileHeader` hook to make mapped address ranges discoverable by C++ exception machinery even though they are absent from Windows' normal loader lists.
6. Scans executable sections for MSVC security-cookie failure paths and redirects those failures into XCL diagnostics before terminating with the original fail-fast status.

`map_image` now returns. Its contract is important: mapping itself does not execute the main image's code.

Source: [`loader.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/loader.rs#L199-L246), [`eh.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/eh.rs#L1-L62), [`eh.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/eh.rs#L67-L190).

## Phase 7: post-map title handoff

The main executable differs from a DLL here:

1. XCL looks for the title data export `__XMemAllocationHooks__`. If both allocator pointers point inside the title image, XCL adopts them so compatibility libraries allocate through the title's allocator where required.
2. XCL writes an initial `report.json` snapshot before executing title code.
3. Except under Wine, it changes `PEB.ImageBaseAddress` to the mapped title base. Calls such as `GetModuleHandle(NULL)` and resource lookups then identify the title rather than `xcl.exe`.
4. It calls the title's TLS callbacks with `DLL_PROCESS_ATTACH`.
5. It enables future thread notifications only after the main image is ready.
6. It creates the game thread with the PE's `SizeOfStackReserve`, with a one-MiB minimum, using `STACK_SIZE_PARAM_IS_A_RESERVATION`.
7. The new thread calls the PE entry point at `base + AddressOfEntryPoint`. XCL passes the process PEB pointer as the entry argument.
8. The host thread waits indefinitely for the game thread and returns its exit code if the entry point returns. Normally the title ends through `ExitProcess` instead.

Source: [`host.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/host.rs#L178-L198), [`kernelx.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/hle/kernelx.rs#L609-L630), [`loader.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/loader.rs#L313-L379).

## Phase 8: runtime compatibility while native title code executes

The CPU executes the title's x64 instructions directly. Compatibility happens at the boundaries established above:

- IAT entries call XCL HLE functions, mapped game-DLL exports, real Windows exports, or logging stubs.
- Runtime `LoadLibrary*`, `GetModuleHandle*`, and `GetProcAddress` shims consult the same HLE/mapped/system module model. HLE libraries use stable fake `HMODULE` pages beginning with `MZ`; mapped game DLL handles are their image bases.
- VFS hooks translate console paths and enforce the read-only game folder even when calls originate in CRT or system DLL code.
- HLE objects translate console memory, graphics, audio, input, storage, and runtime APIs into Windows-facing implementations.
- On `DLL_THREAD_ATTACH`, XCL prepares TLS, then calls mapped-image TLS callbacks and initialized DLL `DllMain` functions unless `DisableThreadLibraryCalls` was requested. Detach happens in reverse module order.
- Missing static imports point to stubs that log actual calls and return configured/default values. Dynamic `GetProcAddress` probing returns `NULL` for missing optional APIs instead of inventing a stub.
- The exception/termination handlers record the failing module and RVA, registers, useful stack words, device-removal state, and a final report.

Source: [`resolver.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/resolver.rs#L349-L454), [`loader.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/loader.rs#L381-L424), [`modules.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/modules.rs#L106-L153).

## Phase 9: reporting and shutdown

XCL writes `report.json` before entry, after a normal return, on crashes, and from process-detach paths. The report contains:

- static import counts by resolution source;
- every statically stubbed import;
- called stubs, call counts, and first caller;
- mapped modules with base and size;
- unmapped VFS paths, redirected writes, and whiteouts;
- shader/DXC/D3D12, audio, XGameRuntime, and DirectStorage statistics;
- crash text and, after launcher post-processing, the process exit code and preflight environment.

The launcher waits for the host process, updates the report with exit/preflight data, and prints its summary.

Source: [`report.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/report.rs#L1-L188), [`launcher.rs`](https://github.com/xcl-dev/xcl/blob/f9a0cb80f9decb41d6f6315f6cf4e06270ba2bda/crates/xcl/src/launcher.rs#L501-L620).

## The short distinction

For a **game DLL**, "after mapping" means: resolve its dependency graph, install TLS, restore final protections, register unwind metadata, run TLS process-attach callbacks, call `DllMain(PROCESS_ATTACH)`, and expose its exports to the importer.

For the **title executable**, "after mapping" means: adopt title allocator hooks, make it the logical process image, run its TLS process-attach callbacks, create a correctly sized game thread, and jump to the native PE entry point while XCL's HLE, VFS, TLS, exception, and reporting machinery remains around it.
