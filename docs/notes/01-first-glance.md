# First-Glance Library Implementation Checklist

`game_scarlett_master.exe` imports exactly **37 Scarlett APIs from seven system libraries**. The list below was rechecked directly from its PE import table with `dumpbin /imports`; package DLLs such as `Bink2Scarlett.dll`, `XboxBvhBuilder.dll`, and `amd_fidelityfx_loader_xbox.dll` are outside this list.

## Library checklist

- [ ] `xmem.dll` — 12 exports. Implement first; graphics and audio depend on its address-space and physical-page model.
- [ ] `d3d12_xs.dll` — 3 exports. Root-signature serialization can forward; device creation needs an Xbox-ABI object proxy.
- [ ] `xg_xs.dll` — 2 exports. Implement with the same texture-layout rules used by the graphics resource path.
- [ ] `XCRDAPI.dll` — 1 export. Return a real DirectStorage factory object with the console vtable.
- [ ] `AcpHal.dll` — 9 exports. One shared APU heap, address translation, SHAPE contexts, and an ACP interface object.
- [ ] `xapu.dll` — 5 exports. Stateful command/result queues and lifetime rules.
- [ ] `chad.dll` — 5 exports. Stateful DSP clients, streams, commands, and status.

## Signature confidence

- **Documented**: signature and contract are in the public Xbox documentation linked under Sources.
- **Corroborated**: public documentation is restricted or absent, but the ABI is independently present in public implementations and agrees with the title's x64 call shape.
- **Provisional type name**: pointer width and argument order are established; an official GDK header is still required before freezing the public header's spelling.

All functions use the Windows x64 ABI (`WINAPI` / `extern "system"`). C++ default arguments shown in documentation do not change the exported ABI.

---

## `xmem.dll` — 12 exports

The core invariant is that 64 KiB physical pages are separate objects from virtual reservations. A reservation made mappable by `XMemVirtualAlloc` must survive map/unmap operations, and one physical page may be mapped at more than one virtual address. A pagefile-backed section plus Windows placeholders is the closest host model; host AWE is unsuitable because it normally requires `SeLockMemoryPrivilege`.

### [ ] `XMemAlloc`

```cpp
PVOID WINAPI XMemAlloc(SIZE_T Size, ULONGLONG Attributes);
```

Allocates ordinary or physical/cache-class memory according to `XALLOC_ATTRIBUTES`, including allocator ID, alignment, zero-initialization, and address constraints. It uses the active title allocation hook; `XMemAllocDefault` bypasses that hook. Return `NULL` on failure and preserve enough metadata for the matching free. **Documented/corroborated.**

### [ ] `XMemAllocDefault`

```cpp
PVOID WINAPI XMemAllocDefault(SIZE_T Size, ULONGLONG Attributes);
```

Default allocator used when no title hook is installed, and callable directly by a hook. It must honor the same observable alignment and attribute contract as `XMemAlloc`. **Documented/corroborated.**

### [ ] `XMemAllocatePhysicalPages`

```cpp
BOOL WINAPI XMemAllocatePhysicalPages(
    ULONG_PTR Flags,
    PULONG_PTR NumberOfPages,
    PULONG_PTR PageArray);
```

`NumberOfPages` is in/out: requested 64 KiB pages on entry, allocated count on return. Fill `PageArray` with opaque page-frame numbers; support the title's observed flags `0x180`, `0x280`, and `0x380`. Allocation may be partial only if the returned count and failure status agree. **Documented/corroborated.**

### [ ] `XMemFree`

```cpp
VOID WINAPI XMemFree(PVOID Address, ULONGLONG Attributes);
```

Releases an `XMemAlloc` allocation through the active free hook. Validate allocator ownership in debug builds; production behavior must not silently free foreign memory. **Documented/corroborated.**

### [ ] `XMemFreeDefault`

```cpp
VOID WINAPI XMemFreeDefault(PVOID Address, ULONGLONG Attributes);
```

