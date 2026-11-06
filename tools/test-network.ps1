param([Parameter(Mandatory=$true)][string]$Address)
$ErrorActionPreference = 'Stop'
$payload = [Text.Encoding]::ASCII.GetBytes('Pi-Zero-W-uCOS2-echo')
$udp = [Net.Sockets.UdpClient]::new()
try {
    $udp.Client.ReceiveTimeout = 5000
    $udp.Connect($Address,5000)
    [void]$udp.Send($payload,$payload.Length)
    $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any,0)
    $received = $udp.Receive([ref]$remote)
    if ([Convert]::ToBase64String($received) -ne [Convert]::ToBase64String($payload)) {
        throw 'UDP echo mismatch'
    }
    Write-Output 'PASS: UDP echo on port 5000'
} finally { $udp.Dispose() }
$tcp = [Net.Sockets.TcpClient]::new()
try {
    $connect = $tcp.ConnectAsync($Address,5001)
    if (-not $connect.Wait(5000)) { throw 'TCP connection timed out' }
    $tcp.ReceiveTimeout = 5000
    $tcp.SendTimeout = 5000
    $stream = $tcp.GetStream()
    $stream.Write($payload,0,$payload.Length)
    $received = New-Object byte[] $payload.Length
    $offset = 0
    while ($offset -lt $received.Length) {
        $count = $stream.Read($received,$offset,$received.Length-$offset)
        if ($count -eq 0) { throw 'TCP closed before full echo' }
        $offset += $count
    }
    if ([Convert]::ToBase64String($received) -ne [Convert]::ToBase64String($payload)) {
        throw 'TCP echo mismatch'
    }
    Write-Output 'PASS: TCP echo on port 5001'
} finally { $tcp.Dispose() }
