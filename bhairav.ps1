# Bhairav OS developer helper for Windows. Everything builds inside Docker.
#
#   .\bhairav.ps1 toolchain   build the Docker toolchain image (first time only)
#   .\bhairav.ps1 build       build the kernel and build\bhairavos.iso (and disk.img,
#                             the FAT32 disk Bhairav OS saves files on, the first time)
#   .\bhairav.ps1 run         boot in a QEMU window (needs QEMU for Windows),
#                             otherwise boot in Docker with the serial console
#   .\bhairav.ps1 run-uefi    same, with UEFI firmware: boots through ShoolLoader
#   .\bhairav.ps1 shot        boot headless, save build\screen.png + serial.log
#   .\bhairav.ps1 clean       delete build\
#   .\bhairav.ps1 shell       open a shell inside the toolchain container
#   .\bhairav.ps1 serve-repo  serve an apt repository of apps\ on port 8080
#                             (in Bhairav OS: apt add-repo http://10.0.2.2:8080)

param([string]$Command = "build")

$ErrorActionPreference = "Stop"
$Image = "bhairav-toolchain"
$Root = $PSScriptRoot

function Invoke-Docker([string[]]$Arguments) {
    # Docker and the build tools print progress on stderr; only the exit code
    # means failure (Windows PowerShell would otherwise stop on the first line).
    $ErrorActionPreference = "Continue"
    & docker @Arguments 2>&1 | ForEach-Object {
        if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { $_ }
    }
    if ($LASTEXITCODE -ne 0) { throw "docker exited with code $LASTEXITCODE" }
}

function Invoke-Toolchain([string[]]$Arguments, [switch]$Interactive) {
    $dockerArgs = @("run", "--rm")
    if ($Interactive) {
        # Interactive sessions need a real terminal, so no output filtering.
        & docker @($dockerArgs + @("-it", "-v", "${Root}:/src", "-w", "/src", $Image) + $Arguments)
        return
    }
    Invoke-Docker ($dockerArgs + @("-v", "${Root}:/src", "-w", "/src", $Image) + $Arguments)
}

switch ($Command) {
    "toolchain" { Invoke-Docker @("build", "-t", $Image, $Root) }
    "build"     { Invoke-Toolchain @("make", "-j8") }
    "shot"      { Invoke-Toolchain @("make", "shot") }
    "clean"     { Invoke-Toolchain @("make", "clean") }
    "shell"     { Invoke-Toolchain @("bash") -Interactive }
    "serve-repo" {
        # The apt repository of apps\ on port 8080 of this PC: Bhairav OS in
        # QEMU reaches it with: apt add-repo http://10.0.2.2:8080
        Invoke-Toolchain @("make", "-j8", "repo")
        Write-Host "Serving build\repo on port 8080 (Ctrl+C stops it)."
        & docker run --rm -it -p 8080:8080 -v "${Root}:/src" -w /src $Image make serve-repo
    }
    { $_ -in "run", "run-uefi" } {
        $uefi = $Command -eq "run-uefi"
        Invoke-Toolchain @("make", "-j8")
        $qemu = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
        if ($qemu) { $qemu = $qemu.Source }
        else {
            $default = "C:\Program Files\qemu\qemu-system-x86_64.exe"
            if (Test-Path $default) { $qemu = $default }
        }
        if ($qemu) {
            $qemuArgs = @("-M", "q35", "-m", "256M", "-cdrom", "$Root\build\bhairavos.iso", "-boot", "d", "-serial", "stdio",
                          "-drive", "file=$Root\disk.img,format=raw,if=none,id=disk", "-device", "ide-hd,drive=disk,bus=ide.0",
                          "-netdev", "user,id=net", "-device", "e1000,netdev=net")
            if ($uefi) {
                # QEMU for Windows ships UEFI firmware (EDK2) next to the program.
                $firmware = Join-Path (Split-Path $qemu) "share\edk2-x86_64-code.fd"
                if (-not (Test-Path $firmware)) { throw "UEFI firmware not found at $firmware" }
                $qemuArgs += @("-drive", "if=pflash,format=raw,readonly=on,file=$firmware")
            }
            & $qemu @qemuArgs
        } else {
            Write-Host "QEMU for Windows not found, so booting in Docker with the serial console."
            Write-Host "To see the graphical screen: winget install SoftwareFreedomConservancy.QEMU"
            Write-Host "Quit with Ctrl+A then X."
            Invoke-Toolchain @("make", $(if ($uefi) { "run-uefi" } else { "run" })) -Interactive
        }
    }
    default { throw "Unknown command '$Command'. Use: toolchain, build, run, run-uefi, shot, clean, shell, serve-repo" }
}