Releases memory returned by `XMemAllocDefault` without invoking title hooks. **Documented.**

### [ ] `XMemFreePhysicalPages`

```cpp
BOOL WINAPI XMemFreePhysicalPages(
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray);
```

Returns page objects to the physical pool. Existing mapped views remain valid until explicitly unmapped; the page numbers must stop being accepted for new mappings. **Documented/corroborated.**

### [ ] `XMemGetWorkingSetStatistics`

```cpp
BOOL WINAPI XMemGetWorkingSetStatistics(
    DWORD WorkingSetFlags,
    PXMEM_WORKING_SET_STATISTICS Statistics);
```

Reports title/tools/stack/kernel-pool usage selected by flags. Populate every field represented by the requested structure version; title limits must reflect memory transferred away by `XMemTransferMemory`. **Documented.**

### [ ] `XMemIsToolsMemAvailable`

```cpp
BOOL WINAPI XMemIsToolsMemAvailable(void);
```

Returns whether profiling/tools memory exists. A retail-compatible process should return `FALSE` unless a tools partition is deliberately configured. **Documented.**

### [ ] `XMemMapPhysicalPages`

```cpp
PVOID WINAPI XMemMapPhysicalPages(
    PVOID VirtualAddress,
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray);
```

Replaces the selected part of a mappable reservation with views of the listed pages and returns `VirtualAddress`. `PageArray == NULL` unmaps while preserving the reservation. Mapping failure returns `NULL`; remapping an occupied range first removes the old views. **Documented/corroborated.**

### [ ] `XMemMapPhysicalPagesScatter`

```cpp
BOOL WINAPI XMemMapPhysicalPagesScatter(
    PVOID* VirtualAddressArray,
    ULONG_PTR NumberOfPages,
    PULONG_PTR PageArray);
```

Maps one 64 KiB page at each address. A null `PageArray`, or a zero entry, unmaps the corresponding address while preserving its reservation. The operation must handle the title's observed large batches rather than looping through a path with quadratic view lookup. **Documented/corroborated.**

### [ ] `XMemTransferMemory`

```cpp
BOOL WINAPI XMemTransferMemory(UINT32 Direction, SIZE_T Size);
```

Moves budget between title, system, and tools partitions; it does not copy bytes. Round or reject sizes according to the console's 2 MiB transfer granularity and update subsequent working-set limits atomically. The title has an observed `Direction = 1`, `Size = 0x1000000` call. The enum type name is **provisional**; width/order and behavior are corroborated.

### [ ] `XMemVirtualAlloc`

```cpp
PVOID WINAPI XMemVirtualAlloc(
    PVOID Address,
    SIZE_T Size,
    DWORD AllocationType,
    ULONG64 Attributes,
    DWORD Protect);
```

Console extension of `VirtualAlloc`: reserve/commit with 64 KiB or 2 MiB page modes, mappable placeholders, GPU protection bits, and address-range attributes. Strip only console-only bits after translating them; do not reject nonzero attributes. The title has 23 static call sites and depends on reservations made with `0x20402000`, `0x20403000`, `0x20406000`, `0x20003000`, and `0x20401000`. **Corroborated.**

---

## `d3d12_xs.dll` — 3 exports

### [ ] `D3D12ConfigureVirtualMemoryX`

```cpp
HRESULT WINAPI D3D12ConfigureVirtualMemoryX(
    const D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION* Configuration);
```

Consumes the title's 32-byte GPU virtual-address configuration before device use. On Windows the host D3D12 driver owns GPU VA placement, so the compatibility layer must validate and retain the configuration, initialize any guest-VA translation policy, then return `S_OK`; returning success while later ignoring guest GPU addresses is not sufficient. The structure name is **provisional**; the one-pointer ABI and 32-byte title payload are corroborated.

### [ ] `D3D12SerializeVersionedRootSignature`

```cpp
HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* RootSignature,
    ID3DBlob** Blob,
    ID3DBlob** ErrorBlob);
```

