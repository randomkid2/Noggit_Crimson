# Rebuilds noggit_dev_world: copies table STRUCTURE from the live source schema, then applies
# synthetic fixtures. No source data is ever copied.
#
#   powershell -NoProfile -File tools/dev-db/seed-dev-db.ps1
#
# Prerequisites:
#   1. tools/dev-db/01_bootstrap_root.sql has been run as root.
#   2. $env:NOGGIT_DEV_DB_PWD holds the noggit_rw password.
#
# Structure comes from the live schema rather than a hand-written DDL file on purpose: a
# hand-written schema only ever proves the doc agrees with itself, so real column drift stays
# invisible. Copying structure makes /schema-check a genuine comparison.

[CmdletBinding()]
param(
    # Skip the structure copy and only re-apply fixtures.
    [switch]$FixturesOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/DbConfig.ps1"

$cfg = Get-NoggitDbConfig

# --- Safety ---------------------------------------------------------------------------
# The guard hook protects the agent's shell calls. This protects the script itself, which is
# the thing actually issuing DDL, and which the hook cannot see inside.
$PROTECTED = @('mysql', 'sys', 'performance_schema', 'information_schema')
foreach ($candidate in @('db-policy.json', 'db-policy.example.json')) {
    $policyPath = Join-Path $PSScriptRoot $candidate
    if (-not (Test-Path $policyPath)) { continue }
    $policy = Get-Content $policyPath -Raw | ConvertFrom-Json
    if ($policy.protectedSchemas) { $PROTECTED = @($policy.protectedSchemas) }
    break
}

if ($PROTECTED -contains $cfg.dev.schema) {
    throw "REFUSING TO RUN: dev schema is '$($cfg.dev.schema)', which is a protected live schema."
}
if ($cfg.dev.schema -ne 'noggit_dev_world') {
    Write-Warning "Dev schema is '$($cfg.dev.schema)', not the expected 'noggit_dev_world'."
}
if ($cfg.dev.schema -eq $cfg.source.schema) {
    throw "REFUSING TO RUN: source and dev schema are both '$($cfg.dev.schema)'."
}

$genDir = Join-Path $PSScriptRoot '.generated'
if (-not (Test-Path $genDir)) { New-Item -ItemType Directory -Path $genDir | Out-Null }
$structureFile = Join-Path $genDir 'structure.sql'

# --- 1. Copy structure ----------------------------------------------------------------
if (-not $FixturesOnly) {
    Write-Host "Dumping structure of $($cfg.tables.Count) tables from '$($cfg.source.schema)'..."

    $dump = Get-MysqlExe -Config $cfg -Name 'mysqldump.exe'
    $env:MYSQL_PWD = Get-DbPassword -Endpoint $cfg.source
    try {
        # --no-tablespaces matters: without it mysqldump demands the global PROCESS privilege,
        # which the project's source account does not have.
        $argv = @(
            '-h', $cfg.source.host, '-P', $cfg.source.port, '-u', $cfg.source.user,
            '--no-data', '--no-tablespaces', '--skip-comments', '--add-drop-table',
            '--default-character-set=utf8mb4',
            $cfg.source.schema
        ) + $cfg.tables

        & $dump @argv | Set-Content -Path $structureFile -Encoding UTF8
        if ($LASTEXITCODE -ne 0) { throw "mysqldump failed with exit code $LASTEXITCODE" }
    }
    finally {
        $env:MYSQL_PWD = $null
    }

    $tableCount = (Select-String -Path $structureFile -Pattern '^CREATE TABLE' -AllMatches).Count
    Write-Host "  wrote $structureFile ($tableCount CREATE TABLE statements)"
    if ($tableCount -ne $cfg.tables.Count) {
        Write-Warning "Expected $($cfg.tables.Count) tables, dumped $tableCount. Check for missing tables in the source schema."
    }

    Write-Host "Applying structure to '$($cfg.dev.schema)'..."
    $srcPath = ($structureFile -replace '\\', '/')
    Invoke-DevSql -Sql "source $srcPath" | Out-Null
}

# --- 2. Apply fixtures ----------------------------------------------------------------
Write-Host "Applying synthetic fixtures..."
$seedPath = ((Join-Path $PSScriptRoot '02_seed_synthetic.sql') -replace '\\', '/')
Invoke-DevSql -Sql "source $seedPath" -WithHeaders

# --- 3. Verify ------------------------------------------------------------------------
Write-Host ''
Write-Host 'Verification:'
Invoke-DevSql -WithHeaders -Sql @'
SELECT
  (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE()) AS tables_,
  (SELECT db_version FROM version LIMIT 1)                                        AS db_version,
  (SELECT COUNT(*) FROM creature)                                                 AS creatures,
  (SELECT COUNT(*) FROM gameobject)                                               AS gameobjects,
  (SELECT COUNT(*) FROM waypoint_data)                                            AS waypoints;
'@

Write-Host ''
Write-Host 'Tile lookup check (expect the 3 fixture creatures in tile 49_31):'
Invoke-DevSql -WithHeaders -Sql @'
SELECT guid, id, FLOOR(32-(position_x/533.33333)) AS blockX,
                 FLOOR(32-(position_y/533.33333)) AS blockY,
       MovementType, wander_distance
FROM creature
WHERE map = 0
  AND FLOOR(32-(position_x/533.33333)) = 49
  AND FLOOR(32-(position_y/533.33333)) = 31
ORDER BY guid;
'@
