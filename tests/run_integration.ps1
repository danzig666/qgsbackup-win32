param([Parameter(Mandatory=$true)][string]$Executable)

$fixture = Join-Path $PSScriptRoot 'fixtures\project.qgs'
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("QgsBackupTests_" + [Guid]::NewGuid().ToString('N'))
$defaultOutput = Join-Path $testRoot 'default'
$noSkipOutput = Join-Path $testRoot 'no-skip'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "Integration test failed: $Message" }
}

try {
    New-Item -ItemType Directory -Force $defaultOutput, $noSkipOutput | Out-Null
    $defaultLog = (& $Executable $fixture $defaultOutput | Out-String)
    Assert-True ($LASTEXITCODE -eq 0) 'default backup returned a failure exit code'
    & $Executable $fixture $noSkipOutput --no-skip-file | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) '--no-skip-file backup returned a failure exit code'

    Assert-True (Test-Path (Join-Path $defaultOutput 'project.qgs')) 'rewritten project is missing'
    Assert-True (Test-Path (Join-Path $defaultOutput 'project.qgd')) 'project-associated file is missing'
    Assert-True (-not (Test-Path (Join-Path $defaultOutput 'project.qml'))) 'project skip pattern was ignored'
    foreach ($extension in 'shp','shx','dbf','prj') { Assert-True (Test-Path (Join-Path $defaultOutput "layers\roads.$extension")) "Shapefile sidecar .$extension is missing" }
    Assert-True (-not (Test-Path (Join-Path $defaultOutput 'layers\roads.qml'))) 'layer skip pattern was ignored'
    Assert-True (-not (Test-Path (Join-Path $defaultOutput 'layers\photo.tif'))) 'TIF skip pattern was ignored'
    $unicodeName = "unicode-$([char]0x00E1).txt"
    Assert-True (Test-Path (Join-Path $defaultOutput ("layers\folderdata\nested\" + $unicodeName))) 'directory datasource was not recursively copied'
    Assert-True (@(Get-ChildItem (Join-Path $defaultOutput 'layers') -Filter 'data*.gpkg').Count -eq 2) 'basename collision was not resolved'
    Assert-True (Test-Path (Join-Path $noSkipOutput 'layers\photo.tif')) '--no-skip-file did not copy TIF'
    Assert-True (Test-Path (Join-Path $noSkipOutput 'layers\roads.qml')) '--no-skip-file did not copy QML sidecar'

    [xml]$project = Get-Content -Raw (Join-Path $defaultOutput 'project.qgs')
    $refs = @($project.SelectNodes('//*[local-name()="datasource"]') | ForEach-Object { $_.InnerText })
    Assert-True ([bool]($refs | Where-Object { $_ -like '*/tests/fixtures/data/photo.tif' })) 'skipped reference should point to the original source'
    Assert-True ($refs -contains 'data/missing.shp') 'missing reference should remain unchanged'
    Assert-True ([bool]($refs | Where-Object { $_ -eq 'file:layers/space%20data.csv?type=csv&delimiter=,' })) 'file URI or provider suffix was not preserved'
    Assert-True ($refs -contains 'https://example.com/service') 'HTTP datasource was changed'
    Assert-True ($refs -contains 'crs=EPSG:3857&type=xyz&url=https://tiles.example.com/{z}/{x}/{y}.png') 'XYZ URL datasource was changed'
    Assert-True ($defaultLog -match 'Copying files: embedded data') 'embedded progress was not summarized'
    Assert-True ($defaultLog -notmatch 'base64:PD94bWw') 'embedded payload leaked into progress output'
    Write-Output 'Integration tests passed'
}
finally {
    $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $resolvedTest = [IO.Path]::GetFullPath($testRoot)
    if ($resolvedTest.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase) -and $resolvedTest -ne $resolvedTemp -and (Test-Path $resolvedTest)) {
        Remove-Item -LiteralPath $resolvedTest -Recurse -Force
    }
}
