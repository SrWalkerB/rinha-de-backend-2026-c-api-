<#
  Build + push the public linux/amd64 images used by the submission compose.
    .\build-images.ps1 -User <dockerhub-user> [-Push]
  Stages references.json.gz, builds api (bakes packed.bin) + lb for amd64.
#>
param([Parameter(Mandatory=$true)][string]$User, [switch]$Push)
$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$rinha = (Resolve-Path (Join-Path $root "..\rinha-de-backend-2026")).Path

New-Item -ItemType Directory -Force (Join-Path $root "resources") | Out-Null
Copy-Item (Join-Path $rinha "resources\references.json.gz") (Join-Path $root "resources\references.json.gz") -Force

$api = "docker.io/$User/rinha-api-c:latest"
$lb  = "docker.io/$User/rinha-lb-c:latest"

docker buildx build --platform linux/amd64 -f Dockerfile.api -t $api $(if($Push){"--push"}else{"--load"}) $root
docker buildx build --platform linux/amd64 -f Dockerfile.lb  -t $lb  $(if($Push){"--push"}else{"--load"}) $root

Write-Host "Built $api and $lb" -ForegroundColor Green
Write-Host "Update docker-compose.submission.yml: replace <DOCKERHUB_USER> with $User"
