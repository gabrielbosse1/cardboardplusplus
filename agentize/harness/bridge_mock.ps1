param(
    [Parameter(Mandatory = $true)][string]$BridgeExe,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Base = 'http://127.0.0.1:8567'
)

# Scripted, deterministic session against the bridge control plane.
# Mock driver: binds 127.0.0.1:42070, ACKs BRIDGE_HELLO, records CAP/CFG bytes.
# Mock phone:  pushes hello/gyro/hand/ping telemetry on 127.0.0.1:42071.
# Everything the bridge publishes over REST + UDP is captured and written to $Out.
# Timing-only fields are masked so baseline and check compare byte-for-byte.

$ErrorActionPreference = 'Stop'

function Get-Ping { param([string]$s) "PING-OK" }

function Invoke-Rest { param([string]$UrlSub, [string]$Method = 'GET', [string]$Body = $null)
    try {
        if ($Method -eq 'POST') {
            $r = Invoke-WebRequest -Uri ($Base + $UrlSub) -Method Post -ContentType 'application/json' -Body $Body -TimeoutSec 5 -UseBasicParsing
        } else {
            $r = Invoke-WebRequest -Uri ($Base + $UrlSub) -Method Get -TimeoutSec 5 -UseBasicParsing
        }
        return $r.Content
    } catch {
        return ("__REST_ERROR: " + $_.Exception.Message)
    }
}

$mockEvents = Join-Path $env:TEMP ("mockdriver_{0}.txt" -f [Guid]::NewGuid().ToString('N'))

# ---------- mock driver: background job on UDP 42070 ----------
$job = Start-Job -ScriptBlock {
    param($outFile)
    $sock = $null
    try {
        $sock = New-Object System.Net.Sockets.UdpClient
        $sock.ExclusiveAddressUse = $false
        $sock.Client.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket,
                                     [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
        $sock.Client.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 42070))
        $sock.Client.ReceiveTimeout = 150
    } catch {
        "BIND_FAILED" | Out-File -FilePath $outFile -Encoding ascii -Force
        return
    }
    $events = @()
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $data = $sock.Receive([ref]$ep)
            $msg = [System.Text.Encoding]::UTF8.GetString($data)
            if ($msg -like 'BRIDGE_HELLO*') {
                $events += 'HELLO'
                $ack = [System.Text.Encoding]::UTF8.GetBytes('BRIDGE_ACK')
                $sock.Send($ack, $ack.Length, $ep) | Out-Null
            } elseif ($msg -like 'CARDBOARD_CAP*') {
                $events += $msg
            } elseif ($msg -like 'BRIDGE_CFG*') {
                $events += $msg
            }
        } catch { }
    }
    $sock.Close()
    $events -join "`n" | Out-File -FilePath $outFile -Encoding ascii -Force
} -ArgumentList $mockEvents

# ---------- helper: send a datagram ----------
function Send-Datagram {
    param([object]$Payload, [string]$Dest = '127.0.0.1', [int]$Port = 42071)
    $c = New-Object System.Net.Sockets.UdpClient
    try {
        if ($Payload -is [byte[]]) { $bytes = $Payload } else { $bytes = [System.Text.Encoding]::UTF8.GetBytes([string]$Payload) }
        $c.Connect($Dest, $Port) | Out-Null
        $c.Send($bytes, $bytes.Length) | Out-Null
    } finally { $c.Close() }
}

# ---------- telemetry packet builders ----------
function New-GyroPacket {
    $b = New-Object byte[] 33
    $b[0] = 0x10
    [BitConverter]::GetBytes([UInt64]1234).CopyTo($b, 1)
    $f = @([Single]0.5, [Single]-0.2, [Single]0.1, [Single]1.0, [Single]9.8, [Single]0.0)
    for ($i = 0; $i -lt 6; $i++) { [BitConverter]::GetBytes($f[$i]).CopyTo($b, 9 + 4 * $i) }
    return $b
}
function New-HandPacket {
    $b = New-Object byte[] 15
    $b[0] = 0x11
    [BitConverter]::GetBytes([UInt64]2345).CopyTo($b, 1)
    $b[9] = 2   # hands
    $b[10] = 21 # landmarks per hand
    [BitConverter]::GetBytes([Single]0.91).CopyTo($b, 11)
    return $b
}
function New-Ping { return [byte[]]@(0x20) }

