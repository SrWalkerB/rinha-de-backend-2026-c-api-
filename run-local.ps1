<#
  Local end-to-end runner (Windows / Docker Desktop).
    .\run-local.ps1            full k6 load test (1->900 rps, 120s)
    .\run-local.ps1 -Smoke     quick smoke (5 iters)
    .\run-local.ps1 -Down      tear down the stack
    .\run-local.ps1 -NoBuild   skip image rebuild
  Brings up lb + api1 + api2, waits for /ready on :9999, runs the OFFICIAL k6
  harness, prints the score from results.json.
#>
param([switch]$Smoke, [switch]$Down, [switch]$NoBuild)
$ErrorActionPreference = "Stop"

$root  = $PSScriptRoot
$rinha = (Resolve-Path (Join-Path $root "..\rinha-de-backend-2026")).Path
$testDir = Join-Path $rinha "test"
$refsSrc = Join-Path $rinha "resources\references.json.gz"
$refsDst = Join-Path $root  "resources\references.json.gz"

if ($Down) { docker compose -f (Join-Path $root "docker-compose.yml") down; exit 0 }

# 1. Stage the dataset into the build context (needed by Dockerfile.api).
New-Item -ItemType Directory -Force (Join-Path $root "resources") | Out-Null
if (-not (Test-Path $refsDst) -or
    (Get-Item $refsSrc).Length -ne (Get-Item $refsDst -ErrorAction SilentlyContinue).Length) {
    Write-Host "Staging references.json.gz ..."
    Copy-Item $refsSrc $refsDst -Force
}

# 2. Build + start the stack.
$compose = Join-Path $root "docker-compose.yml"
if ($NoBuild) { docker compose -f $compose up -d }
else          { docker compose -f $compose up -d --build }

# 3. Wait for /ready (mirror engine: 20 x 3s).
Write-Host "Waiting for http://localhost:9999/ready ..."
$ok = $false
for ($i = 0; $i -lt 20; $i++) {
    try {
        $r = Invoke-WebRequest -Uri "http://localhost:9999/ready" -TimeoutSec 3 -UseBasicParsing
        if ($r.StatusCode -ge 200 -and $r.StatusCode -lt 300) { $ok = $true; break }
    } catch { Start-Sleep -Seconds 3 }
}
if (-not $ok) { Write-Error "API never became ready"; docker compose -f $compose logs --tail=50; exit 1 }
Write-Host "Ready."

# 4. Run k6 (native k6.exe if present, else dockerized with host networking).
$script = if ($Smoke) { "/test/smoke.js" } else { "/test/test.js" }
$nativeScript = if ($Smoke) { Join-Path $testDir "smoke.js" } else { Join-Path $testDir "test.js" }
$env:K6_NO_USAGE_REPORT = "true"
if (Get-Command k6 -ErrorAction SilentlyContinue) {
    Push-Location $rinha
    k6 run $nativeScript
    Pop-Location
} else {
    docker run --rm --network host -e K6_NO_USAGE_REPORT=true `
        -v "${testDir}:/test" grafana/k6 run $script
}

# 5. Print the score.
$results = Join-Path $testDir "results.json"
if (Test-Path $results) {
    Write-Host "`n===== RESULTS =====" -ForegroundColor Cyan
    Get-Content $results | Out-String | Write-Host
    $j = Get-Content $results | ConvertFrom-Json
    Write-Host ("p99 = {0} ms   final_score = {1}" -f $j.p99, $j.scoring.final_score) -ForegroundColor Green
} else {
    Write-Warning "no results.json produced"
}
