# Asks for the game's files with three pickers, as LetsGoPikachu.exe does, and extracts them into
# orig\ with tools/prepare.sh (README, Setup step 2). tools\build_exe.cmd runs it when orig\ isn't
# ready. LETSGO_BASE, LETSGO_UPDATE and LETSGO_KEYS give the files without the pickers.
#
#   powershell -STA -ExecutionPolicy Bypass -File tools\prepare_dump.ps1 -Bash <Git Bash's bash.exe>
param([Parameter(Mandatory = $true)] [string] $Bash)
$ErrorActionPreference = 'Stop'
$game = "Pok$([char]0xE9)mon: Let's Go, Pikachu!"
$nsp = 'Switch packages (*.nsp)|*.nsp|All files (*.*)|*.*'
$files = @(
    @{ Env = 'LETSGO_BASE'; Title = "$game - the base game NSP, v1.0.0 (title ID 010003F003A34000)"; Filter = $nsp },
    @{ Env = 'LETSGO_UPDATE'; Title = "$game - the v1.0.2 update NSP (title ID 010003F003A34800)"; Filter = $nsp },
    @{ Env = 'LETSGO_KEYS'; Title = "$game - prod.keys from your Switch"; Filter = 'Keys (*.keys)|*.keys|All files (*.*)|*.*' }
)

$picked = @()
$folder = $null
$owner = $null
foreach ($file in $files) {
    $path = [Environment]::GetEnvironmentVariable($file.Env)
    if (-not $path) {
        if (-not $owner) {
            Write-Host "The build needs your copy of the game once. Pick the base game NSP (v1.0.0), its v1.0.2 update NSP and prod.keys."
            Add-Type -AssemblyName System.Windows.Forms
            $owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true }   # the pickers open in front
        }
        $dialog = New-Object System.Windows.Forms.OpenFileDialog -Property @{ Title = $file.Title; Filter = $file.Filter }
        if ($folder) { $dialog.InitialDirectory = $folder }
        if ($dialog.ShowDialog($owner) -ne [System.Windows.Forms.DialogResult]::OK) {
            Write-Host "No file was picked, so the game wasn't extracted. Run the build again to pick the files."
            exit 1
        }
        $path = $dialog.FileName
        $folder = Split-Path -Parent $path
    }
    $picked += $path
}

Write-Host "Extracting the game into orig\. This takes a few minutes."
& $Bash tools/prepare.sh $picked[0] $picked[1] $picked[2]
exit $LASTEXITCODE
