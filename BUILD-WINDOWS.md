# Build and test on Windows

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

Use the board's actual COM port in place of `COM4`. The receiver uses GPIO18 for SCK, GPIO19 for MISO, GPIO23 for MOSI, GPIO5 for CSN, and GPIO14 for the CC1101's GDO2. Power the CC1101 from 3.3 V and connect the grounds.

```powershell
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Probe -Port COM4
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Backup -Port COM4
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Flash -Port COM4
powershell.exe -NoProfile -File .\scripts\Somfy-Bench.ps1 -Action Monitor -Port COM4
```

`Probe` reads the flash ID. `Backup` reads the whole flash into the output directory and prints its SHA-256 hash. `Flash` writes the selected firmware, so complete the backup first if you need to preserve the board's current contents. If you built with `-Clang`, use the same flag for `Flash` and `Monitor` to select that build.

If esptool reports the wrong boot mode, hold **BOOT**, tap **EN**, then release **BOOT** once esptool identifies the chip. Exit the serial monitor with **Ctrl+]** before flashing again.

The receiver starts at 433.42 MHz with a 270.83 kHz receive filter and zero frequency offset. The monitor prints the register readback, RX state, and checksummed Somfy frames. Keys **1–4** select the receiver profiles shown at startup.

## Pair and control one RTS blind

The firmware uses a persistent virtual remote and standard 56-bit RTS transmission. Pair the first blind through the serial monitor.

Connect **ESP32 GPIO13 to CC1101 GDO0** for TX. Keep **GPIO14 connected to GDO2** for RX. GDO0 is left high impedance by the CC1101 during reception so it does not fight the ESP32 output.

The monitor prints the new virtual remote address and next rolling code at startup. The address is independent of the physical remote detected by the receiver. Keep profile **1** selected for transmission; profiles 2�4 remain receive diagnostics.

1. Put the intended blind into programming mode with its already paired remote, following the motor's instructions (normally hold that remote's PROG button until the blind jogs).
2. Press **p** once in the serial monitor to send the virtual remote's short programming command. Confirm the blind jogs. Programming is a toggle: sending it again while the motor is in programming mode can remove this remote.
3. Use **o** to open, **c** to close, and **m** for My/stop. My stops a moving blind or requests its stored favorite position when idle; this 56-bit command does not distinguish those two actions.
4. Press **i** to print the virtual remote address and next rolling code. Restart the ESP32 and check that the address is unchanged and commands still work.

Each button press sends an initial frame plus two repeats with the same rolling code. A successful log means transmission completed; RTS does not acknowledge movement. Confirm movement on the blind.

The address and next unused rolling code are stored in NVS. The next code is committed before any transmission, including pairing. A failed transmission consumes its reserved code. Storage errors disable TX until restart rather than risk reusing a code. Do not erase NVS between builds if you want to retain the paired virtual remote. The firmware does not automatically erase NVS on initialization errors and refuses to wrap an exhausted rolling counter.


## Set up Wi-Fi and test HTTP control

1. Flash with Somfy-Bench. On first boot, join the open Wi-Fi network named **Somfy-Setup-XXXXXX**. The suffix is the virtual remote address.
2. Open **http://192.168.4.1/** and enter your home Wi-Fi SSID and password. Leave the password blank for an open home network.
3. When the page shows the LAN address, switch back to your home Wi-Fi and open that address. The setup access point closes shortly after a successful join.

The ESP32 saves the Wi-Fi credentials after receiving a LAN IP address. It reopens setup if joining the saved network fails. From the LAN page, **Reopen Wi-Fi setup** displays the setup network name and switches the ESP32 back to setup mode. The setup access point serves Wi-Fi configuration while the blind controls are available on the LAN.

The LAN page and HTTP API use no login or token. Use profile **1** for control of the existing paired blind.

The HTTP build uses the ESP-IDF 1500 KiB factory app partition to fit Wi-Fi. Flash the full build with Somfy-Bench so the partition table is included. The NVS partition keeps its existing offset and size, preserving the virtual remote and rolling code.

```powershell
$base = 'http://192.168.1.50' # Replace with the LAN address shown after setup.
Invoke-RestMethod "$base/api/blinds"
Invoke-RestMethod "$base/api/discovery" -Method Post
```

After starting discovery, press a button on the physical remote within 30 seconds. Read the results with:

```powershell
Invoke-RestMethod "$base/api/discovery"
```

Discovery records up to 16 distinct remote addresses and keeps the latest decoded frame for each address. Results remain available until the next scan. An address identifies a transmitter, which may operate multiple blinds.

Send a command to the existing paired blind:

```powershell
$accepted = Invoke-RestMethod "$base/api/blinds/1/commands" -Method Post -ContentType 'application/json' -Body '{"command":"open"}'
$accepted.requestId
Invoke-RestMethod "$base/api/requests/$($accepted.requestId)"
```

Commands are `open`, `close`, `my`, and `stop`. Both `my` and `stop` send the RTS My command. HTTP 202 means the request entered the queue for the radio task. Poll `GET /api/requests/{requestId}` using the accepted ID. The result is `pending` until the radio task finishes. The ESP32 keeps the last 16 completed requests in RAM, including discovery requests. An expired result returns HTTP 410 and an unknown ID returns HTTP 404. IDs and results reset on reboot. `GET /api/requests/latest` still returns the most recent completion. A full queue returns HTTP 503. A result of `transmitted` means the radio burst completed. Confirm the blind's movement separately.