Forward to host `d3d12.dll`. Translate supported Xbox-only root-signature flags before forwarding, preserve all descriptor/range data, and return a useful error blob on failure. **Documented by the shared D3D12 ABI.**

### [ ] `D3D12XboxCreateDevice`

```cpp
HRESULT WINAPI D3D12XboxCreateDevice(
    IUnknown* Adapter,
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* Parameters,
    REFIID Riid,
    void** Device);
```

Create the host D3D12 device, then return an Xbox-ABI proxy rather than the raw PC object. The proxy must preserve COM identity/lifetime and expose the console's extended device, queue, command-list, heap, fence, and resource vtables. The title passes a 0x40-byte parameter block and checks the `HRESULT`. **Corroborated.**

---

## `xg_xs.dll` — 2 exports

### [ ] `XGCreateTextureComputer`

```cpp
HRESULT WINAPI XGCreateTextureComputer(
    const D3D12_RESOURCE_DESC* Description,
    IXGTextureComputer** Computer);
```

Validate the resource description, calculate Scarlett texture layout, and return a reference-counted layout computer. The object must expose the console vtable used for complete layout queries, subresource copies in both directions, and 64 KiB tiling. Never return success without writing `*Computer`. **Corroborated; interface type spelling is provisional.**

### [ ] `XGSuggestSwizzleMode`

```cpp
HRESULT WINAPI XGSuggestSwizzleMode(
    const D3D12_RESOURCE_DESC* Description,
    UINT Flags,
    D3D12_TEXTURE_LAYOUT* Layout);
```

Choose a layout from dimension and resource flags: linear for 1D, depth-target and render-target variants where required, otherwise the standard texture swizzle. Write the selected console layout value to `*Layout`; reject null pointers and unsupported descriptions. **Corroborated; no public GDK declaration was found.**

---

## `XCRDAPI.dll` — 1 export

### [ ] `DStorageGetFactory`

```cpp
HRESULT WINAPI DStorageGetFactory(REFIID Riid, void** Factory);
```

Return the process-wide DirectStorage factory for the requested console interface (`IDStorageFactoryX`, `X1`, or `X2`). The object must implement COM lifetime, queue creation, file opening, status arrays, staging-buffer sizing, and global error/state operations. Unsupported compression/swizzle paths must return a failure `HRESULT` with outputs cleared, never successful no-ops. **Documented.**

---

## `AcpHal.dll` — 9 exports

All nine exports share one process-wide APU memory manager. `APU_ADDRESS` is a 32-bit coprocessor address, not a truncated host pointer; maintain explicit virtual-to-APU and APU-to-virtual allocation maps.

### [ ] `AcpHalAllocateShapeContexts`

```cpp
HRESULT WINAPI AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* ContextArrays);
```

Allocate the requested shared SHAPE/XMA context arrays, fill every output pointer, and reject a second allocation until release. The number of SRC contexts must equal PCM plus XMA contexts. **Documented.**

### [ ] `AcpHalCreate`

```cpp
HRESULT WINAPI AcpHalCreate(IAcpHal** Acp);
```

Return a reference-counted ACP client object. It needs connection state, command submission, message retrieval, and flowgraph processing or an explicit failure for unsupported graphs. Contexts must already exist. **Documented.**

### [ ] `AcpHalReleaseShapeContexts`

```cpp
HRESULT WINAPI AcpHalReleaseShapeContexts(void);
```

Release the process-wide SHAPE/XMA arrays after clients stop using them; return the documented not-allocated error when no allocation exists. **Documented.**

### [ ] `ApuAlloc`

```cpp
HRESULT WINAPI ApuAlloc(
    void** VirtualAddress,
    APU_ADDRESS* PhysicalAddress,
    UINT32 SizeInBytes,
    UINT32 AlignmentInBytes,
    UINT32 Flags);
```

Suballocate cached or noncached memory from the APU heap. Return both views of the same allocation, honor power-of-two alignment, and clear outputs on failure. **Documented.**

### [ ] `ApuCreateHeap`

