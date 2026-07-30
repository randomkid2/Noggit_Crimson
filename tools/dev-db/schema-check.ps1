# Verifies a target schema against the assertions recorded in docs/schema-335.md.
#
#   powershell -NoProfile -File tools/dev-db/schema-check.ps1 -Target source
#   powershell -NoProfile -File tools/dev-db/schema-check.ps1 -Target both
#
# Every assertion below exists because a published reference got it wrong, or because the
# column name differs between the 3.3.5 and master branches. Hardcoding any of these in C++
# without checking at runtime silently corrupts emitted SQL.
#
# Exit 0 = all assertions hold. Exit 1 = at least one failed.

[CmdletBinding()]
param(
    [ValidateSet('source', 'dev', 'both')]
    [string]$Target = 'source'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot/DbConfig.ps1"
$cfg = Get-NoggitDbConfig

# --- Assertions -----------------------------------------------------------------------
# kind: column-present | column-absent | table-present | table-absent

$ASSERTIONS = @(
    @{ kind = 'column-present'; table = 'creature';                col = 'wander_distance'; why = 'renamed from spawndist; AzerothCore still uses the old name' }
    @{ kind = 'column-absent';  table = 'creature';                col = 'spawndist';       why = 'old TC / AzerothCore name must not be present' }
    @{ kind = 'column-absent';  table = 'creature';                col = 'Comment';         why = 'creature has no Comment column; comments live in the .sql text' }
    @{ kind = 'column-present'; table = 'creature';                col = 'StringId';        why = 'recent addition; absent on older TDB' }
    @{ kind = 'column-present'; table = 'creature';                col = 'zoneId';          why = 'present but core-derived - emit 0' }

    @{ kind = 'column-present'; table = 'creature_addon';          col = 'StandState';      why = 'bytes1 was split into discrete columns' }
    @{ kind = 'column-present'; table = 'creature_addon';          col = 'MountCreatureID'; why = 'part of the bytes1 split' }
    @{ kind = 'column-absent';  table = 'creature_addon';          col = 'bytes1';          why = 'the brief claims bytes1 exists; it does not' }
    @{ kind = 'column-absent';  table = 'creature_addon';          col = 'bytes2';          why = 'the brief claims bytes2 exists; it does not' }
    @{ kind = 'column-present'; table = 'creature_template_addon'; col = 'SheathState';     why = 'same split applies to the template table' }

    @{ kind = 'column-present'; table = 'creature_model_info';     col = 'Gender';                 why = '3.3.5 has Gender; master dropped it' }
    @{ kind = 'column-present'; table = 'creature_model_info';     col = 'DisplayID_Other_Gender'; why = 'older name was modelid_other_gender' }
    @{ kind = 'column-absent';  table = 'creature_model_info';     col = 'VerifiedBuild';          why = 'master has it, 3.3.5 does not' }

    @{ kind = 'column-present'; table = 'smart_scripts';           col = 'event_param5';    why = 'brief claims only 4 event params on 3.3.5' }
    @{ kind = 'column-present'; table = 'smart_scripts';           col = 'target_param4';   why = 'brief claims only 3 target params' }

    @{ kind = 'column-present'; table = 'gameobject_template_addon'; col = 'artkit3';       why = 'artkit0..3 exist' }
    @{ kind = 'column-absent';  table = 'gameobject_template_addon'; col = 'artkit4';       why = 'master-only' }
    @{ kind = 'column-absent';  table = 'gameobject_template_addon'; col = 'WorldEffectID'; why = 'master-only' }

    @{ kind = 'column-present'; table = 'creature_template';       col = 'modelid1';        why = 'the only model source on this schema' }
    @{ kind = 'column-present'; table = 'creature_template';       col = 'AIName';          why = 'decides which scripting system owns the creature' }

    @{ kind = 'column-present'; table = 'waypoint_data';           col = 'wpguid';          why = 'core-managed; never author it' }
    @{ kind = 'column-present'; table = 'conditions';              col = 'ConditionStringValue1'; why = 'brief omits this column' }

    @{ kind = 'table-present';  table = 'waypoint_data';    why = 'the one waypoint path table that exists' }
    @{ kind = 'table-present';  table = 'pool_members';     why = 'unified replacement for pool_creature/pool_gameobject' }
    @{ kind = 'table-absent';   table = 'waypoints';        why = 'brief claims SmartAI paths live here; table does not exist' }
    @{ kind = 'table-absent';   table = 'script_waypoint';  why = 'brief claims escort scripts use it; table does not exist' }
    @{ kind = 'table-absent';   table = 'creature_template_model'; why = 'brief claims it coexists with modelid1..4; it does not' }
    @{ kind = 'table-absent';   table = 'pool_creature';    why = 'replaced by pool_members' }
    @{ kind = 'table-absent';   table = 'pool_gameobject';  why = 'replaced by pool_members' }
)

# --- Helpers --------------------------------------------------------------------------

function Get-SchemaFacts {
    param([string]$Which, [string]$Schema)

    $sql = @"
SELECT CONCAT('T|', table_name) FROM information_schema.tables  WHERE table_schema = '$Schema'
UNION ALL
SELECT CONCAT('C|', table_name, '|', column_name) FROM information_schema.columns WHERE table_schema = '$Schema';
"@

    $rows = if ($Which -eq 'source') { Invoke-SourceSql -Sql $sql -Schema $Schema } else { Invoke-DevSql -Sql $sql }

    $tables  = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $columns = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($r in $rows) {
        if (-not $r) { continue }
        $p = $r -split '\|'
        if ($p[0] -eq 'T') { [void]$tables.Add($p[1]) }
        elseif ($p[0] -eq 'C') { [void]$columns.Add("$($p[1]).$($p[2])") }
    }
    return @{ Tables = $tables; Columns = $columns }
}

function Get-DbVersion {
    param([string]$Which, [string]$Schema, $Facts)

    # Probe both. The reference DB here has only `version`; other TDBs have version_db_world.
    #
    # SQL literals must be single-quoted. Double quotes inside an argument get mangled by
    # PowerShell's native-command argument passing, and mysql then rejects the option and
    # prints its version banner instead of running the query.
    if ($Facts.Tables.Contains('version_db_world')) {
        $q = "SELECT CONCAT('version_db_world: ', db_version) FROM version_db_world LIMIT 1;"
    }
    elseif ($Facts.Tables.Contains('version')) {
        $q = "SELECT CONCAT('version: ', db_version, ' / core ', core_revision) FROM version LIMIT 1;"
    }
    else {
        return 'NO VERSION TABLE (neither version_db_world nor version)'
    }

    $r = if ($Which -eq 'source') { Invoke-SourceSql -Sql $q -Schema $Schema } else { Invoke-DevSql -Sql $q }
    return ($r | Select-Object -First 1)
}

function Invoke-SchemaCheck {
    param([string]$Which, [string]$Schema)

    Write-Host ''
    Write-Host "=== $Which : $Schema ===" -ForegroundColor Cyan

    $facts = Get-SchemaFacts -Which $Which -Schema $Schema
    Write-Host "tables: $($facts.Tables.Count)   columns: $($facts.Columns.Count)"
    Write-Host "version: $(Get-DbVersion -Which $Which -Schema $Schema -Facts $facts)"
    Write-Host ''

    $failures = @()
    foreach ($a in $ASSERTIONS) {
        switch ($a.kind) {
            'column-present' { $held = $facts.Columns.Contains("$($a.table).$($a.col)"); $label = "$($a.table).$($a.col) present" }
            'column-absent'  { $held = -not $facts.Columns.Contains("$($a.table).$($a.col)"); $label = "$($a.table).$($a.col) absent" }
            'table-present'  { $held = $facts.Tables.Contains($a.table); $label = "table $($a.table) present" }
            'table-absent'   { $held = -not $facts.Tables.Contains($a.table); $label = "table $($a.table) absent" }
        }

        if ($held) {
            Write-Host ("  PASS  {0}" -f $label)
        }
        else {
            Write-Host ("  FAIL  {0}  <- {1}" -f $label, $a.why) -ForegroundColor Red
            $failures += $label
        }
    }

    return $failures
}

# --- Run ------------------------------------------------------------------------------

$allFailures = @()

if ($Target -in @('source', 'both')) {
    $allFailures += Invoke-SchemaCheck -Which 'source' -Schema $cfg.source.schema
}
if ($Target -in @('dev', 'both')) {
    $allFailures += Invoke-SchemaCheck -Which 'dev' -Schema $cfg.dev.schema
}

Write-Host ''
if ($allFailures.Count -eq 0) {
    Write-Host "All $($ASSERTIONS.Count) assertions hold." -ForegroundColor Green
    exit 0
}
Write-Host "$($allFailures.Count) assertion(s) FAILED:" -ForegroundColor Red
$allFailures | ForEach-Object { Write-Host "  - $_" }
Write-Host ''
Write-Host 'A failure means docs/schema-335.md is stale for this target, or the target is a'
Write-Host 'different core/branch (AzerothCore, TC master). Update the doc, do not "fix" the DB.'
exit 1
