# PreToolUse guard. Blocks writes to any live database and unscoped recursive deletes.
#
# WHY THIS EXISTS AND NOT JUST settings.json:
#   settings.json allow/deny match the whole command string, so `safe && dangerous` slips
#   through (anthropics/claude-code#18846). `--dangerously-skip-permissions` disables
#   permission checks entirely but PreToolUse hooks still run. This hook is the real control.
#
# POLICY: allowlist, not blocklist. Exactly one schema is writable. A blocklist of names
#   containing "prod" would match none of the live schemas on this machine.
#
# CONTRACT: reads the hook JSON on stdin. Exit 0 allows, exit 2 blocks and returns stderr
#   to Claude. Any other exit code is a non-blocking error, so all failure paths here that
#   should stop the tool call must use exit 2.

[CmdletBinding()]
param(
    # Run the built-in assertions instead of acting as a hook.
    [switch]$Test
)

Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------
#
# Loaded from tools/dev-db/db-policy.json, falling back to db-policy.example.json.
# The real file is gitignored because it names the operator's own databases, which is not
# information to publish. Keeping policy in config rather than in this script is also why
# this file is safe to share.
#
# Fail-safe: if no policy can be read at all, the defaults below still refuse any
# state-changing statement that does not explicitly name the writable schema.

$WRITABLE_SCHEMA   = 'noggit_dev_world'
$PROTECTED_SCHEMAS = @('mysql', 'sys', 'performance_schema', 'information_schema')

$policyDir = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'tools/dev-db'
foreach ($candidate in @('db-policy.json', 'db-policy.example.json')) {
    $policyPath = Join-Path $policyDir $candidate
    if (-not (Test-Path $policyPath)) { continue }
    try {
        $policy = Get-Content $policyPath -Raw | ConvertFrom-Json
        if ($policy.writableSchema) { $WRITABLE_SCHEMA = [string]$policy.writableSchema }
        if ($policy.protectedSchemas) { $PROTECTED_SCHEMAS = @($policy.protectedSchemas) }
        break
    }
    catch {
        # A malformed policy file must not silently widen what is allowed. Keep the
        # conservative defaults above and carry on.
        [Console]::Error.WriteLine("guard-db: ignoring unreadable policy $candidate - $($_.Exception.Message)")
    }
}

# Write-capable MySQL clients. mysqldump is excluded: it only reads.
$WRITE_CAPABLE_CLIENTS = 'mysql|mysqladmin|mysqlimport|mysqlsh|mysqlpump'

# SQL that changes state. Deliberately broad.
$MUTATION_SQL = '\b(INSERT|UPDATE|DELETE|DROP|CREATE|ALTER|TRUNCATE|REPLACE|GRANT|REVOKE|RENAME|LOAD\s+DATA|LOCK\s+TABLES|SET\s+GLOBAL|FLUSH|SHUTDOWN)\b'

# ---------------------------------------------------------------------------
# Detection
# ---------------------------------------------------------------------------

function Get-ScrubbedCommand {
    <#
      Removes client *invocations* so the binary name cannot be mistaken for a schema name.
      'mysql' is both a client and a live system schema, so a naive text match blocks every
      legitimate write. Only command-position tokens are scrubbed: a token is an invocation
      when it is followed by whitespace or end-of-string. That keeps a qualified reference
      like `mysql.user` intact so writes to the system schema are still caught.
    #>
    param([string]$Command)
    return $Command -replace "(?i)(^|[\s`"'&|;(])(?:[^\s`"';|&]*[/\\])?($WRITE_CAPABLE_CLIENTS)(\.exe)?(?=\s|$)", '$1<CLIENT>'
}

