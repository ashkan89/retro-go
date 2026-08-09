# List all your target configurations here
$Targets = @(
    "esp32-s3-n16r8-st7796",
    "esp32-s3-n8r2-st7796",
    "esp32-s3-n16r8-st7789v2",
    "esp32-s3-n8r2-st7789v2",
    "esp32-s3-n16r8-ili9341",
    "esp32-s3-n8r2-ili9341"
)

foreach ($Target in $Targets) {
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    Write-Host "Starting build for target: $Target" -ForegroundColor Cyan
    Write-Host "----------------------------------------" -ForegroundColor Cyan
    
    # Run the python build tool
    python rg_tool.py --target $Target build all

    # Check if the build failed ($LASTEXITCODE is not 0)
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed for target: $Target. Aborting remaining builds."
        Exit $LASTEXITCODE
    }
}

Write-Host "All builds completed successfully!" -ForegroundColor Green
