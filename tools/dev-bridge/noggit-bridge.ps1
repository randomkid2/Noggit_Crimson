# Drive a NOGGIT_DEV_BRIDGE build over its loopback command socket.
#
# The bridge exists so rendering can be checked without a human at the window. It is compiled only
# into a -DNOGGIT_DEV_BRIDGE=ON build and listens only when NOGGIT_BRIDGE_PORT is set, so this
# script does nothing against an ordinary build -- see src/noggit/DevBridge.hpp.
#
# The socket is created by MapView, which means it appears once a map is open, not at startup.
# Wait-NoggitBridge exists for exactly that gap.
#
#   . tools/dev-bridge/noggit-bridge.ps1
#   Start-Noggit
#   Wait-NoggitBridge                       # blocks until a map has been opened
#   Send-Noggit "goto -9500 70 58"
#   Send-Noggit "loadspawns"
#   Send-Noggit "screenshot C:\path\shot.png"

$script:NoggitBridgePort = if ($env:NOGGIT_BRIDGE_PORT) { [int]$env:NOGGIT_BRIDGE_PORT } else { 27055 }

function Send-Noggit
{
    <#
      .SYNOPSIS
        Send one command line and return the single reply line.
      .DESCRIPTION
        One connection per command. That is slower than holding one open, and it is deliberate:
        a dropped connection then costs one command rather than wedging a session, and nothing has
        to track socket state across calls.
    #>
    param
    (
        [Parameter(Mandatory = $true)][string] $Command,
        [int] $Port = $script:NoggitBridgePort,
        [int] $TimeoutMs = 30000
    )

    $client = New-Object System.Net.Sockets.TcpClient
    try
    {
        # 127.0.0.1 explicitly rather than "localhost": the latter can resolve to ::1 first, and
        # the bridge binds QHostAddress::LocalHost, which is IPv4.
        $connect = $client.BeginConnect('127.0.0.1', $Port, $null, $null)

        if (-not $connect.AsyncWaitHandle.WaitOne(2000))
        {
            throw "No bridge listening on 127.0.0.1:$Port. Is a map open, and was noggit started with NOGGIT_BRIDGE_PORT set?"
        }

        $client.EndConnect($connect)
        $client.ReceiveTimeout = $TimeoutMs

        $stream = $client.GetStream()
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.NewLine = "`n"
        $writer.AutoFlush = $true
        $writer.WriteLine($Command)

        $reader = New-Object System.IO.StreamReader($stream)
        $reply = $reader.ReadLine()

        if ($null -eq $reply)
        {
            throw "Bridge closed the connection without replying to: $Command"
        }

        return $reply
    }
    finally
    {
        $client.Close()
    }
}

function Wait-NoggitBridge
{
    <#
      .SYNOPSIS
        Block until the bridge answers, i.e. until a map has been opened.
      .DESCRIPTION
        MapView creates the socket, and MapView does not exist until a project and map are chosen,
        so there is no way to shorten this from the script side -- only to wait for it.
    #>
    param
    (
        [int] $Port = $script:NoggitBridgePort,
        [int] $TimeoutSeconds = 300
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ((Get-Date) -lt $deadline)
    {
        try
        {
            if ((Send-Noggit -Command 'ping' -Port $Port) -eq 'OK pong')
            {
                Write-Output "Bridge is up on 127.0.0.1:$Port"
                return $true
            }
        }
        catch
        {
            Start-Sleep -Milliseconds 750
        }
    }

    throw "Bridge did not come up within $TimeoutSeconds seconds. Open a map in Noggit first."
}

function Start-Noggit
{
    <#
      .SYNOPSIS
        Launch a dev-bridge build with the port armed.
    #>
    param
    (
        [string] $Exe = "$PSScriptRoot\..\..\build\bin\RelWithDebInfo\noggit.exe",
        [int] $Port = $script:NoggitBridgePort
    )

    if (-not (Test-Path $Exe))
    {
        throw "No binary at $Exe. Configure with -DNOGGIT_DEV_BRIDGE=ON and build the noggit target."
    }

    $env:NOGGIT_BRIDGE_PORT = "$Port"

    $process = Start-Process -FilePath $Exe -PassThru -WorkingDirectory (Split-Path $Exe -Parent)
    Write-Output "Started noggit (PID $($process.Id)) with NOGGIT_BRIDGE_PORT=$Port"

    return $process
}
