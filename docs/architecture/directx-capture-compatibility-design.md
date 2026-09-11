# Windows DirectX Capture Compatibility Design

Date: 2026-03-20
Status: design-ready
Scope: Host capture pipeline for desktop controller sessions.

## Goals
- Render remote image for common DirectX game windows without persistent black frames.
- Maintain low-latency capture path under game workloads.
- Provide deterministic fallback between capture backends with telemetry.

## Workload Profile
- Windowed DirectX game.
- Borderless-fullscreen DirectX game.
- Mixed desktop and game overlays.

## Capture Backend Strategy
Primary order on Windows:
1. Desktop Duplication API (DDA)
- First choice for broad compatibility and low overhead.

2. Windows Graphics Capture (WGC)
- Fallback when DDA cannot provide stable frames for target window mode.

3. Optional vendor or advanced path (future)
- Keep out of MVP unless baseline paths fail acceptance.

## Backend Selection Heuristics
- Start with DDA for desktop session.
- Detect repeated black/invalid frame patterns and frame stagnation.
- Switch to WGC when DDA health thresholds are violated.
- Re-evaluate backend periodically to avoid lock-in to degraded path.

## Health Signals
- `frame_present_delta_ms`
- `black_frame_ratio`
- `stale_frame_count`
- `backend_switch_count`
- `capture_to_encode_latency_ms`

## Frame Pipeline Requirements
- Zero-copy or minimal-copy transfer from capture to encoder where practical.
- Maintain monotonic timestamps for decode/render stability.
- Separate capture thread and encode thread with bounded queue.
- Backpressure policy must prefer freshness over deep buffering.

## DirectX Window Handling
- Detect target window mode transitions (windowed to borderless and reverse).
- Preserve capture continuity across alt-tab and focus changes.
- Handle resize and resolution changes without pipeline restart when possible.

## Failure Modes and Fallback
- Persistent black frames:
  - Trigger backend failover.
  - Emit diagnostic event with GPU/driver metadata.
- Device lost or reset:
  - Reinitialize capture device with bounded retry.
- Encoder overload:
  - Reduce frame rate or bitrate by policy before dropping connection.

## Test Plan Mapping
- M04-T04: DirectX compatibility implementation and backend telemetry.
- M05 integration: decode/render stability during backend switch.
- M10 integration: metrics and diagnostics export for backend decisions.

## Acceptance Criteria
- Windowed and borderless DirectX game windows are visible remotely in baseline environment.
- No persistent black screen beyond configured recovery threshold.
- Backend failover completes within target recovery window.
- Latency and FPS remain within MVP quality targets during sustained run.
