# Shared release targets used by GitHub Actions and local builds.
$Targets = Get-Content -LiteralPath (Join-Path $PSScriptRoot "tools/release-targets.json") -Raw | ConvertFrom-Json

foreach ($Target in $Targets) {
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    Write-Host "Starting build for target: $Target" -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    
    # Run the python build tool
    python rg_tool.py --target $Target release factory launcher retro-core prboom-go gwenesis fmsx retro-legacy

    # Check if the build failed ($LASTEXITCODE is not 0)
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed for target: $Target. Aborting remaining builds."
        Exit $LASTEXITCODE
    }
}

Write-Host "All builds completed successfully!" -ForegroundColor Green