function Test-DbViolation {
    <#
      Returns $null when the command is acceptable, otherwise the reason string.
      Evaluated against the whole command AND each decomposed segment, because a
      mutation keyword and its schema name can be separated by an operator.
    #>
    param([string]$Command)

    if ([string]::IsNullOrWhiteSpace($Command)) { return $null }

    $invokesWriteClient = $Command -match "(?i)(^|[\s`"'&|;(/\\])($WRITE_CAPABLE_CLIENTS)(\.exe)?\b"
    if (-not $invokesWriteClient) { return $null }

    $scrubbed = Get-ScrubbedCommand -Command $Command

    # mysqlimport and mysqlpump always write.
    $alwaysWrites = $Command -match '(?i)\b(mysqlimport|mysqlpump)(\.exe)?\b'

    # Feeding a .sql file in is a write even with no SQL keyword in the command text.
    $feedsSqlFile = ($Command -match '<\s*\S+\.sql') -or
                    ($Command -match '(?i)\bsource\s+\S+\.sql') -or
                    ($Command -match '\\\.\s+\S+\.sql')

    $hasMutationSql = $Command -match "(?i)$MUTATION_SQL"

    if (-not ($alwaysWrites -or $feedsSqlFile -or $hasMutationSql)) {
        return $null   # read-only use of a write-capable client
    }

    $named = @($PROTECTED_SCHEMAS | Where-Object {
        $scrubbed -match "(?i)(^|[^A-Za-z0-9_.])$([regex]::Escape($_))([^A-Za-z0-9_]|$)"
    })

    if ($named.Count -gt 0) {
        return "targets protected schema(s): $($named -join ', ')"
    }

    # A state-changing statement that names no schema at all runs against the connection's
    # default database, which we cannot see from here. Refuse unless the dev schema is explicit.
    if ($scrubbed -notmatch "(?i)\b$([regex]::Escape($WRITABLE_SCHEMA))\b") {
        return "state-changing SQL with no explicit schema; only '$WRITABLE_SCHEMA' is writable"
    }

    return $null
}

function Test-DeleteViolation {
    param([string]$Command)

    if ([string]::IsNullOrWhiteSpace($Command)) { return $null }

    $isRecursiveDelete =
        ($Command -match '(?i)\brm\s+(-[a-zA-Z]*[rR][a-zA-Z]*\s+)*-[a-zA-Z]*[fF]') -or
        ($Command -match '(?i)\brm\s+(-[a-zA-Z]*[fF][a-zA-Z]*\s+)*-[a-zA-Z]*[rR]') -or
        ($Command -match '(?i)\brm\s+-[a-zA-Z]*rf') -or
        ($Command -match '(?i)Remove-Item\b(?=.*-Recurse)(?=.*-Force)')

    if (-not $isRecursiveDelete) { return $null }

    # Filesystem roots and home directories are never acceptable targets.
    if ($Command -match '(?i)(rm\s+-\S+|Remove-Item.*)\s+["'']?([A-Za-z]:[\\/]?["'']?(\s|$)|[\\/]["'']?(\s|$)|~)') {
        return 'recursive delete targeting a filesystem root or home directory'
    }

    return $null
}

function Split-CommandSegments {
    param([string]$Command)
    # Split on shell operators so `harmless && harmful` is inspected piecewise.
    return ($Command -split '&&|\|\||;|\||\r?\n') | Where-Object { $_.Trim() }
}

