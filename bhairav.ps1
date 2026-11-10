# BhairavOS developer helper for Windows. Everything builds inside Docker.
#
#   .\bhairav.ps1 toolchain   build the Docker toolchain image (first time only)
#   .\bhairav.ps1 build       build the kernel and build\bhairavos.iso
#   .\bhairav.ps1 run         boot in a QEMU window (needs QEMU for Windows),
#                             otherwise boot in Docker with the serial console
#   .\bhairav.ps1 shot        boot headless, save build\screen.png + serial.log
#   .\bhairav.ps1 clean       delete build\
#   .\bhairav.ps1 shell       open a shell inside the toolchain container

param([string]$Command = "build")

$ErrorActionPreference = "Stop"
$Image = "bhairav-toolchain"
$Root = $PSScriptRoot

function Invoke-Toolchain([string[]]$Arguments, [switch]$Interactive) {
    $dockerArgs = @("run", "--rm")
    if ($Interactive) { $dockerArgs += "-it" }
    $dockerArgs += @("-v", "${Root}:/src", "-w", "/src", $Image) + $Arguments
    # Tools print progress on stderr; only the exit code means failure.
    $ErrorActionPreference = "Continue"
    & docker @dockerArgs 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { $_ }
    }
    if ($LASTEXITCODE -ne 0) { throw "docker exited with code $LASTEXITCODE" }
}

switch ($Command) {
    "toolchain" { & docker build -t $Image $Root }
    "build"     { Invoke-Toolchain @("make", "-j8") }
    "shot"      { Invoke-Toolchain @("make", "shot") }
    "clean"     { Invoke-Toolchain @("make", "clean") }
    "shell"     { Invoke-Toolchain @("bash") -Interactive }
    "run" {
        Invoke-Toolchain @("make", "-j8")
        $qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
        if (-not $qemu) {
            $default = "C:\Program Files\qemu\qemu-system-x86_64.exe"
            if (Test-Path $default) { $qemu = $default }
        }
        if ($qemu) {
            & $qemu -M q35 -m 256M -cdrom "$Root\build\bhairavos.iso" -boot d -serial stdio
        } else {
            Write-Host "QEMU for Windows not found, so booting in Docker with the serial console."
            Write-Host "To see the graphical screen: winget install SoftwareFreedomConservancy.QEMU"
            Write-Host "Quit with Ctrl+A then X."
            Invoke-Toolchain @("make", "run") -Interactive
        }
    }
    default { throw "Unknown command '$Command'. Use: toolchain, build, run, shot, clean, shell" }
}