# ---------- the session ----------
$proc = Start-Process -FilePath $BridgeExe -ArgumentList '--headless' -PassThru

try {
    # wait for the REST server to come up
    $up = $false
    for ($i = 0; $i -lt 25; $i++) {
        if ((Invoke-Rest '/health') -like '*"ok":true*') { $up = $true; break }
        Start-Sleep -Milliseconds 200
    }
    if (-not $up) { Write-Error "bridge REST server never came up (port busy or process failed)."; exit 1 }

    Start-Sleep -Milliseconds 1500   # let HELLO/ACK handshake land

    # phone telemetry
    Send-Datagram 'CARDBOARD_PHONE_HELLO v1'
    Start-Sleep -Milliseconds 120
    Send-Datagram (New-GyroPacket)
    Start-Sleep -Milliseconds 120
    Send-Datagram (New-HandPacket)
    Start-Sleep -Milliseconds 120
    Send-Datagram (New-Ping)
    Start-Sleep -Milliseconds 400

    $s1   = Invoke-Rest '/status'
    $l1   = Invoke-Rest '/logs?n=60'
    $cfg  = Invoke-Rest '/settings' 'POST' '{"width":1600,"height":900,"fps":60,"bitrate":8,"encoder":"nvenc"}'
    Start-Sleep -Milliseconds 400
    $s2   = Invoke-Rest '/status'
    $l2   = Invoke-Rest '/logs?n=60'

    Send-Datagram (New-Ping)         # keep phone alive within the timeout window
    Start-Sleep -Milliseconds 400

    $s3   = Invoke-Rest '/status'
    $h    = Invoke-Rest '/health'
    $idx  = Invoke-Rest '/'
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    Wait-Job $job -Timeout 15 | Out-Null
    Stop-Job $job -ErrorAction SilentlyContinue | Out-Null
    Remove-Job $job -Force -ErrorAction SilentlyContinue | Out-Null
}

# ---------- compose + canonicalize transcript ----------
$mockDriver = @()
if (Test-Path $mockEvents) {
    $raw = Get-Content -LiteralPath $mockEvents
    if ($raw) {
        # Canonicalize: HELLO multiplicity is timing-dependent, CAP/CFG is not.
        $helloCount = @($raw | Where-Object { $_ -eq 'HELLO' }).Count
        $events = New-Object System.Collections.Generic.List[string]
        if ($helloCount -gt 0) { $events.Add('HELLO (present)') }
        foreach ($m in ($raw | Where-Object { $_ -ne 'HELLO' } | Select-Object -Unique | Sort-Object)) { $events.Add($m) }
        $mockDriver = $events.ToArray()
    }
    if (-not $mockDriver) { $mockDriver = @('NO_EVENTS') }
} else {
    $mockDriver = @('NO_EVENTS')
}
Remove-Item -LiteralPath $mockEvents -Force -ErrorAction SilentlyContinue

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('== CARDBOARD BRIDGE TRANSCRIPT ==')
$lines.Add("[health]");  $lines.Add($h)
$lines.Add("[index]");   $lines.Add($idx)
$lines.Add("[status-1]"); $lines.Add($s1)
$lines.Add("[logs-1]");  $lines.Add($l1)
$lines.Add("[settings]"); $lines.Add($cfg)
$lines.Add("[status-2]"); $lines.Add($s2)
$lines.Add("[logs-2]");  $lines.Add($l2)
$lines.Add("[status-3]"); $lines.Add($s3)
$lines.Add('[mock-driver-events]')
foreach ($m in $mockDriver) { $lines.Add($m) }
$lines.Add('== END ==')

$canon = ($lines -join "`n")
$canon = $canon -replace '"(gyro_fps|hand_fps|stream_fps|latency_ms)"\s*:\s*\d+', '"$1":-1'
$canon = $canon -replace 'phone hello from 127\.0\.0\.1:\d+', 'phone hello from 127.0.0.1:PORT'
$canon | Out-File -FilePath $Out -Encoding utf8

# sanity: the session must actually have exercised the driver config push
if ($mockDriver -notcontains ('CARDBOARD_CAP 1600 900')) {
    Write-Error "session invariant broken: mock driver never received CARDBOARD_CAP. Events: $($mockDriver -join ' | ')"
    exit 1
}

Write-Output "OK"
exit 0