# Shared connection plumbing for the dev-DB tooling.
#
# Dot-source it:  . "$PSScriptRoot/DbConfig.ps1"
#
# Passwords are resolved at call time into MYSQL_PWD for the duration of one child process.
# They are never written to disk, never printed, and never placed on a command line (where
# they would be visible in the process table and trigger mysql's own insecurity warning).

Set-StrictMode -Version Latest

function Get-NoggitDbConfig {
    <#
      Prefers the operator's dev-db.config.json, falling back to the committed example so a
      fresh clone is usable without editing anything first. The real file is gitignored: it
      holds machine-specific paths.
    #>
    param([string]$ConfigPath)

    if ($ConfigPath) {
        if (-not (Test-Path $ConfigPath)) { throw "Config not found: $ConfigPath" }
        return Get-Content $ConfigPath -Raw | ConvertFrom-Json
    }

    foreach ($candidate in @('dev-db.config.json', 'dev-db.config.example.json')) {
        $p = Join-Path $PSScriptRoot $candidate
        if (Test-Path $p) { return Get-Content $p -Raw | ConvertFrom-Json }
    }
    throw "No config found in $PSScriptRoot (looked for dev-db.config.json and dev-db.config.example.json)"
}

function Get-MysqlExe {
    param($Config, [string]$Name = 'mysql.exe')

    $candidate = Join-Path $Config.mysqlBinDir $Name
    if (Test-Path $candidate) { return $candidate }

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    throw "$Name not found in $($Config.mysqlBinDir) or on PATH"
}

function Get-DbPassword {
    <#
      Order of resolution:
        1. The named environment variable.
        2. For the source connection only, the semicolon-delimited value of the given key in
           a TrinityCore .conf file (host;port;user;pass;db).
      Returns the password. Callers must not log it.
    #>
    param($Endpoint)

    $envName = $Endpoint.passwordEnv
    if ($envName) {
        $fromEnv = [Environment]::GetEnvironmentVariable($envName)
        if ($fromEnv) { return $fromEnv }
    }

    $hasFile = $Endpoint.PSObject.Properties.Name -contains 'sourcePasswordFile'
    if ($hasFile -and $Endpoint.sourcePasswordFile -and (Test-Path $Endpoint.sourcePasswordFile)) {
        $key = $Endpoint.sourcePasswordKey
        $line = Select-String -Path $Endpoint.sourcePasswordFile -Pattern "^\s*$key\s*=" | Select-Object -First 1
        if ($line) {
            $parts = (($line.Line -split '=', 2)[1].Trim().Trim('"')) -split ';'
            if ($parts.Count -ge 4) { return $parts[3] }
        }
    }

    throw "No password available. Set `$env:$envName before running."
}

function Invoke-MysqlClient {
    param(
        $Config,
        $Endpoint,
        [Parameter(Mandatory = $true)][string]$Sql,
        [string]$Schema,
        [switch]$WithHeaders
    )

    $exe = Get-MysqlExe -Config $Config
    if (-not $Schema) { $Schema = $Endpoint.schema }

    $env:MYSQL_PWD = Get-DbPassword -Endpoint $Endpoint
    try {
        $argv = @('-h', $Endpoint.host, '-P', $Endpoint.port, '-u', $Endpoint.user, '-B')
        if (-not $WithHeaders) { $argv += '-N' }
        $argv += @($Schema, '-e', $Sql)
        & $exe @argv
    }
    finally {
        $env:MYSQL_PWD = $null
    }
}

function Invoke-SourceSql {
    <#
      Reads from the live source schema. READ-ONLY BY CONTRACT: the guard hook blocks
      state-changing statements against protected schemas, and noggit_ro has SELECT only.
    #>
    param([Parameter(Mandatory = $true)][string]$Sql, [string]$Schema, [switch]$WithHeaders)

    $cfg = Get-NoggitDbConfig
    if (-not $Schema) { $Schema = $cfg.source.schema }
    Invoke-MysqlClient -Config $cfg -Endpoint $cfg.source -Sql $Sql -Schema $Schema -WithHeaders:$WithHeaders
}

function Invoke-DevSql {
    param([Parameter(Mandatory = $true)][string]$Sql, [switch]$WithHeaders)

    $cfg = Get-NoggitDbConfig
    Invoke-MysqlClient -Config $cfg -Endpoint $cfg.dev -Sql $Sql -WithHeaders:$WithHeaders
}
