# Capture recovery and Host cursor

Updated: 2026-09-15

Implementation task: [M01-T03](../runtime/MODULE_KANBAN.md). Release qualification remains in [PROJECT_STATE](../runtime/PROJECT_STATE.md).

## Recovery boundary

`WindowsCaptureSession` owns backend lifetime, typed failure stage/category/HRESULT, desktop access checks and a monotonically increasing capture generation. `CaptureRecoveryPolicy` permits one DDA rebuild per incident. Only a real captured frame ends that incident; a timeout, static image or black-frame ratio does not trigger recovery or prove recovery.

`ACCESS_LOST` and device loss release the failed backend before reconstruction. Device reconstruction includes the D3D11 device. Other acquisition failures retain the configured consecutive-failure threshold. If rebuilding fails on an explicitly ordinary, connected, unrestricted desktop, selection proceeds DDA → WGC → GDI. Stable WGC does not periodically switch back to DDA.

Unknown, secure or disconnected desktops pause capture. Every backend candidate repeats the permission check. Once every eligible backend fails, no backend is retried until the observed desktop/display context changes or the user requests **重试画面**. The existing worker coordinator provides retry/stop wakeups and a bounded desktop-state probe; capture APIs are not called in the paused state.

`CaptureStreamGate` rejects frames from an earlier generation, clears retained video/pacer dependencies and requires a fresh keyframe. Capture loss releases pressed input and pauses Host input even for an older Controller. New peers report actual presentation of the fresh generation before recovery completes. The Controller displays **画面已暂停** and requires a new **Start Control** action after recovery. A delayed active-control status cannot re-arm it.

Capture denial before the first frame also opens the Controller workspace so that the pause message and **重试画面** are reachable. Opening this recovery UI does not count as frame presentation or enable input.

When capture reconstruction changes the D3D11 device, the encoder reopens its native frame pool on that device and requests an IDR. Texture submission rejects a cross-device copy instead of silently accepting an invalid copy. The existing scaler also recreates device-owned resources when its source device changes.

## Cursor ownership

The Controller retains the system default pointer. No local pointer replacement, hiding, prediction or separate cursor transport is introduced. The Host's real pointer is included in encoded video, so two pointers can be visible.

- DDA caches pointer visibility, top-left output position and shape. A visible separate pointer is composited once; an already embedded pointer is not composited again. Color, monochrome AND/XOR and masked-color shapes share the same CPU/GPU interpretation.
- DDA's GPU path composites on the existing owned frame texture using cursor-sized resources. Shape uploads occur on shape changes. It adds no full-frame CPU readback.
- WGC explicitly enables system pointer capture where that interface is available and uses no extra overlay.
- GDI uses the real system cursor and hotspot against the selected display origin. It does not substitute primary-monitor pixels if the selected monitor cannot be captured.
- Cursor composition precedes the existing region-crop and scaling path. DDA rotation maps upright output coordinates to the duplication surface.

## Optional Control v1 capability

Hello/capabilities advertise `capture_status_version=1`. Existing source-activity messages optionally carry capture state (`1=capturing`, `2=recovering`, `3=paused`), generation and first fresh keyframe ID. Existing keyframe requests can carry an explicit capture retry. Unsupported/missing capability preserves legacy wire behavior; unknown optional fields are tolerated. Host permission/input checks never depend on the peer implementing the capability.

Old Host/new Controller and new Host/old Controller must be tested independently. An old Host does not acquire the new cursor behavior merely because its Controller was upgraded. Same-version testing does not replace mixed-version, rolling-upgrade and reconnect acceptance.

## Local evidence and acceptance

Normal runtime diagnostics contain numeric capture state, generation, stage, category, raw HRESULT and rebuild count. Detailed desktop names, session/token state and display identity are stored separately in `capture-evidence/capture-failure-*.jsonl` beside the local process logs. That file is not part of Control log export. Each recorder retains at most 64 distinct failure observations; repeated identical observations and timeouts do not write files.

Focused tests exercise the actual session recovery loop through an internal backend/desktop dependency seam. Production flags and remote messages cannot install those hooks. Hardware D3D11 tests compare composed pixels with the CPU implementation. Physical Host/Controller trials, cursor visual checks, native-resolution performance measurements and mixed-version results remain separate acceptance evidence.
