# Build and flash on Windows

These scripts build the ESP-IDF receiver for the original ESP32, including the HiLetGo ESP32-WROOM. Use Windows x64, Git for Windows on your PATH, and a repository path without spaces. Run the examples in PowerShell from the repository root.

## Install the tools and build

```powershell
powershell.exe -NoProfile -File .\scripts\Install-SomfyTools.ps1
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Build
```

The installer downloads portable Python 3.12, ESP-IDF v5.5.2, its Python dependencies, and the Xtensa tools. It keeps the downloads and installation under `build/windows/toolchain`. The build writes the app, bootloader, partition table, and flash manifest to `build/windows/build-tuned`. Git ignores the `build/` directory.

For the Espressif Clang build used during the bench test, pass `-Clang` to both scripts. ESP-IDF 5.5.2 treats Clang support as experimental. The Clang binaries go to `build/windows/build-clang`.

```powershell
powershell.exe -NoProfile -File .\scripts\Install-SomfyTools.ps1 -Clang
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Build -Clang
```

Both scripts accept `-OutputRoot C:\path\to\output`. Pass the same location to each. When copying the repository to another machine or location, run setup and build again there. CMake caches and Python virtual environments contain absolute paths.

## Flash and monitor

Use the board's actual COM port in place of `COM4`.

```powershell
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Probe -Port COM4
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Backup -Port COM4
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Flash -Port COM4
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Monitor -Port COM4
```

`Probe` reads the flash ID. `Backup` reads the whole flash into the output directory and prints its SHA-256 hash. `Flash` writes the selected firmware, so complete the backup first if you need to preserve the board's current contents. If you built with `-Clang`, use the same flag for `Flash` and `Monitor` to select that build.

If esptool reports the wrong boot mode, hold **BOOT**, tap **EN**, then release **BOOT** once esptool identifies the chip. Exit the serial monitor with **Ctrl+]** before flashing again.

For wiring and hardware checks, see [Test on Windows](TEST-WINDOWS.md).
