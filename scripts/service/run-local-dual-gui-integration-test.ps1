param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [ValidateRange(30, 600)]
    [int]$RunSeconds = 300,

    [ValidateRange(5, 600)]
    [int]$HostLeadSeconds = 10,

    [ValidateRange(5, 120)]
    [int]$StableSeconds = 15,

    [string]$RuntimeExe = '',

    [string]$SessionCode = '',

    [string]$NetworkBindAddress = '',

    [ValidateSet('dht', 'file')]
    [string]$SignalTransport = 'dht',

    [string]$ReportRoot = 'build\reports',

    [switch]$SkipBuild,

    [switch]$DropOneMediaFragment,

    [ValidateSet('fresh', 'delayed', 'expired', 'stale-revision')]
    [string]$InjectedFeedbackClass = 'fresh',

    [ValidateSet('none', 'media', 'control')]
    [string]$ForceRequiredChannelClose = 'none',

    [switch]$ForceAgentChannelCloseAfterEvent,

    [switch]$AllowRemoteInput,

    [string]$AgentProjectRoot = '',

    [string]$ControllerAgentProjectRoot = '',

    [switch]$AgentFixtureProvider,

    [switch]$NativeSizeBaseline,

    [ValidateRange(0, 5)]
    [int]$HostRestartCount = 0,

    [ValidateRange(30, 180)]
    [int]$HostRestartTimeoutSeconds = 90,

    [ValidateRange(30, 300)]
    [int]$HostRestartDiscoveryTimeoutSeconds = 180,

    [switch]$KeepProcesses
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'cross-lan-integration-config.ps1')

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Get-RunId {
    return Get-Date -Format 'yyyyMMdd_HHmmss_fff'
}

function New-LocalSessionCode {
    return [guid]::NewGuid().ToString('N').Substring(0, 8).ToUpperInvariant()
}

function Test-AvailableDhtPort {
    param([ValidateRange(1, 65535)][int]$Port)

    $sockets = [System.Collections.Generic.List[System.Net.Sockets.Socket]]::new()
    try {
        foreach ($address in @(
            [System.Net.IPAddress]::Loopback,
            [System.Net.IPAddress]::IPv6Loopback
        )) {
            foreach ($transport in @('tcp', 'udp')) {
                $socketType = if ($transport -eq 'tcp') {
                    [System.Net.Sockets.SocketType]::Stream
                } else {
                    [System.Net.Sockets.SocketType]::Dgram
                }
                $protocolType = if ($transport -eq 'tcp') {
                    [System.Net.Sockets.ProtocolType]::Tcp
                } else {
                    [System.Net.Sockets.ProtocolType]::Udp
                }
                $socket = [System.Net.Sockets.Socket]::new(
                    $address.AddressFamily,
                    $socketType,
                    $protocolType)
                $sockets.Add($socket)
                $socket.ExclusiveAddressUse = $true
                if ($address.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetworkV6) {
                    $socket.DualMode = $false
                }
                $socket.Bind([System.Net.IPEndPoint]::new($address, $Port))
                if ($transport -eq 'tcp') {
                    $socket.Listen(1)
                }
            }
        }
        return $true
    } catch [System.Net.Sockets.SocketException] {
        return $false
    } finally {
        foreach ($socket in $sockets) {
            $socket.Dispose()
        }
    }
}

function Get-AvailableDhtPort {
    for ($attempt = 0; $attempt -lt 64; ++$attempt) {
        # TCP port-zero allocation may repeatedly walk a UDP-excluded range.
        # Sample independent candidates; the existing four-bind probe remains
        # authoritative and closes every temporary socket before returning.
        $candidate = Get-Random -Minimum 49152 -Maximum 65536
        if ($candidate -in @($script:HostIceUdpPort, $script:ControllerIceUdpPort)) {
            continue
        }
        if (Test-AvailableDhtPort -Port $candidate) {
            return $candidate
        }
    }

    throw 'Unable to reserve a DHT port that accepts IPv4/IPv6 TCP and UDP binds.'
}

function Get-RedactedSessionCode {
    param([string]$Code)
    return ($Code.Substring(0, 4) + '****')
}

function Get-IntegrationArgumentList {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('host', 'controller')]
        [string]$Role,

        [Parameter(Mandatory = $true)]
        [string]$RoleDirectory,

        [Parameter(Mandatory = $true)]
        [string]$SignalDirectory,

        [Parameter(Mandatory = $true)]
        [string]$ControlName,

        [Parameter(Mandatory = $true)]
        [string]$CurrentRunId
    )

    $arguments = @(
        '--gui-auto-start',
        '--gui-role', $Role,
        '--session-code', $SessionCode,
        '--signal-transport', $SignalTransport,
        '--signal-timeout-seconds', '0',
        '--run-seconds', '0',
        '--signal-dir', $SignalDirectory,
        '--stream-smoke',
        '--enable-ice-tcp',
        '--ice-udp-port', $(if ($Role -eq 'host') {
            [string]$script:HostIceUdpPort
        } else {
            [string]$script:ControllerIceUdpPort
        }),
        '--enable-port-mapping',
        '--dht-listen-port', $(if ($Role -eq 'host') {
            [string]$script:HostDhtListenPort
        } else {
            [string]$script:ControllerDhtListenPort
        }),
        '--log-dir', $RoleDirectory,
        '--coordination-journal-path', (Join-Path $RoleDirectory 'coordination-v1.jsonl'),
        '--enable-agent-control',
        '--agent-control-name', ("{0}.Agent" -f $ControlName),
        '--run-id', $CurrentRunId,
        '--enable-debug-control',
        '--debug-control-name', $ControlName
    )

    foreach ($server in $script:CrossLanIntegrationIceServers) {
        $arguments += @('--ice-server', $server)
    }

    if (-not [string]::IsNullOrWhiteSpace($NetworkBindAddress)) {
        $arguments += @('--network-bind-address', $NetworkBindAddress)
    }

    if ($Role -eq 'host') {
        $arguments += @('--gui-persist-host-wait', '--stream-require-capture')
        if ($AllowRemoteInput) {
            $arguments += '--allow-remote-input'
        }
        if (-not [string]::IsNullOrWhiteSpace($AgentProjectRoot)) {
            $resolvedAgentProject = (Resolve-Path -LiteralPath $AgentProjectRoot -ErrorAction Stop).Path
            $arguments += @(
                '--allow-remote-agent',
                '--agent-project-root', $resolvedAgentProject
            )
        }
    } elseif ($DropOneMediaFragment) {
        $arguments += @(
            '--stream-qa-drop-one-media-fragment',
            '--stream-qa-incomplete-feedback-class', $InjectedFeedbackClass
        )
    }
    if ($Role -eq 'controller' -and $ForceRequiredChannelClose -ne 'none') {
        $arguments += @(
            '--stream-qa-force-required-channel-close', $ForceRequiredChannelClose
        )
    }
    if ($Role -eq 'controller' -and $ForceAgentChannelCloseAfterEvent) {
        if ($Configuration -ne 'Debug') {
            throw 'ForceAgentChannelCloseAfterEvent is Debug-only.'
        }
        $arguments += '--agent-qa-force-channel-close-after-event'
    }

    if ($Role -eq 'controller' -and -not [string]::IsNullOrWhiteSpace($ControllerAgentProjectRoot)) {
        $resolvedControllerProject = (Resolve-Path -LiteralPath $ControllerAgentProjectRoot -ErrorAction Stop).Path
        $arguments += @('--allow-remote-agent', '--agent-project-root', $resolvedControllerProject)
    }
    if ($AgentFixtureProvider) {
        if ($Configuration -ne 'Debug') { throw 'AgentFixtureProvider is Debug-only.' }
        $arguments += '--agent-qa-fixture-provider'
    }
    if ($NativeSizeBaseline -and $Role -eq 'host') { $arguments += '--stream-qa-native-size' }
    return $arguments
}

