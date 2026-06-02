<#
  Build + push the public linux/amd64 images used by the submission compose.
    .\build-images.ps1 -User <dockerhub-user> [-Tag <tag>] [-Push]
  Stages references.json.gz, builds api (bakes packed.bin) + lb for amd64.
  Each image is pushed under an immutable version tag (-Tag, default = git
  short sha) AND :latest, so the submission compose can pin the version tag and
  never risk an old :latest cache.
#>
param(
  [Parameter(Mandatory=$true)][string]$User,
  [string]$Tag,
  [switch]$Push
)
$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$rinha = (Resolve-Path (Join-Path $root "..\rinha-de-backend-2026")).Path

if (-not $Tag) { $Tag = (git -C $root rev-parse --short HEAD).Trim() }

New-Item -ItemType Directory -Force (Join-Path $root "resources") | Out-Null
Copy-Item (Join-Path $rinha "resources\references.json.gz") (Join-Path $root "resources\references.json.gz") -Force

$apiV = "docker.io/$User/rinha-api-c:$Tag"; $apiL = "docker.io/$User/rinha-api-c:latest"
$lbV  = "docker.io/$User/rinha-lb-c:$Tag";  $lbL  = "docker.io/$User/rinha-lb-c:latest"

$out = if ($Push) { "--push" } else { "--load" }
docker buildx build --platform linux/amd64 -f Dockerfile.api -t $apiV -t $apiL $out $root
docker buildx build --platform linux/amd64 -f Dockerfile.lb  -t $lbV  -t $lbL  $out $root

Write-Host "Built + tagged :$Tag and :latest" -ForegroundColor Green
Write-Host "Pin the version tag in docker-compose.submission.yml:"
Write-Host "  image: $apiV"
Write-Host "  image: $lbV"
