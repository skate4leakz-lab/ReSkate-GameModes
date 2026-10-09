<#
Builds and (with -Publish) releases ReSkate with game modes to a GitHub repository whose
releases the launcher updates from. Players run that launcher once; from then on every
launch checks the repository's latest release and replaces ReSkate.dll and the launcher
when they changed.

  powershell -ExecutionPolicy Bypass -File contrib\release-modes.ps1 -Version 1.1.8-modes.1 -Repo owner/name
  ... -Notes "What changed" -Publish

The release build goes to build\release-modes (auto-update on, pointed at -Repo); the
development build in build\vs2022-x64 keeps updates off so a release never replaces the DLL
being tested. launcher.json pins the game build from Engine\Game\Build\supported_build.h, so a
release can only ask for the skate. build this source supports, and takes the DepotDownloader
pin from official ReSkate's latest release. Publishing needs the GitHub CLI (gh), signed in.
#>
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+')][string]$Version,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repo,
    [string]$Notes = '',
    [string]$Title = '',
    [switch]$Publish
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $root 'build\release-modes'
$out = Join-Path $root "build\release-out\$Version"
$gh = 'C:\Program Files\GitHub CLI\gh.exe'
# GitHub's runners have gh on the path rather than in Program Files.
if (-not (Test-Path $gh) -and (Get-Command gh -ErrorAction SilentlyContinue)) { $gh = (Get-Command gh).Source }

function Step($text) { Write-Host "== $text" -ForegroundColor Cyan }
function Sha256($path) { (Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant() }

# 1. Build with auto-update on, pointed at the repository.
Step "Building $Version for $Repo"
$devShell = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1'
if (Test-Path $devShell) { & $devShell -Arch amd64 -SkipAutomaticLocation | Out-Null }
$env:VCLibPackagePath = Join-Path $root 'build\no-vcpkg\vcpkg.user'
cmake -S $root -B $buildDir -G 'Visual Studio 17 2022' -A x64 -T v143 "-DDINGOSDK_VERSION=$Version" `
    -DDINGOSDK_LAUNCHER_AUTO_UPDATE=ON "-DDINGOSDK_RELEASE_REPO=$Repo"
if ($LASTEXITCODE) { throw 'CMake configure failed' }
cmake --build $buildDir --config Release --target dingosdk_runtime dingosdk_launcher -- /m
if ($LASTEXITCODE) { throw 'Build failed' }

# 2. The files players get.
Step 'Collecting files'
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
$dll = Join-Path $out 'ReSkate.dll'
$launcher = Join-Path $out 'ReSkateLauncher.exe'
Copy-Item (Join-Path $buildDir 'Release\ReSkate.dll') $dll
Copy-Item (Join-Path $buildDir 'Release\ReSkateLauncher.exe') $launcher

# 3. launcher.json: our launcher and runtime, the game build this source supports, and
#    official ReSkate's DepotDownloader pin.
Step 'Writing launcher.json'
$pins = Get-Content (Join-Path $root 'Engine\Game\Build\supported_build.h') -Raw
function Pin($pattern) {
    $m = [regex]::Match($pins, $pattern)
    if (-not $m.Success) { throw "supported_build.h has no match for $pattern" }
    $m.Groups[1].Value
}
$game = [ordered]@{
    app_id       = [int64](Pin 'steam_app_id\s*=\s*(\d+)')
    depot_id     = [int64](Pin 'steam_depot_id\s*=\s*(\d+)')
    manifest_id  = Pin 'steam_manifest_id\s*=\s*"(\d+)"'
    build_id     = Pin 'steam_build_id\s*=\s*"(\d+)"'
    skate_sha256 = Pin 'game_sha256\s*=\s*"([0-9a-f]{64})"'
}
$headers = @{ 'User-Agent' = 'reskate-modes-release' }
# Signed requests on GitHub's runners, whose shared addresses run out of anonymous ones.
if ($env:GH_TOKEN) { $headers['Authorization'] = "Bearer $env:GH_TOKEN" }
$official = Invoke-RestMethod 'https://api.github.com/repos/Dingo-Shenanigans/ReSkate/releases/latest' -Headers $headers
$officialJson = Invoke-RestMethod ($official.assets | Where-Object name -eq 'launcher.json').browser_download_url -Headers $headers
if ($officialJson.game.build_id -ne $game.build_id) {
    Write-Warning ("Official ReSkate $($official.tag_name) targets game build $($officialJson.game.build_id); " +
                   "this source supports $($game.build_id). Merge the newer ReSkate before releasing for it.")
}
$config = [ordered]@{
    schema           = 1
    launcher         = [ordered]@{ version = $Version; url = 'asset:ReSkateLauncher.exe'; sha256 = Sha256 $launcher; size = (Get-Item $launcher).Length }
    runtime          = [ordered]@{ version = $Version; url = 'asset:ReSkate.dll'; sha256 = Sha256 $dll; size = (Get-Item $dll).Length }
    game             = $game
    depot_downloader = $officialJson.depot_downloader
}
$json = Join-Path $out 'launcher.json'
# UTF-8 without a byte-order mark, on Windows PowerShell 5.1 as well as 7.
[IO.File]::WriteAllText($json, ($config | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))

# 4. A zip for a first install.
Step 'Packaging'
$readme = Join-Path $out 'INSTALL.txt'
@"
ReSkate with game modes $Version

First install: close skate., copy ReSkateLauncher.exe and ReSkate.dll into your skate. folder
(the one with Skate.exe), then start the game with ReSkateLauncher.exe.

After that the launcher keeps itself and the mod up to date from https://github.com/$Repo/releases
every time you start it.

Game modes: open the ReSkate menu (Insert) > GAME MODES, or type `mode help` in the console.
Source code (GPL-3.0): https://github.com/$Repo
"@ | Set-Content -Path $readme
$zip = Join-Path $out "ReSkate-GameModes-$Version.zip"
Compress-Archive -Path $launcher, $dll, $readme -DestinationPath $zip -Force

Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
Write-Host "launcher.json:"; Get-Content $json | Write-Host

if (-not $Publish) {
    Write-Host "Built only. Re-run with -Publish to create release v$Version on $Repo." -ForegroundColor Yellow
    return
}

# 5. Publish: the newest release is what every launcher reads.
Step "Publishing v$Version to $Repo"
if (-not (Test-Path $gh)) { throw 'The GitHub CLI is not installed (winget install GitHub.cli)' }
& $gh auth status | Out-Null
if ($LASTEXITCODE) { throw 'The GitHub CLI is not signed in (gh auth login)' }
$title = if ($Title) { $Title } else { "ReSkate Game Modes $Version" }
if (-not $Notes) { $Notes = "ReSkate with game modes $Version. First install: see INSTALL.txt in the zip. The launcher updates itself after that." }
# Tagged on the commit this was built from, which must already be on GitHub (git push).
$commit = (& git -C $root rev-parse HEAD).Trim()
& $gh release create "v$Version" --repo $Repo --target $commit --title $title --notes $Notes --latest $json $dll $launcher $zip
if ($LASTEXITCODE) { throw 'gh release create failed' }
Write-Host "Published: https://github.com/$Repo/releases/tag/v$Version" -ForegroundColor Green