function Get-RoleLogText {
    param([string]$RoleDirectory)

    $paths = @(
        Get-ChildItem -LiteralPath $RoleDirectory -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -eq '.log' } |
            Select-Object -ExpandProperty FullName
    )
    $chunks = foreach ($path in $paths) {
        Get-Content -LiteralPath $path -Raw -ErrorAction SilentlyContinue
    }
    return ($chunks -join "`n")
}

function Get-LastMatchingLine {
    param(
        [string]$Text,
        [string]$Pattern
    )

    return @(
        $Text -split "`r?`n" |
            Where-Object { $_ -match $Pattern }
    ) | Select-Object -Last 1
}

function ConvertFrom-KeyValueLine {
    param([string]$Line)

    $fields = @{}
    foreach ($match in [regex]::Matches([string]$Line, '([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)')) {
        $fields[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $fields
}

function Get-IntegerField {
    param(
        [hashtable]$Fields,
        [string]$Name,
        [int]$Default = -1
    )

    if (-not $Fields.ContainsKey($Name)) {
        return $Default
    }
    $value = 0
    if ([int]::TryParse([string]$Fields[$Name], [ref]$value)) {
        return $value
    }
    return $Default
}

function Get-DoubleField {
    param(
        [hashtable]$Fields,
        [string]$Name,
        [double]$Default = -1
    )

    if (-not $Fields.ContainsKey($Name)) {
        return $Default
    }
    $value = 0.0
    if ([double]::TryParse(
            [string]$Fields[$Name],
            [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$value)) {
        return $value
    }
    return $Default
}

function Test-LogPassCriteria {
    param(
        [string]$HostText,
        [string]$ControllerText,
        [bool]$RequireInjectedLoss = $false,
        [bool]$RequireReconnectRecovery = $false,
        [ValidateSet('fresh', 'delayed', 'expired', 'stale-revision')]
        [string]$ExpectedFeedbackClass = 'fresh'
    )

    $hostStatsLine = Get-LastMatchingLine -Text $HostText -Pattern 'Runtime desktop stream stats role=host '
    $controllerStatsLine = Get-LastMatchingLine -Text $ControllerText -Pattern 'Runtime desktop stream stats role=controller '
    $hostRatesLine = Get-LastMatchingLine -Text $HostText -Pattern 'Runtime desktop stream rates role=host '
    $controllerRatesLine = Get-LastMatchingLine -Text $ControllerText -Pattern 'Runtime desktop stream rates role=controller '
    $guiStatsLine = Get-LastMatchingLine -Text $ControllerText -Pattern 'GUI direct frame stats '
    $hostAdaptationLine = Get-LastMatchingLine -Text $HostText -Pattern 'Runtime stream adaptation stats role=host '
    $hostDiagnosticsLine = Get-LastMatchingLine -Text $HostText -Pattern 'Runtime host stream diagnostics role=host '
    $hostInputLine = Get-LastMatchingLine -Text $HostText -Pattern 'Runtime remote input stats role=host '
    $hostTransportFeedbackLine = Get-LastMatchingLine -Text $HostText -Pattern 'Runtime media transport feedback stats role=host '
    $controllerTransportFeedbackLine = Get-LastMatchingLine -Text $ControllerText -Pattern 'Runtime media transport feedback stats role=controller '
    $hostFields = ConvertFrom-KeyValueLine -Line $hostStatsLine
    $controllerFields = ConvertFrom-KeyValueLine -Line $controllerStatsLine
    $hostRatesFields = ConvertFrom-KeyValueLine -Line $hostRatesLine
    $controllerRatesFields = ConvertFrom-KeyValueLine -Line $controllerRatesLine
    $guiFields = ConvertFrom-KeyValueLine -Line $guiStatsLine
    $hostAdaptationFields = ConvertFrom-KeyValueLine -Line $hostAdaptationLine
    $hostDiagnosticsFields = ConvertFrom-KeyValueLine -Line $hostDiagnosticsLine
    $hostInputFields = ConvertFrom-KeyValueLine -Line $hostInputLine
    $hostTransportFeedbackFields = ConvertFrom-KeyValueLine -Line $hostTransportFeedbackLine
    $controllerTransportFeedbackFields = ConvertFrom-KeyValueLine -Line $controllerTransportFeedbackLine

    $checks = [ordered]@{
        host_remote_description = $HostText -match 'Runtime remote description applied role=host'
        controller_remote_description = $ControllerText -match 'Runtime remote description applied role=controller'
        host_connected = $HostText -match 'connected=true'
        controller_connected = $ControllerText -match 'connected=true'
        host_channel_open = [string]$hostFields['channel_open'] -eq 'true'
        controller_channel_open = [string]$controllerFields['channel_open'] -eq 'true'
        host_no_startup_stall = [string]$hostRatesFields['health'] -notmatch '_startup_stalled$'
        controller_no_startup_stall = [string]$controllerRatesFields['health'] -notmatch '_startup_stalled$'
        host_real_capture = (Get-IntegerField -Fields $hostFields -Name 'captured') -gt 0
        host_no_synthetic = (Get-IntegerField -Fields $hostFields -Name 'synthetic') -eq 0
        host_capture_failures = (Get-IntegerField -Fields $hostFields -Name 'capture_failures') -eq 0
        host_encoded = (Get-IntegerField -Fields $hostFields -Name 'encoded') -gt 0
        host_encode_failures = (Get-IntegerField -Fields $hostFields -Name 'encode_failures') -eq 0
        host_transmitted = (Get-IntegerField -Fields $hostFields -Name 'transmitted') -gt 0
        host_transmit_failures = (Get-IntegerField -Fields $hostFields -Name 'transmit_failures') -eq 0
        controller_transport_feedback_sent =
            (Get-IntegerField -Fields $controllerTransportFeedbackFields -Name 'feedback_sent_total') -gt 0
        host_transport_feedback_received =
            (Get-IntegerField -Fields $hostTransportFeedbackFields -Name 'feedback_received_total') -gt 0
        host_transport_acked_bitrate =
            (Get-IntegerField -Fields $hostTransportFeedbackFields -Name 'acked_bitrate_kbps') -gt 0
        host_transport_feedback_valid =
            (Get-IntegerField -Fields $hostTransportFeedbackFields -Name 'feedback_invalid_total') -eq 0
        host_pacer_depth_bounded =
            (Get-IntegerField -Fields $hostTransportFeedbackFields -Name 'pacer_active_depth') -le 1 -and
            (Get-IntegerField -Fields $hostTransportFeedbackFields -Name 'pacer_pending_depth') -le 1
        host_pacer_recovered =
            [string]$hostTransportFeedbackFields['pacer_keyframe_required'] -eq 'false'
        controller_received = (Get-IntegerField -Fields $controllerFields -Name 'received') -gt 0
        controller_media_fragments = (Get-IntegerField -Fields $controllerFields -Name 'media_fragments_received') -gt 0
        controller_frames_reassembled = (Get-IntegerField -Fields $controllerFields -Name 'encoded_frames_reassembled') -gt 0
        controller_direct_pipe_written = (Get-IntegerField -Fields $controllerFields -Name 'direct_pipe_written') -gt 0
        controller_runtime_does_not_claim_gui_decode = (Get-IntegerField -Fields $controllerFields -Name 'decoded') -eq 0
        controller_decode_failures = (Get-IntegerField -Fields $controllerFields -Name 'decode_failures') -eq 0
        controller_runtime_does_not_claim_gui_render = (Get-IntegerField -Fields $controllerFields -Name 'rendered') -eq 0
        controller_render_failures = (Get-IntegerField -Fields $controllerFields -Name 'render_failures') -eq 0
        # A static desktop legitimately has zero FPS in the latest five-second
        # window. Require cumulative presentation evidence so a still-visible
        # last frame remains a valid GUI playback result.
        gui_playback = (Get-IntegerField -Fields $guiFields -Name 'presented_total') -gt 0
        gui_decode_success = (Get-IntegerField -Fields $guiFields -Name 'gui_decode_success_total') -gt 0
        gui_decode_failures = (Get-IntegerField -Fields $guiFields -Name 'decode_failures') -eq 0
        gui_present_failures = (Get-IntegerField -Fields $guiFields -Name 'present_failures') -eq 0
    }

    if ($RequireInjectedLoss) {
        $qaLossFrameId = Get-IntegerField -Fields $controllerFields -Name 'qa_media_loss_frame_id' -Default 0
        $qaRecoveryKeyframeId = Get-IntegerField -Fields $controllerFields -Name 'qa_media_recovery_keyframe_id' -Default 0
        $checks['qa_fragment_dropped'] =
            (Get-IntegerField -Fields $controllerFields -Name 'qa_media_fragment_drops' -Default 0) -eq 1
        $checks['qa_reassembly_detected'] =
            (Get-IntegerField -Fields $controllerFields -Name 'reassembly_failures' -Default 0) -gt 0 -and
            (Get-IntegerField -Fields $controllerFields -Name 'incomplete_frames_dropped' -Default 0) -gt 0
        $checks['qa_feedback_sent'] =
            (Get-IntegerField -Fields $controllerFields -Name 'incomplete_frame_feedback_sent_total' -Default 0) -gt 0
        $checks['qa_feedback_received'] =
            (Get-IntegerField -Fields $hostFields -Name 'incomplete_frame_feedback_received_total' -Default 0) -gt 0
        $freshTotal = Get-IntegerField -Fields $hostAdaptationFields -Name 'feedback_fresh_total' -Default 0
        $delayedTotal = Get-IntegerField -Fields $hostAdaptationFields -Name 'feedback_delayed_total' -Default 0
        $expiredTotal = Get-IntegerField -Fields $hostAdaptationFields -Name 'feedback_expired_total' -Default 0
        $staleTotal = Get-IntegerField -Fields $hostAdaptationFields -Name 'feedback_stale_revision_total' -Default 0
        $unknownTotal = Get-IntegerField -Fields $hostAdaptationFields -Name 'feedback_unknown_frame_total' -Default 0
        $immediateRateChanges = Get-IntegerField -Fields $hostAdaptationFields -Name 'feedback_immediate_rate_change_total' -Default 0
        $delayedQueued = Get-IntegerField -Fields $hostAdaptationFields -Name 'feedback_delayed_queued_total' -Default 0
        $ignoredTotal = Get-IntegerField -Fields $hostAdaptationFields -Name 'feedback_ignored_total' -Default 0
        switch ($ExpectedFeedbackClass) {
            'fresh' {
                $checks['qa_feedback_fresh'] = $freshTotal -gt 0 -and
                    $delayedTotal -eq 0 -and $expiredTotal -eq 0 -and
                    $staleTotal -eq 0 -and $unknownTotal -eq 0
                $checks['qa_feedback_does_not_duplicate_transport_backoff'] =
                    $immediateRateChanges -eq 0
            }
            'delayed' {
                $checks['qa_feedback_delayed'] = $delayedTotal -gt 0 -and
                    $freshTotal -eq 0 -and $expiredTotal -eq 0 -and
                    $staleTotal -eq 0 -and $unknownTotal -eq 0
                $checks['qa_feedback_queued_not_immediate'] =
                    $delayedQueued -eq 0 -and $immediateRateChanges -eq 0 -and $ignoredTotal -eq 0
            }
            'expired' {
                $checks['qa_feedback_expired'] = $expiredTotal -gt 0 -and
                    $freshTotal -eq 0 -and $delayedTotal -eq 0 -and
                    $staleTotal -eq 0 -and $unknownTotal -eq 0
                $checks['qa_feedback_expired_ignored'] =
                    $ignoredTotal -gt 0 -and $immediateRateChanges -eq 0 -and $delayedQueued -eq 0
            }
            'stale-revision' {
                $checks['qa_feedback_stale_revision'] = $staleTotal -gt 0 -and
                    $freshTotal -eq 0 -and $delayedTotal -eq 0 -and
                    $expiredTotal -eq 0 -and $unknownTotal -eq 0
                $checks['qa_feedback_stale_ignored'] =
                    $ignoredTotal -gt 0 -and $immediateRateChanges -eq 0 -and $delayedQueued -eq 0
            }
        }
        $checks['qa_keyframe_requested'] =
            (Get-IntegerField -Fields $hostDiagnosticsFields -Name 'keyframe_requests_received_total' -Default 0) -gt 0
        $checks['qa_transport_gap_detected'] =
            (Get-IntegerField -Fields $hostTransportFeedbackFields `
                -Name 'transport_lost_packets_total' -Default 0) -gt 0
        $checks['qa_keyframe_recovered'] =
            $qaLossFrameId -gt 0 -and $qaRecoveryKeyframeId -gt $qaLossFrameId
    }

    if ($RequireReconnectRecovery) {
        $forcedFrameId = Get-IntegerField -Fields $controllerFields `
            -Name 'qa_forced_channel_close_frame_id' -Default 0
        $postReconnectFrameId = Get-IntegerField -Fields $controllerFields `
            -Name 'qa_post_reconnect_frame_id' -Default 0
        $checks['qa_required_channel_closed_once'] =
            (Get-IntegerField -Fields $controllerFields `
                -Name 'qa_forced_channel_close_total' -Default 0) -eq 1
        $checks['host_recovery_reset'] =
            (Get-IntegerField -Fields $hostFields -Name 'recovery_reset_total' -Default 0) -gt 0
        $checks['controller_recovery_reset'] =
            (Get-IntegerField -Fields $controllerFields -Name 'recovery_reset_total' -Default 0) -gt 0
        $checks['host_required_channels_reopened'] =
            (Get-IntegerField -Fields $hostFields -Name 'required_channels_open_total' -Default 0) -ge 2 -and
            (Get-IntegerField -Fields $hostFields -Name 'recovery_success_total' -Default 0) -gt 0
        $checks['controller_required_channels_reopened'] =
            (Get-IntegerField -Fields $controllerFields -Name 'required_channels_open_total' -Default 0) -ge 2 -and
            (Get-IntegerField -Fields $controllerFields -Name 'recovery_success_total' -Default 0) -gt 0
        $checks['controller_received_post_reconnect_frame'] =
            $forcedFrameId -gt 0 -and $postReconnectFrameId -gt $forcedFrameId
        $checks['controller_delivered_post_reconnect_frame_to_gui'] =
            (Get-IntegerField -Fields $controllerFields `
                -Name 'qa_post_reconnect_direct_pipe_written_total' -Default 0) -gt 0
        $checks['controller_viewport_replayed'] =
            (Get-IntegerField -Fields $hostFields -Name 'viewport_request_total' -Default 0) -ge 2
        $checks['remote_input_not_reactivated'] =
            (Get-IntegerField -Fields $hostInputFields -Name 'state' -Default 2) -ne 2 -and
            (Get-IntegerField -Fields $hostInputFields -Name 'queue_current' -Default -1) -eq 0
        $checks['new_epoch_generated'] =
            $HostText -match 'Runtime rebuilding signaling session role=host .*epoch_generation=1' -and
            $ControllerText -match 'Runtime rebuilding signaling session role=controller .*epoch_generation=1'
    }

    $failed = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | Select-Object -ExpandProperty Key)
    return [pscustomobject]@{
        Passed = $failed.Count -eq 0
        FailedChecks = $failed
        HostStatsLine = $hostStatsLine
        ControllerStatsLine = $controllerStatsLine
        HostRatesLine = $hostRatesLine
        ControllerRatesLine = $controllerRatesLine
        GuiStatsLine = $guiStatsLine
        HostAdaptationLine = $hostAdaptationLine
        HostDiagnosticsLine = $hostDiagnosticsLine
        HostInputLine = $hostInputLine
        HostTransportFeedbackLine = $hostTransportFeedbackLine
        ControllerTransportFeedbackLine = $controllerTransportFeedbackLine
        HostStreamHealth = [string]$hostRatesFields['health']
        ControllerStreamHealth = [string]$controllerRatesFields['health']
        FirstMediaDeadlineMs = Get-IntegerField `
            -Fields $hostRatesFields -Name 'first_media_deadline_ms' -Default 0
        HostPacerDeadlineDrops = Get-IntegerField `
            -Fields $hostTransportFeedbackFields -Name 'pacer_deadline_drops'
        Checks = [pscustomobject]$checks
    }
}

function Initialize-WindowCaptureType {
    if ($null -ne ('RedClawWindowCapture' -as [type])) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class RedClawWindowCapture
{
    public sealed class WindowInfo
    {
        public IntPtr Handle;
        public string Title;
        public int Left;
        public int Top;
        public int Width;
        public int Height;
    }

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int command);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int width, int height, uint flags);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int index);

    [StructLayout(LayoutKind.Sequential)]
    private struct Point { public int X; public int Y; }

    [DllImport("user32.dll")]
    private static extern IntPtr WindowFromPoint(Point point);

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);

    public static IntPtr BeginPhysicalCapture()
    {
        return SetThreadDpiAwarenessContext(new IntPtr(-4));
    }

    public static void EndPhysicalCapture(IntPtr previous)
    {
        if (previous != IntPtr.Zero) SetThreadDpiAwarenessContext(previous);
    }

    public static bool RaiseForCapture(IntPtr handle)
    {
        bool alreadyTopmost = (GetWindowLongPtr(handle, -20).ToInt64() & 8) != 0;
        if (!SetWindowPos(handle, new IntPtr(-1), 0, 0, 0, 0, 0x13))
            throw new InvalidOperationException("Unable to expose the owned Controller window");
        return alreadyTopmost;
    }

    public static void RestoreCaptureOrder(IntPtr handle, bool wasTopmost)
    {
        if (!wasTopmost) SetWindowPos(handle, new IntPtr(-2), 0, 0, 0, 0, 0x13);
    }

    public static bool IsCaptureSurfaceUnobscured(WindowInfo window)
    {
        int[] xs = { window.Left + 16, window.Left + window.Width / 2, window.Left + window.Width - 17 };
        int[] ys = { window.Top + 16, window.Top + window.Height / 2, window.Top + window.Height - 17 };
        foreach (int x in xs) foreach (int y in ys) {
            if (GetAncestor(WindowFromPoint(new Point { X = x, Y = y }), 2) != window.Handle)
                return false;
        }
        return true;
    }

    public static WindowInfo[] FindVisibleWindows(uint processId)
    {
        var result = new List<WindowInfo>();
        EnumWindows(delegate(IntPtr handle, IntPtr ignored) {
            uint owner;
            GetWindowThreadProcessId(handle, out owner);
            if (owner != processId || !IsWindowVisible(handle)) {
                return true;
            }
            Rect rect;
            if (!GetWindowRect(handle, out rect) || rect.Right <= rect.Left || rect.Bottom <= rect.Top) {
                return true;
            }
            var title = new StringBuilder(256);
            GetWindowText(handle, title, title.Capacity);
            result.Add(new WindowInfo {
                Handle = handle,
                Title = title.ToString(),
                Left = rect.Left,
                Top = rect.Top,
                Width = rect.Right - rect.Left,
                Height = rect.Bottom - rect.Top
            });
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }

    public static void Activate(IntPtr handle)
    {
        if (IsIconic(handle)) ShowWindow(handle, 9);
        SetForegroundWindow(handle);
    }
}
'@
}

function Save-ControllerPlaybackScreenshot {
    param(
        [int]$ProcessId,
        [string]$Path
    )

    Initialize-WindowCaptureType
    Add-Type -AssemblyName System.Drawing
    $previousDpi = [RedClawWindowCapture]::BeginPhysicalCapture()
    if ($previousDpi -eq [IntPtr]::Zero) {
        throw 'Unable to establish physical-pixel screenshot coordinates.'
    }
    $raisedHandle = [IntPtr]::Zero
    $wasTopmost = $false
    try {
        $windows = @([RedClawWindowCapture]::FindVisibleWindows([uint32]$ProcessId))
        $target = $windows |
            Where-Object { $_.Title -eq 'RedClaw' -and $_.Width -ge 360 -and $_.Height -ge 240 } |
            Select-Object -First 1
        if ($null -eq $target) {
            throw "Controller playback window was not visible for pid=$ProcessId."
        }

        [RedClawWindowCapture]::Activate($target.Handle)
        # Expose only this test-owned window without synthetic keyboard input or
        # stealing foreground permission. Restore its topmost flag in finally.
        $wasTopmost = [RedClawWindowCapture]::RaiseForCapture($target.Handle)
        $raisedHandle = $target.Handle
        Start-Sleep -Milliseconds 750
        # Restore/activation may change geometry. Never capture the stale rectangle
        # or accept pixels belonging to an obscuring application.
        $targetHandle = $target.Handle
        $target = [RedClawWindowCapture]::FindVisibleWindows([uint32]$ProcessId) |
            Where-Object { $_.Handle -eq $targetHandle } | Select-Object -First 1
        if ($null -eq $target -or -not [RedClawWindowCapture]::IsCaptureSurfaceUnobscured($target)) {
            throw 'Controller playback is obscured; refusing an unrelated screen capture.'
        }
        $bitmap = [System.Drawing.Bitmap]::new($target.Width, $target.Height)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CopyFromScreen(
                $target.Left,
                $target.Top,
                0,
                0,
                [System.Drawing.Size]::new($target.Width, $target.Height),
                [System.Drawing.CopyPixelOperation]::SourceCopy)
            if (-not [RedClawWindowCapture]::IsCaptureSurfaceUnobscured($target)) {
                throw 'Controller became obscured during screenshot; evidence rejected.'
            }
            $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    } finally {
        if ($raisedHandle -ne [IntPtr]::Zero) {
            [RedClawWindowCapture]::RestoreCaptureOrder($raisedHandle, $wasTopmost)
        }
        [RedClawWindowCapture]::EndPhysicalCapture($previousDpi)
    }
}

function Invoke-ControlAction {
    param(
        [string]$Action,
        [string]$ControlName
    )

    [void](Invoke-ControlJson -Action $Action -ControlName $ControlName)
}

function Invoke-ControlJson {
    param(
        [string]$Action,
        [string]$ControlName,
        [ValidateRange(1, 200)]
        [int]$Limit = 100
    )

    $output = @(
        & (Join-Path $PSScriptRoot 'invoke-cross-lan-debug-control.ps1') `
            -Action $Action -ControlName $ControlName -Limit $Limit `
            -TimeoutMs 3000 -Json 2>$null
    )
    if ($LASTEXITCODE -ne 0) {
        throw "Debug control action '$Action' failed with exit code $LASTEXITCODE."
    }
    return (($output -join "`n") | ConvertFrom-Json)
}

function Stop-OwnedProcess {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$ControlName
    )

    if ($null -eq $Process -or $Process.HasExited) {
        return
    }
    $ownedChildProcessIds = @(
        Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Where-Object { $_.ParentProcessId -eq $Process.Id } |
            Select-Object -ExpandProperty ProcessId
    )
    try {
        Invoke-ControlAction -Action 'stop' -ControlName $ControlName
        Start-Sleep -Seconds 1
    } catch {
        Write-Host "[local-dual-gui-test] warning: debug stop failed pid=$($Process.Id)"
    }
    if (-not $Process.HasExited) {
        [void]$Process.CloseMainWindow()
        [void]$Process.WaitForExit(5000)
    }
    if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    }
    foreach ($childProcessId in $ownedChildProcessIds) {
        if (Get-Process -Id $childProcessId -ErrorAction SilentlyContinue) {
            Stop-Process -Id $childProcessId -Force -ErrorAction SilentlyContinue
        }
    }
}

$repoRoot = Get-RepoRoot
Set-Location $repoRoot

if ([string]::IsNullOrWhiteSpace($SessionCode)) {
    $SessionCode = New-LocalSessionCode
}
if ($SessionCode -notmatch '^[A-Za-z0-9]{8}$') {
    throw 'SessionCode must contain exactly 8 letters or digits.'
}
if (-not [string]::IsNullOrWhiteSpace($NetworkBindAddress)) {
    $parsedAddress = $null
    if (-not [System.Net.IPAddress]::TryParse($NetworkBindAddress, [ref]$parsedAddress)) {
        throw 'NetworkBindAddress must be an IPv4 address.'
    }
}
if ($ForceRequiredChannelClose -ne 'none' -and $SignalTransport -ne 'dht') {
    throw 'ForceRequiredChannelClose requires SignalTransport dht.'
}
if ($HostRestartCount -gt 0 -and $SignalTransport -ne 'dht') {
    throw 'HostRestartCount requires SignalTransport dht.'
}

$runId = Get-RunId
$reportRootAbsolute = if ([System.IO.Path]::IsPathRooted($ReportRoot)) {
    $ReportRoot
} else {
    Join-Path $repoRoot $ReportRoot
}
$runDirectory = Join-Path $reportRootAbsolute "local-dual-gui-$runId"
$hostDirectory = Join-Path $runDirectory 'host'
$controllerDirectory = Join-Path $runDirectory 'controller'
$sharedSignalDirectory = Join-Path $runDirectory 'shared-signal'
$hostSignalDirectory = if ($SignalTransport -eq 'file') {
    $sharedSignalDirectory
} else {
    Join-Path $hostDirectory 'signal'
}
$controllerSignalDirectory = if ($SignalTransport -eq 'file') {
    $sharedSignalDirectory
} else {
    Join-Path $controllerDirectory 'signal'
}
New-Item -ItemType Directory -Force -Path `
    $hostDirectory, $controllerDirectory, $hostSignalDirectory, $controllerSignalDirectory | Out-Null

$hostOutLog = Join-Path $hostDirectory 'gui.out.log'
$hostErrLog = Join-Path $hostDirectory 'gui.err.log'
$controllerOutLog = Join-Path $controllerDirectory 'gui.out.log'
$controllerErrLog = Join-Path $controllerDirectory 'gui.err.log'
$screenshotPath = Join-Path $runDirectory 'controller-live-view.png'
$resultJson = Join-Path $runDirectory 'result.json'
$hostControlName = "RedClawDesktop.LocalGui.Host.$runId"
$controllerControlName = "RedClawDesktop.LocalGui.Controller.$runId"
$preexistingProcessIds = @(Get-Process -Name 'redclaw_desktop' -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
$script:HostDhtListenPort = 0
$script:ControllerDhtListenPort = 0
$script:HostIceUdpPort = 55000
$script:ControllerIceUdpPort = 55001
$hostDhtListenPortBaseline = 0

Write-Host "[local-dual-gui-test] run_id=$runId session=$(Get-RedactedSessionCode -Code $SessionCode) signal_transport=$SignalTransport host_ice_udp_port=$($script:HostIceUdpPort) controller_ice_udp_port=$($script:ControllerIceUdpPort)"
Write-Host "[local-dual-gui-test] preexisting_processes=$($preexistingProcessIds -join ',') (left untouched)"

if (-not $SkipBuild) {
    Write-Host "[local-dual-gui-test] building $Configuration via build.ps1 -SkipConfigure"
    & (Join-Path $repoRoot 'build.ps1') -Configuration $Configuration -SkipConfigure
    if ($LASTEXITCODE -ne 0) {
        throw "build.ps1 failed with exit code $LASTEXITCODE"
    }
}

if ([string]::IsNullOrWhiteSpace($RuntimeExe)) {
    $RuntimeExe = Join-Path $repoRoot ("release\{0}\redclaw_desktop.exe" -f $Configuration)
}
$runtimePath = (Resolve-Path -LiteralPath $RuntimeExe).Path
$runtimeHash = (Get-FileHash -LiteralPath $runtimePath -Algorithm SHA256).Hash.ToLowerInvariant()

$hostProc = $null
$controllerProc = $null
$evaluation = $null
$screenshotError = ''
$stableSince = $null
$stableDeadlineDrops = $null
$timedOut = $false
$failureClassification = ''
$remoteLogTransferRequired = $ForceRequiredChannelClose -ne 'none'
$remoteLogTransferPassed = -not $remoteLogTransferRequired
$remoteLogTransferError = ''
$remoteLogSnapshotPath = Join-Path $controllerDirectory 'remote-host-after-reconnect.log'
$remoteLogSnapshotRequestId = ''
$remoteLogSnapshotLineCount = 0
$hostRestartPassed = $HostRestartCount -eq 0
$hostRestartRecords = [System.Collections.Generic.List[object]]::new()
$controllerGuiPidBaseline = 0
$controllerRuntimePidBaseline = 0

try {
    if ($SignalTransport -eq 'dht') {
        $script:HostDhtListenPort = Get-AvailableDhtPort
        $hostDhtListenPortBaseline = $script:HostDhtListenPort
    }
    $hostArgs = Get-IntegrationArgumentList `
        -Role host -RoleDirectory $hostDirectory -SignalDirectory $hostSignalDirectory `
        -ControlName $hostControlName -CurrentRunId $runId
    Write-Host '[local-dual-gui-test] starting Host GUI'
    $hostProc = Start-Process -FilePath $runtimePath -ArgumentList $hostArgs `
        -RedirectStandardOutput $hostOutLog -RedirectStandardError $hostErrLog `
        -PassThru -WindowStyle Normal

    Write-Host "[local-dual-gui-test] waiting ${HostLeadSeconds}s for Host signaling readiness"
    $hostLeadDeadline = [DateTimeOffset]::UtcNow.AddSeconds($HostLeadSeconds)
    while ([DateTimeOffset]::UtcNow -lt $hostLeadDeadline) {
        if ($hostProc.HasExited) {
            throw "Host GUI exited early with code $($hostProc.ExitCode)."
        }
        Start-Sleep -Seconds 1
    }

    if ($SignalTransport -eq 'dht') {
        $script:ControllerDhtListenPort = Get-AvailableDhtPort
    }
    $controllerArgs = Get-IntegrationArgumentList `
        -Role controller -RoleDirectory $controllerDirectory -SignalDirectory $controllerSignalDirectory `
        -ControlName $controllerControlName -CurrentRunId $runId
    Write-Host '[local-dual-gui-test] starting Controller GUI'
    $controllerProc = Start-Process -FilePath $runtimePath -ArgumentList $controllerArgs `
        -RedirectStandardOutput $controllerOutLog -RedirectStandardError $controllerErrLog `
        -PassThru -WindowStyle Normal

    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($RunSeconds)
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        if ($hostProc.HasExited) {
            throw "Host GUI exited early with code $($hostProc.ExitCode)."
        }
        if ($controllerProc.HasExited) {
            throw "Controller GUI exited early with code $($controllerProc.ExitCode)."
        }

        $hostText = Get-RoleLogText -RoleDirectory $hostDirectory
        $controllerText = Get-RoleLogText -RoleDirectory $controllerDirectory
        $evaluation = Test-LogPassCriteria `
            -HostText $hostText -ControllerText $controllerText `
            -RequireInjectedLoss ([bool]$DropOneMediaFragment) `
            -RequireReconnectRecovery ($ForceRequiredChannelClose -ne 'none') `
            -ExpectedFeedbackClass $InjectedFeedbackClass
        $startupHealth = @(
            $evaluation.HostStreamHealth,
            $evaluation.ControllerStreamHealth
        ) | Where-Object { $_ -match '_startup_stalled$' } | Select-Object -First 1
        if (-not [string]::IsNullOrWhiteSpace($startupHealth)) {
            $failureClassification = $startupHealth
            Write-Host "[local-dual-gui-test] first-media gate failed: $failureClassification"
            break
        }
        if ($evaluation.Passed) {
            if ($null -eq $stableSince) {
                $stableSince = [DateTimeOffset]::UtcNow
                $stableDeadlineDrops = $evaluation.HostPacerDeadlineDrops
                Write-Host "[local-dual-gui-test] stream gate reached; verifying ${StableSeconds}s stability"
            } elseif ($evaluation.HostPacerDeadlineDrops -ne $stableDeadlineDrops) {
                $stableSince = [DateTimeOffset]::UtcNow
                $stableDeadlineDrops = $evaluation.HostPacerDeadlineDrops
                Write-Host '[local-dual-gui-test] pacer deadline drop observed; restarting healthy window'
            }
            if (([DateTimeOffset]::UtcNow - $stableSince).TotalSeconds -ge $StableSeconds) {
                break
            }
        } else {
            $stableSince = $null
            $stableDeadlineDrops = $null
        }
        Start-Sleep -Seconds 1
    }

    if ($null -eq $evaluation -or -not $evaluation.Passed -or $null -eq $stableSince -or
        (([DateTimeOffset]::UtcNow - $stableSince).TotalSeconds -lt $StableSeconds)) {
        if ([string]::IsNullOrWhiteSpace($failureClassification)) {
            $timedOut = $true
            $failureClassification = 'gate_timeout'
        }
    } else {
        if ($HostRestartCount -gt 0) {
            $controllerGuiPidBaseline = $controllerProc.Id
            $controllerStatus = Invoke-ControlJson `
                -Action 'status' -ControlName $controllerControlName
            $controllerRuntimePidBaseline = [int64]$controllerStatus.status.runtime_pid
            if ($controllerRuntimePidBaseline -le 0) {
                throw 'Controller did not expose a stable runtime PID before Host restart testing.'
            }

            $controllerFieldsBeforeRestart = ConvertFrom-KeyValueLine `
                -Line $evaluation.ControllerStatsLine
            $lastControllerDirectPipeWritten = Get-IntegerField `
                -Fields $controllerFieldsBeforeRestart -Name 'direct_pipe_written' -Default 0
            $hostRestartPassed = $true

            for ($restartIndex = 1; $restartIndex -le $HostRestartCount; ++$restartIndex) {
                $oldHostPid = $hostProc.Id
                Write-Host ("[local-dual-gui-test] stopping Host for restart {0}/{1} pid={2}" -f `
                    $restartIndex, $HostRestartCount, $oldHostPid)
                Stop-OwnedProcess -Process $hostProc -ControlName $hostControlName
                if (-not $hostProc.HasExited) {
                    throw "Host GUI pid=$oldHostPid did not exit before restart $restartIndex."
                }

                $restartDirectory = Join-Path $runDirectory ("host-restart-{0}" -f $restartIndex)
                $restartSignalDirectory = Join-Path $restartDirectory 'signal'
                New-Item -ItemType Directory -Force `
                    -Path $restartDirectory, $restartSignalDirectory | Out-Null
                $hostOutLog = Join-Path $restartDirectory 'gui.out.log'
                $hostErrLog = Join-Path $restartDirectory 'gui.err.log'
                $hostControlName = "RedClawDesktop.LocalGui.Host.$runId.restart$restartIndex"
                $portReleaseDeadline = [DateTimeOffset]::UtcNow.AddSeconds(5)
                while (-not (Test-AvailableDhtPort -Port $hostDhtListenPortBaseline) -and
                    [DateTimeOffset]::UtcNow -lt $portReleaseDeadline) {
                    Start-Sleep -Milliseconds 250
                }
                if (-not (Test-AvailableDhtPort -Port $hostDhtListenPortBaseline)) {
                    throw "Host DHT listen port $hostDhtListenPortBaseline was not released after process exit."
                }
                $script:HostDhtListenPort = $hostDhtListenPortBaseline
                $restartRunId = "$runId-host-restart-$restartIndex"
                $hostArgs = Get-IntegrationArgumentList `
                    -Role host -RoleDirectory $restartDirectory `
                    -SignalDirectory $restartSignalDirectory `
                    -ControlName $hostControlName -CurrentRunId $restartRunId
                $hostProc = Start-Process -FilePath $runtimePath -ArgumentList $hostArgs `
                    -RedirectStandardOutput $hostOutLog -RedirectStandardError $hostErrLog `
                    -PassThru -WindowStyle Normal
                Write-Host ("[local-dual-gui-test] started Host restart {0}/{1} old_pid={2} new_pid={3}" -f `
                    $restartIndex, $HostRestartCount, $oldHostPid, $hostProc.Id)

                $restartStartedAt = [DateTimeOffset]::UtcNow
                $restartDiscoveryDeadline = $restartStartedAt.AddSeconds(
                    $HostRestartDiscoveryTimeoutSeconds)
                $restartAdoptedAt = $null
                $restartConvergenceDeadline = $null
                $restartObservationDeadline = $restartDiscoveryDeadline
                $restartStableSince = $null
                $restartRecoveredWithinWindow = $false
                $restartEvaluation = $null
                $restartBlockingFailedChecks = @('evaluation_unavailable')
                $restartDiagnosticFailedChecks = @('evaluation_unavailable')
                $restartAdoptionCount = 0
                $restartControllerDirectPipeWritten = $lastControllerDirectPipeWritten
                $restartControllerRuntimePid = 0
                while ([DateTimeOffset]::UtcNow -lt $restartObservationDeadline) {
                    if ($hostProc.HasExited) {
                        throw "Restarted Host GUI exited early with code $($hostProc.ExitCode)."
                    }
                    if ($controllerProc.HasExited -or
                        $controllerProc.Id -ne $controllerGuiPidBaseline) {
                        throw 'Controller GUI did not remain alive during Host restart testing.'
                    }

                    try {
                        $controllerStatus = Invoke-ControlJson `
                            -Action 'status' -ControlName $controllerControlName
                        $restartControllerRuntimePid = [int64]$controllerStatus.status.runtime_pid
                        $restartAdoptionCount = [int64]$controllerStatus.status.dht.persistent_offer_adopted_host_restart_total
                    } catch {
                        $restartControllerRuntimePid = 0
                        $restartAdoptionCount = 0
                    }
                    $restartHostText = Get-RoleLogText -RoleDirectory $restartDirectory
                    $controllerText = Get-RoleLogText -RoleDirectory $controllerDirectory
                    $restartEvaluation = Test-LogPassCriteria `
                        -HostText $restartHostText -ControllerText $controllerText
                    $restartDiagnosticFailedChecks = @($restartEvaluation.FailedChecks)
                    # An instantaneous ACKed-bitrate estimate can return to zero
                    # after real frames have crossed the recovered session (and
                    # is expected to do so for a static source). Keep it in the
                    # evidence, but do not classify an otherwise healthy Host
                    # restart as a signaling/ICE recovery failure.
                    $restartBlockingFailedChecks = @(
                        $restartDiagnosticFailedChecks | Where-Object {
                            $_ -ne 'host_transport_acked_bitrate'
                        })
                    $restartControllerFields = ConvertFrom-KeyValueLine `
                        -Line $restartEvaluation.ControllerStatsLine
                    $restartControllerDirectPipeWritten = Get-IntegerField `
                        -Fields $restartControllerFields -Name 'direct_pipe_written' -Default 0
                    if ($restartAdoptionCount -ge $restartIndex -and
                        $null -eq $restartAdoptedAt) {
                        $restartAdoptedAt = [DateTimeOffset]::UtcNow
                        $restartConvergenceDeadline = $restartAdoptedAt.AddSeconds(
                            $HostRestartTimeoutSeconds)
                        $restartObservationDeadline = $restartConvergenceDeadline
                        Write-Host ("[local-dual-gui-test] Host restart {0}/{1} instance adopted; starting {2}s convergence window" -f `
                            $restartIndex, $HostRestartCount,
                            $HostRestartTimeoutSeconds)
                    }
                    $restartReady = $restartBlockingFailedChecks.Count -eq 0 -and
                        $restartAdoptionCount -ge $restartIndex -and
                        $restartControllerDirectPipeWritten -gt $lastControllerDirectPipeWritten -and
                        $restartControllerRuntimePid -eq $controllerRuntimePidBaseline
                    if ($restartReady) {
                        if ($null -eq $restartStableSince) {
                            $restartStableSince = [DateTimeOffset]::UtcNow
                            $restartRecoveredWithinWindow =
                                $null -ne $restartConvergenceDeadline -and
                                $restartStableSince -le $restartConvergenceDeadline
                            # Recovery must happen inside the 90-second gate, but
                            # its stability sample must not be truncated merely
                            # because recovery occurred near the deadline.
                            $restartObservationDeadline = $restartStableSince.AddSeconds(
                                $StableSeconds + 2)
                            Write-Host ("[local-dual-gui-test] Host restart {0}/{1} recovered; verifying {2}s stability" -f `
                                $restartIndex, $HostRestartCount, $StableSeconds)
                        } elseif (([DateTimeOffset]::UtcNow - $restartStableSince).TotalSeconds `
                                -ge $StableSeconds) {
                            break
                        }
                    } else {
                        if ($null -ne $restartStableSince -and
                            $null -ne $restartConvergenceDeadline -and
                            [DateTimeOffset]::UtcNow -lt $restartConvergenceDeadline) {
                            # A transient metric (for example the first ACKed
                            # bitrate sample) may disappear while the recovered
                            # session is still warming up. Keep searching for a
                            # complete stable window until the original
                            # convergence deadline instead of ending the gate at
                            # the shortened stability-sample deadline.
                            $restartObservationDeadline = $restartConvergenceDeadline
                            Write-Host ("[local-dual-gui-test] Host restart {0}/{1} stability sample interrupted; continuing convergence window" -f `
                                $restartIndex, $HostRestartCount)
                        }
                        $restartStableSince = $null
                    }
                    Start-Sleep -Seconds 1
                }

                $restartRecovered = $null -ne $restartEvaluation -and
                    $restartBlockingFailedChecks.Count -eq 0 -and
                    $restartRecoveredWithinWindow -and
                    $restartAdoptionCount -ge $restartIndex -and
                    $restartControllerDirectPipeWritten -gt $lastControllerDirectPipeWritten -and
                    $restartControllerRuntimePid -eq $controllerRuntimePidBaseline -and
                    $null -ne $restartStableSince -and
                    (([DateTimeOffset]::UtcNow - $restartStableSince).TotalSeconds `
                        -ge $StableSeconds)
                $hostRestartRecords.Add([pscustomobject][ordered]@{
                    restart_index = $restartIndex
                    old_host_pid = $oldHostPid
                    new_host_pid = $hostProc.Id
                    controller_gui_pid = $controllerProc.Id
                    controller_runtime_pid = $restartControllerRuntimePid
                    discovery_seconds = $(if ($null -eq $restartAdoptedAt) {
                        -1
                    } else {
                        [Math]::Round(($restartAdoptedAt - $restartStartedAt).TotalSeconds, 3)
                    })
                    convergence_seconds = $(if ($null -eq $restartStableSince -or
                        $null -eq $restartAdoptedAt) {
                        -1
                    } else {
                        [Math]::Round(($restartStableSince - $restartAdoptedAt).TotalSeconds, 3)
                    })
                    adoption_count = $restartAdoptionCount
                    direct_pipe_written_before = $lastControllerDirectPipeWritten
                    direct_pipe_written_after = $restartControllerDirectPipeWritten
                    failed_checks = @($restartBlockingFailedChecks)
                    diagnostic_failed_checks = @($restartDiagnosticFailedChecks)
                    recovered = $restartRecovered
                    host_directory = $restartDirectory
                })
                if (-not $restartRecovered) {
                    $hostRestartPassed = $false
                    $failureClassification = 'host_restart_recovery_failed'
                    Write-Host ("[local-dual-gui-test] Host restart {0}/{1} did not recover within {2}s" -f `
                        $restartIndex, $HostRestartCount, $HostRestartTimeoutSeconds)
                    break
                }

                $evaluation = $restartEvaluation
                $lastControllerDirectPipeWritten = $restartControllerDirectPipeWritten
                Write-Host ("[local-dual-gui-test] Host restart {0}/{1} PASS adoption_count={2} direct_pipe_written={3}" -f `
                    $restartIndex, $HostRestartCount, $restartAdoptionCount,
                    $restartControllerDirectPipeWritten)
            }
        }

        if ($remoteLogTransferRequired) {
            try {
                $snapshotResponse = Invoke-ControlJson `
                    -Action 'remote_log_snapshot' -ControlName $controllerControlName
                $remoteLogSnapshotRequestId = [string]$snapshotResponse.result.peer_request_id
                $remoteLogDeadline = [DateTimeOffset]::UtcNow.AddSeconds(15)
                do {
                    Start-Sleep -Milliseconds 250
                    $readResponse = Invoke-ControlJson `
                        -Action 'remote_log_read' -ControlName $controllerControlName -Limit 50
                    $remoteError = [string]$readResponse.result.remote_error
                    if (-not [string]::IsNullOrWhiteSpace($remoteError)) {
                        throw "Remote Host log snapshot failed: $remoteError"
                    }
                    $remoteLines = @($readResponse.result.lines)
                    if ([bool]$readResponse.result.complete -and $remoteLines.Count -gt 0) {
                        $remoteLines | Set-Content -LiteralPath $remoteLogSnapshotPath -Encoding UTF8
                        $remoteLogSnapshotLineCount = $remoteLines.Count
                        $remoteLogTransferPassed = $true
                        break
                    }
                } while ([DateTimeOffset]::UtcNow -lt $remoteLogDeadline)
                if (-not $remoteLogTransferPassed) {
                    throw 'Remote Host log snapshot did not complete within 15 seconds.'
                }
                Write-Host "[local-dual-gui-test] remote Host log completed after reconnect: $remoteLogSnapshotPath"
            } catch {
                $remoteLogTransferError = $_.Exception.Message
                $failureClassification = 'remote_log_after_reconnect_failed'
                Write-Host "[local-dual-gui-test] $remoteLogTransferError"
            }
        }
        try {
            if ($remoteLogTransferPassed) {
                Save-ControllerPlaybackScreenshot -ProcessId $controllerProc.Id -Path $screenshotPath
                Write-Host "[local-dual-gui-test] captured Controller live view: $screenshotPath"
            }
        } catch {
            $screenshotError = $_.Exception.Message
            Write-Host "[local-dual-gui-test] screenshot failed: $screenshotError"
        }
    }
} finally {
    if (-not $KeepProcesses) {
        Stop-OwnedProcess -Process $controllerProc -ControlName $controllerControlName
        Stop-OwnedProcess -Process $hostProc -ControlName $hostControlName
    }
}

$screenshotExists = Test-Path -LiteralPath $screenshotPath
$remoteLogSnapshotExists = Test-Path -LiteralPath $remoteLogSnapshotPath
$overallPassed = $null -ne $evaluation -and $evaluation.Passed -and -not $timedOut `
    -and $remoteLogTransferPassed -and $screenshotExists -and $hostRestartPassed
$result = [ordered]@{
    schema = 'redclaw.local-dual-gui.result.v2'
    generated_at = (Get-Date).ToString('o')
    run_id = $runId
    session_code_redacted = Get-RedactedSessionCode -Code $SessionCode
    runtime_path = $runtimePath
    runtime_sha256 = $runtimeHash
    signal_transport = $SignalTransport
    allow_remote_input = [bool]$AllowRemoteInput
    host_dht_listen_port = $script:HostDhtListenPort
    controller_dht_listen_port = $script:ControllerDhtListenPort
    host_ice_udp_port = $script:HostIceUdpPort
    controller_ice_udp_port = $script:ControllerIceUdpPort
    network_bind_address = $(if ([string]::IsNullOrWhiteSpace($NetworkBindAddress)) { 'auto' } else { $NetworkBindAddress })
    preexisting_process_ids_untouched = $preexistingProcessIds
    owned_host_pid = $(if ($null -eq $hostProc) { 0 } else { $hostProc.Id })
    owned_controller_pid = $(if ($null -eq $controllerProc) { 0 } else { $controllerProc.Id })
    host_lead_seconds = $HostLeadSeconds
    stable_seconds = $StableSeconds
    run_seconds_limit = $RunSeconds
    overall_passed = $overallPassed
    timed_out = $timedOut
    failure_classification = $failureClassification
    first_media_deadline_ms = $(if ($null -eq $evaluation) { 0 } else { $evaluation.FirstMediaDeadlineMs })
    failed_checks = $(if ($null -eq $evaluation) { @('evaluation_unavailable') } else { @($evaluation.FailedChecks) })
    host_stats_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.HostStatsLine })
    controller_stats_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.ControllerStatsLine })
    host_rates_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.HostRatesLine })
    controller_rates_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.ControllerRatesLine })
    gui_stats_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.GuiStatsLine })
    host_adaptation_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.HostAdaptationLine })
    host_diagnostics_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.HostDiagnosticsLine })
    host_input_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.HostInputLine })
    host_transport_feedback_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.HostTransportFeedbackLine })
    controller_transport_feedback_line = $(if ($null -eq $evaluation) { '' } else { $evaluation.ControllerTransportFeedbackLine })
    host_pacer_deadline_drops = $(if ($null -eq $evaluation) { 0 } else { $evaluation.HostPacerDeadlineDrops })
    injected_loss_required = [bool]$DropOneMediaFragment
    injected_feedback_class = $InjectedFeedbackClass
    forced_required_channel_close = $ForceRequiredChannelClose
    host_restart_count = $HostRestartCount
    host_restart_timeout_seconds = $HostRestartTimeoutSeconds
    host_restart_discovery_timeout_seconds = $HostRestartDiscoveryTimeoutSeconds
    host_restart_passed = $hostRestartPassed
    controller_gui_pid_baseline = $controllerGuiPidBaseline
    controller_runtime_pid_baseline = $controllerRuntimePidBaseline
    host_restart_records = @($hostRestartRecords)
    remote_log_after_reconnect_required = $remoteLogTransferRequired
    remote_log_after_reconnect_passed = $remoteLogTransferPassed
    remote_log_after_reconnect_error = $remoteLogTransferError
    remote_log_snapshot_request_id = $remoteLogSnapshotRequestId
    remote_log_snapshot_line_count = $remoteLogSnapshotLineCount
    remote_log_snapshot_path = $(if ($remoteLogSnapshotExists) { $remoteLogSnapshotPath } else { '' })
    remote_log_snapshot_sha256 = $(if ($remoteLogSnapshotExists) { (Get-FileHash -LiteralPath $remoteLogSnapshotPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' })
    screenshot_path = $(if ($screenshotExists) { $screenshotPath } else { '' })
    screenshot_sha256 = $(if ($screenshotExists) { (Get-FileHash -LiteralPath $screenshotPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' })
    screenshot_error = $screenshotError
    host_directory = $hostDirectory
    controller_directory = $controllerDirectory
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultJson -Encoding UTF8

Write-Host "[local-dual-gui-test] result=$resultJson"
Write-Host "[local-dual-gui-test] overall_passed=$overallPassed failed_checks=$($result.failed_checks -join ',')"

if (-not $overallPassed) {
    exit 1
}

exit 0