```cpp
HRESULT WINAPI ApuCreateHeap(
    UINT32 CachedSizeInBytes,
    UINT32 NonCachedSizeInBytes);
```

Create the sole process-wide cached/noncached APU heap. It persists until process exit; subsequent calls return `APU_E_HEAP_ALREADY_ALLOCATED`. Calling another ACP/APU API first must lazily create the documented default heap. **Documented.**

### [ ] `ApuFree`

```cpp
void WINAPI ApuFree(void* VirtualAddress);
```

Return an APU allocation to the correct cached/noncached free list and invalidate its APU address mapping. **Documented.**

### [ ] `ApuIsVirtualAddressValid`

```cpp
bool WINAPI ApuIsVirtualAddressValid(
    const void* VirtualAddress,
    UINT32 PhysicalAlignmentInBytes);
```

Return true only for a live `ApuAlloc` address whose associated APU address satisfies the requested alignment. Interior-pointer acceptance must follow the official header/runtime contract before implementation is frozen. **Documented.**

### [ ] `ApuMapApuAddress`

```cpp
void* WINAPI ApuMapApuAddress(APU_ADDRESS ApuPhysicalAddress);
```

Translate a live 32-bit APU address back to its CPU virtual address; return `nullptr` for invalid or freed addresses. **Documented.**

### [ ] `ApuMapVirtualAddress`

```cpp
APU_ADDRESS WINAPI ApuMapVirtualAddress(const void* VirtualAddress);
```

Translate a live APU allocation's CPU address to its 32-bit coprocessor address; return zero when invalid. **Documented.**

---

## `xapu.dll` — 5 exports

`XApuHandle` represents a client session. Commands are asynchronous: enqueueing must not claim completion, result count must reflect ready results, and disconnect must detect pending work.

### [ ] `XApuConnect`

```cpp
HRESULT WINAPI XApuConnect(
    XApuConnectInputParameters* Parameters,
    XApuConnectOutputParameters* Data,
    XApuHandle* Handle);
```

Validate process type and queue limits, allocate the shared command-data block, optionally create the result-ready event, and return a live session handle. Clear all outputs on failure. **Documented.**

### [ ] `XApuDequeueResult`

```cpp
HRESULT WINAPI XApuDequeueResult(
    XApuHandle Handle,
    XApuResult* CommandResult);
```

Pop one completed result. Return `XAPU_E_PENDING_RESULTS` when none is ready; do not fabricate successful output. **Documented.**

### [ ] `XApuDisconnect`

```cpp
HRESULT WINAPI XApuDisconnect(XApuHandle Handle);
```

Destroy the session, its data allocation, and its event. Return `XAPU_E_PENDING_RESULTS` if commands remain, while still applying the documented cleanup behavior. **Documented.**

### [ ] `XApuEnqueueCommand`

```cpp
HRESULT WINAPI XApuEnqueueCommand(
    XApuHandle Handle,
    XApuCommandId* Command,
    uint32_t* TotalCommandCount);
```

Copy/validate the command, enqueue it, and optionally return the monotonically increasing submitted-command count. Return queue-full or device-fatal errors without consuming the command. **Documented.**

### [ ] `XApuGetResultCount`

```cpp
uint32_t WINAPI XApuGetResultCount(
    XApuHandle Handle,
    uint32_t* TotalResultCount);
```

Return the number of currently ready results and optionally the monotonically increasing total completed-result count. **Documented.**

---

## `chad.dll` — 5 exports

XDSP uses caller-owned aligned buffers plus client and stream handles. The implementation must retain per-stream command sequence, completion status, and failure state.

### [ ] `XDspActivate`

```cpp
HRESULT WINAPI XDspActivate(
    XDspClientHandle ClientHandle,
    XDspActivationParameters* Parameters,
    XDspStatus** Status,
    XDspStreamHandle* StreamHandle);
```

Acquire a stream resource, validate process-specific parameters, and return both a persistent status block and stream handle. Never report success with either output unset. **Documented.**