function Get-Violation {
    param([string]$Command)

    $candidates = @($Command) + (Split-CommandSegments -Command $Command)
    foreach ($c in $candidates) {
        $r = Test-DbViolation -Command $c
        if ($r) { return "database write blocked - $r" }
        $r = Test-DeleteViolation -Command $c
        if ($r) { return "delete blocked - $r" }
    }
    return $null
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

if ($Test) {
    # Pin a fixture policy so results do not depend on the operator's db-policy.json.
    # 'legacy_world' stands in for a non-standard live schema of the kind that belongs in
    # local config rather than in this file.
    $WRITABLE_SCHEMA   = 'noggit_dev_world'
    $PROTECTED_SCHEMAS = @('world', 'characters', 'auth', 'legacy_world',
                           'mysql', 'sys', 'performance_schema', 'information_schema')

    $cases = @(
        @{ c = 'mysql -u trinity world -e "SELECT * FROM creature LIMIT 1;"';                 block = $false; why = 'read from live world' }
        @{ c = 'mysql -u trinity world -e "SHOW COLUMNS FROM creature;"';                     block = $false; why = 'show from live world' }
        @{ c = 'mysqldump --no-data world creature';                                          block = $false; why = 'dump is read-only' }
        @{ c = 'mysql noggit_dev_world -e "INSERT INTO creature VALUES (1);"';                 block = $false; why = 'write to dev schema' }
        @{ c = 'mysql -u trinity world -e "UPDATE creature SET curhealth=1;"';                block = $true;  why = 'write to live world' }
        @{ c = 'mysql -e "DROP DATABASE legacy_world;"';                                      block = $true;  why = 'drop live schema' }
        @{ c = 'git status && mysql world -e "DELETE FROM creature;"';                        block = $true;  why = 'compound bypass' }
        @{ c = 'mysql -e "USE world; DROP TABLE creature;"';                                  block = $true;  why = 'schema split from keyword' }
        @{ c = 'mysql world < patch.sql';                                                     block = $true;  why = 'sql file redirect' }
        @{ c = 'mysql -e "INSERT INTO creature VALUES (1);"';                                 block = $true;  why = 'mutation, no schema named' }
        @{ c = 'mysql -e "GRANT ALL ON mysql.* TO ''x''@''localhost'';"';                     block = $true;  why = 'grant on system schema' }
        @{ c = 'C:\Program Files\MySQL\bin\mysql.exe world -e "DROP TABLE creature;"';        block = $true;  why = 'full path client, live schema' }
        @{ c = 'mysql noggit_dev_world -e "DROP TABLE IF EXISTS creature;"';                  block = $false; why = 'ddl on dev schema' }
        @{ c = 'mysqlimport noggit_dev_world data.txt';                                       block = $false; why = 'import into dev schema' }
        @{ c = 'mysqlimport world data.txt';                                                  block = $true;  why = 'import into live schema' }
        @{ c = 'mysql characters -e "TRUNCATE TABLE guild;"';                                 block = $true;  why = 'truncate live characters' }
        # The drive letters below are arbitrary fixtures -- Test-DeleteViolation matches any
        # [A-Za-z]: root. Do not substitute a letter that exists on the machine you run this on.
        @{ c = 'rm -rf /';                                                                    block = $true;  why = 'delete root' }
        @{ c = 'rm -rf C:\';                                                                  block = $true;  why = 'delete drive root' }
        @{ c = 'Remove-Item -Recurse -Force D:\';                                             block = $true;  why = 'delete drive root ps' }
        @{ c = 'rm -rf build/CMakeFiles';                                                     block = $false; why = 'scoped build clean' }
        @{ c = 'cmake --build . --config RelWithDebInfo';                                     block = $false; why = 'ordinary build' }
        @{ c = 'git commit -m "add schema doc"';                                              block = $false; why = 'ordinary git' }
    )

    $fail = 0
    foreach ($t in $cases) {
        $v = Get-Violation -Command $t.c
        $blocked = [bool]$v
        $ok = ($blocked -eq $t.block)
        if (-not $ok) { $fail++ }
        $status = if ($ok) { 'PASS' } else { 'FAIL' }
        $act = if ($blocked) { 'BLOCK' } else { 'ALLOW' }
        "{0}  {1,-5} {2,-28} {3}" -f $status, $act, $t.why, $t.c
    }
    ''
    if ($fail -eq 0) {
        "All $($cases.Count) cases passed."
        exit 0
    }
    "$fail of $($cases.Count) cases FAILED."
    exit 1
}

# ---------------------------------------------------------------------------
# Hook entry point
# ---------------------------------------------------------------------------

$raw = [Console]::In.ReadToEnd()
if ([string]::IsNullOrWhiteSpace($raw)) { exit 0 }

try {
    $payload = $raw | ConvertFrom-Json
}
catch {
    # Unparseable input means we cannot reason about the call. Fail closed.
    [Console]::Error.WriteLine("guard-db: could not parse hook payload; blocking. $($_.Exception.Message)")
    exit 2
}

$command = $null
if ($payload.PSObject.Properties.Name -contains 'tool_input' -and $payload.tool_input) {
    if ($payload.tool_input.PSObject.Properties.Name -contains 'command') {
        $command = [string]$payload.tool_input.command
    }
}
if (-not $command) { exit 0 }

$violation = Get-Violation -Command $command
if ($violation) {
    [Console]::Error.WriteLine(@"
BLOCKED by .claude/hooks/guard-db.ps1: $violation

Only '$WRITABLE_SCHEMA' is writable. Live schemas are read-only:
  $($PROTECTED_SCHEMAS -join ', ')

Emit a reviewable .sql changeset instead, or target $WRITABLE_SCHEMA explicitly.
See CLAUDE.md HARD RULES 1 and 2.
"@)
    exit 2
}

exit 0
