# Test on Windows

Build and flash the firmware using the [Windows build guide](BUILD-WINDOWS.md). Run the PowerShell examples from the repository root.

## Wire the radio and verify reception

The receiver uses GPIO18 for SCK, GPIO19 for MISO, GPIO23 for MOSI, GPIO5 for CSN, and GPIO14 for the CC1101's GDO2. Power the CC1101 from 3.3 V and connect the grounds.

The receiver starts at 433.42 MHz with a 270.83 kHz receive filter and zero frequency offset. The monitor prints the register readback, RX state, and checksummed Somfy frames. Keys **1–4** select the receiver profiles shown at startup.

## Pair and control one RTS blind

The serial keys control blind 1 using standard 56-bit RTS transmission. A previously saved bench remote migrates to blind 1 with its existing address and next rolling code.

Connect **ESP32 GPIO13 to CC1101 GDO0** for TX. Keep **GPIO14 connected to GDO2** for RX. GDO0 is left high impedance by the CC1101 during reception so it does not fight the ESP32 output.

The monitor prints the new virtual remote address and next rolling code at startup. The address is independent of the physical remote detected by the receiver. Keep profile **1** selected for transmission; profiles 2�4 remain receive diagnostics.

1. Put the intended blind into programming mode with its already paired remote, following the motor's instructions (normally hold that remote's PROG button until the blind jogs).
2. Press **p** once in the serial monitor to send the virtual remote's short programming command. After the blind jogs in response, press **y** to confirm pairing. A second programming command can remove a remote that was just paired.
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


## Pair and test another blind in the browser

Open the ESP32's LAN address after Wi-Fi setup. Under **Blinds**, enter a name and select **Add blind**. For an existing unpaired blind, use its card directly.

1. Use the physical remote to put only the intended blind into programming mode. Select **Send pairing signal** on that blind's card.
2. If the motor jogs in response, select **Confirm motor jog**. Check the motor before retrying a pairing signal.
3. Use **Open**, **Close**, and **My / Stop** on each card. Confirm that only the selected blind responds.
4. Reboot the ESP32, reload the page, and test both blinds again.

## Add and pair another blind over HTTP

The registry stores up to eight blinds in NVS. The serial keys control blind 1. The web page can pair and control each saved blind. Use the LAN address from Wi-Fi setup and select radio profile **1**.

| Request | Body | Completed result |
| --- | --- | --- |
| `POST /api/blinds` | `{"name":"Kitchen"}` | `blind_created` with the new `blindId`, or `blind_create_failed` |
| `POST /api/blinds/{blindId}/pair` | Empty | The radio result for the short RTS programming command |
| `POST /api/blinds/{blindId}/confirm` | Empty | `pairing_confirmed` or `confirmation_rejected` |
| `POST /api/blinds/{blindId}/commands` | `{"command":"open"}` | The radio result for the specified blind |

Each POST returns a request ID. Poll `GET /api/requests/{requestId}` for its result. `GET /api/blinds` lists the name, ID, address, pairing state, and readiness of each saved blind.

1. Create the second blind and record the returned `blindId`.
2. Put only the intended motor into programming mode with its existing remote. Send the `pair` request once and watch for the motor to jog in response.
3. Send `confirm` after observing that jog. A transmitted result alone does not confirm pairing. If transmission was uncertain, check the motor before sending another programming command.
4. Send commands separately to IDs 1 and 2. Reboot the ESP32 and repeat the test to check that the identities, pairing states, and rolling codes persisted.

The new registry takes over rolling-code storage after migration. Earlier firmware reads the old bench counter, which will be stale after commands from this build.

## Create a group from remote discovery

Start a scan or choose **Discover remote for this blind** on a saved blind.
Press a movement button on the physical remote channel. The first detected
remote opens the group editor. If several addresses appear, choose the one
you want. Select the saved blinds you saw move and click **Save group**.
Existing members and the blind that started the scan are preselected.
Use **Edit members** to change the group later. Existing pairings are reused.