### [ ] `XDspConnectWithMaximumStreamLimit`

```cpp
HRESULT WINAPI XDspConnectWithMaximumStreamLimit(
    float* BaseBuffer,
    uint32_t BaseBufferLength,
    uint32_t AggregateImpulseResponseInSeconds,
    uint32_t MaximumStreamCount,
    uint32_t MaximumChannelCount,
    XDspClientHandle* Handle);
```

Register the caller's 16-byte-aligned shared buffer, reserve client capacity, and enforce the global client/impulse-response limits. The buffer remains caller-owned until disconnect. **Documented.**

### [ ] `XDspDeactivate`

```cpp
HRESULT WINAPI XDspDeactivate(XDspStreamHandle Handle);
```

Release a stream only after submitted work completes; otherwise return `XDSP_E_PENDING_RESULTS`. **Documented.**

### [ ] `XDspDisconnect`

```cpp
HRESULT WINAPI XDspDisconnect(XDspClientHandle Handle);
```

Release the client only after all streams are deactivated; otherwise return `XDSP_E_NOT_ALL_HANDLES_DEACTIVATED`. **Documented.**

### [ ] `XDspSubmitCommand`

```cpp
HRESULT WINAPI XDspSubmitCommand(
    XDspStreamHandle Handle,
    XDspCommand* Command,
    uint32_t* CommandSequence);
```

Validate and enqueue one command, return its monotonically increasing sequence, and update the persistent `XDspStatus` as work completes. Return queue-full/device-fatal errors without claiming completion. **Documented.**

---

## Sources and implementation references

Primary documentation:

- [XMem physical mapping](https://docs.xbox.com/reference/system/xmem/functions/xmemmapphysicalpages.md), [scatter mapping](https://docs.xbox.com/reference/system/xmem/functions/xmemmapphysicalpagesscatter.md), [working-set statistics](https://docs.xbox.com/reference/system/xmem/functions/xmemgetworkingsetstatistics.md), and [tools memory](https://docs.xbox.com/reference/system/xmem/functions/xmemistoolsmemavailable.md).
- [APU API](https://docs.xbox.com/reference/audio/apu/apu_members.md) and [ACP HAL API](https://docs.xbox.com/reference/audio/acphal/acphal_members.md).
- [XAPU API](https://docs.xbox.com/reference/audio/xapu/xapu_members.md) and [XDSP API](https://docs.xbox.com/reference/audio/xdspaudio/xdspaudio_members.md).
- [DStorageGetFactory](https://docs.xbox.com/reference/system/dstorage/functions/dstoragegetfactory.md).

Public corroboration used where Microsoft pages are restricted or absent:

- [VIctory `xmem` implementation](https://github.com/Marvisak/VIctory/blob/ffe310eb2098e52169f4c44db5ffdabf43a72622/crates/victory-hle/src/modules/xmem.rs): placeholder-backed reservations, 64 KiB physical pages, signatures, and transfer/statistics behavior.
- [VIctory `xg_xs` implementation](https://github.com/Marvisak/VIctory/blob/main/crates/victory-hle/src/modules/xg.rs): exported signatures, texture-layout computer, swizzle selection, and vtable surface.
- [xcl D3D12.X implementation](https://github.com/ivall/xcl/blob/main/crates/xcl/src/hle/d3d12x/mod.rs): device/configuration ABI and host D3D12 forwarding.
- [Retail executable XMem call-shape research](https://github.com/Cappi1998/LONDBackup/blob/main/docs/xmem_exe_calls.md) and [audio/XG/DirectStorage call-shape research](https://github.com/Cappi1998/LONDBackup/blob/main/docs/audio_hal_exe_calls.md).

## Completion rule

A box is complete only when the export has the documented ABI, owns/validates its state correctly, returns failure instead of a successful no-op for unsupported behavior, and has a focused test covering success, invalid arguments, lifetime, and the title-observed call shape. Implementing only the named export while omitting the object vtable it returns does not complete the item.
