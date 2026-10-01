# Checks the ZIP from the package step (build\packages). Never launches TF3.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$repo=Split-Path $PSScriptRoot -Parent
$version=(Get-Content -LiteralPath (Join-Path $repo 'VERSION') -Raw).Trim()
$packages=Join-Path $repo 'build\packages'
$run=Join-Path $repo 'build\package-test'
if ([IO.Path]::GetFullPath($run) -ne [IO.Path]::GetFullPath((Join-Path $repo 'build\package-test'))) { throw 'Unsafe test directory' }
if (Test-Path -LiteralPath $run) { Remove-Item -LiteralPath $run -Recurse -Force }
function Assert($condition,[string]$message) { if (!$condition) { throw $message } }
function Text([string]$path) { return [IO.File]::ReadAllText($path) }
function Files([string]$zip) {
  $archive=[IO.Compression.ZipFile]::OpenRead($zip)
  try { return @($archive.Entries | Where-Object { !$_.FullName.EndsWith('/') } | ForEach-Object { $_.FullName }) } finally { $archive.Dispose() }
}

# Exactly the drop-in layout from the install rules in CMakeLists.txt.
$zip=Join-Path $packages "FeverScaler-v$version-windows-x64.zip"
$own='scripts/feverscaler/'
$expected=@('winhttp.dll','winhttp.ini','RTX40MFG.asi','RTX40MFGCore.dll','FeverScaler-README.txt','scripts/feverscaler.asi')
$expected+=@('LICENSE.txt','THIRD_PARTY_NOTICES.txt','DIAGNOSTICS.txt','VERSION.txt','licenses/rtx30mfg-compact-logging.patch') | ForEach-Object { $own+$_ }
$expected+=@('sl.interposer','sl.common','sl.dlss_g','sl.dlss','nvngx_dlss','sl.reflex','sl.pcl','nvngx_dlssg','NvLowLatencyVk') | ForEach-Object { $own+'Streamline/'+$_+'.dll' }
$expected+=Get-ChildItem -LiteralPath (Join-Path $repo 'licenses') -File | ForEach-Object { $own+'licenses/'+$_.Name }
$expected+=Get-ChildItem -LiteralPath (Join-Path $repo 'settings') -File | ForEach-Object { $own+'settings/'+$_.Name }
$entries=Files $zip
$difference=Compare-Object $expected $entries
Assert (!$difference) ('ZIP layout differs: '+(($difference | ForEach-Object { $_.SideIndicator+$_.InputObject }) -join ', '))
$extracted=Join-Path $run 'extracted'
Expand-Archive -LiteralPath $zip -DestinationPath $extracted
Assert ([Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $extracted 'scripts/feverscaler.asi')).ProductVersion -eq $version) 'DLL resource version differs from VERSION'
Assert ((Text (Join-Path $extracted 'scripts/feverscaler/VERSION.txt')) -eq "$version`n") 'VERSION.txt differs from VERSION'
# Only the load point, so the unlock in the game folder and other mods' plugins load.
Assert ((Text (Join-Path $extracted 'winhttp.ini')) -eq "[GlobalSets]`r`nDontLoadFromDllMain=0`r`n") 'winhttp.ini must only set DontLoadFromDllMain=0'

# The removal instructions name everything the ZIP adds.
$install=Text (Join-Path $repo 'docs/INSTALL.txt')
$removed=@('winhttp.dll','winhttp.ini','RTX40MFG.asi','RTX40MFGCore.dll','FeverScaler-README.txt')
foreach ($item in $removed+@('scripts\feverscaler.asi','scripts\feverscaler\')) { Assert ($install.Contains($item)) ('Install/removal instructions do not mention '+$item) }
Assert (@($entries | Where-Object { $_ -notmatch '^scripts/feverscaler(\.asi|/)' -and $_ -notin $removed }).Count -eq 0) 'ZIP adds files the removal instructions do not cover'

# Extracting an upgrade preserves runtime settings, logs and unrelated mods in an isolated fixture.
$fixture=Join-Path $run 'game'
New-Item -ItemType Directory -Path (Join-Path $fixture 'scripts') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $fixture 'scripts\feverscaler.ini'), "[feverscaler]`nKeyMenu=Ctrl+F8`n")
[IO.File]::WriteAllText((Join-Path $fixture 'feverscaler.log'), 'existing diagnostics')
[IO.File]::WriteAllText((Join-Path $fixture 'scripts\another-mod.asi'), 'another mod')
Expand-Archive -LiteralPath $zip -DestinationPath $fixture -Force
Expand-Archive -LiteralPath $zip -DestinationPath $fixture -Force
Assert ((Text (Join-Path $fixture 'scripts\feverscaler.ini')) -eq "[feverscaler]`nKeyMenu=Ctrl+F8`n") 'Upgrade overwrote preferences'
Assert ((Text (Join-Path $fixture 'feverscaler.log')) -eq 'existing diagnostics') 'Upgrade overwrote logs'
Assert ((Text (Join-Path $fixture 'scripts\another-mod.asi')) -eq 'another mod') 'Upgrade overwrote another mod'
foreach ($item in @('scripts\feverscaler.ini','feverscaler*.log*','RTX40MFG-Universal.status.json','<user data>\feverscaler\')) {
  Assert ($install.Contains($item)) ('Cleanup instructions do not mention '+$item)
}
Write-Output "Package checks passed for $version"
