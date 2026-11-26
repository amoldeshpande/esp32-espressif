# Somfy HTTP API

Use `http://<ESP32 LAN address>` after Wi-Fi setup. The LAN API uses no login or token. Send JSON bodies with `Content-Type: application/json`. An empty body means zero bytes. Browser POST requests with an `Origin` header must match the device origin.

## Radio availability

`GET /api/radio` returns `{"ready":true,"error":null}` when radio
initialization succeeds, or `{"ready":false,"error":"radio_unavailable"}`
when it fails. The bridge keeps Wi-Fi and HTTP available when the
CC1101 is missing or does not initialize. Saved configuration, backup,
restore, and firmware updates remain accessible.

When unavailable, movement commands, pairing signals, position moves,
motor My programming, group commands, and starting discovery return
HTTP 503 with `{"error":"radio_unavailable"}`. No request ID is
allocated. Saved blinds report `ready:false`. Correct the radio wiring
with power disconnected, then restart the bridge to initialize it again.

## Blinds and discovery

| Method and path | Body | Result |
| --- | --- | --- |
| `GET /api/blinds` | Empty | Array of saved blinds |
| `POST /api/blinds` | `{"name":"Kitchen"}` | Queues creation of an unpaired blind |
| `POST /api/blinds/{blindId}/pair` | Empty | Queues an RTS programming signal |
| `POST /api/blinds/{blindId}/confirm` | Empty | Queues confirmation of pairing |
| `POST /api/blinds/{blindId}/commands` | `{"command":"open"}` | Queues a movement command for a paired blind |
| `POST /api/discovery` | Empty | Queues a new 30-second scan |
| `GET /api/discovery` | Empty | Current scan status and observed remotes |

A blind record looks like this:

```json
{"id":2,"name":"Kitchen","address":"97E360","pairing":"paired","ready":true}
```

The registry holds up to eight blinds. Names must be 1 to 32 bytes. IDs are numeric and persist across reboot while the blind is saved. The address is the six-digit hexadecimal address of the ESP32's virtual remote. Pairing is `unpaired`, `awaiting_confirmation`, or `paired`. Check `ready` before sending movement commands.

Commands are `open`, `close`, `my`, and `stop`. Both `my` and `stop` send RTS My, which stops a moving blind or requests its stored favorite position when idle. Radio transmission requires profile 1.

To pair a blind, create it and read its `blindId` from the completed request. Use the existing physical remote to put the intended motor into programming mode, then send `pair` once. Call `confirm` after observing the motor jog in response. Check the motor before retrying a programming command.

Discovery returns:

```json
{"active":true,"overflow":false,"remotes":[{"address":"ED673D","command":4,"rollingCode":428,"bits":56}]}
```

Press a physical remote button during the scan. The list keeps the latest decoded frame for each of up to 16 addresses. Results remain after the scan ends and are cleared when the next scan starts. An observed address identifies a transmitter, which may control multiple blinds.

## Manage blinds and remote groups

| Method and path | Body | Completed results |
| --- | --- | --- |
| `POST /api/blinds/{blindId}/rename` | `{"name":"Kitchen east"}` | `blind_renamed` or `rename_failed` |
| `POST /api/blinds/{blindId}/remove` | Empty | `blind_removed` or `remove_failed` |
| `POST /api/blinds/{blindId}/remote` | `{"address":"ED673D"}` | `remote_saved` or `remote_save_failed` |
| `POST /api/blinds/{blindId}/link-remote` | `{"address":"F0673D"}` | `physical_remotes_saved`, `physical_remote_limit`, or `physical_remote_save_failed` |
| `POST /api/blinds/{blindId}/unlink-remote` | `{"address":"F0673D"}` | `physical_remotes_saved` or `physical_remote_save_failed` |
| `POST /api/blinds/{blindId}/individual-remote` | `{"address":"F0673D"}` | Legacy first-slot setter |
| `POST /api/blinds/{blindId}/calibrate` | `{"openMs":18000,"closeMs":20000}` | `travel_times_saved` or `travel_times_save_failed` |
| `POST /api/groups/{remoteAddress}/commands` | `{"command":"close"}` | `group_transmitted`, `group_partially_transmitted`, or `group_not_transmitted` |
| `POST /api/groups/{remoteAddress}/rename` | `{"name":"All blinds"}` | `group_renamed` or `group_rename_failed` |
| `POST /api/groups/{remoteAddress}/delete` | Empty | `group_deleted` or `group_delete_failed` |
| `POST /api/groups/{remoteAddress}/link-channel` | `{"address":"F0673D"}` | `group_channels_saved`, `group_channel_limit`, or `group_channel_save_failed` |
| `POST /api/groups/{remoteAddress}/unlink-channel` | `{"address":"F0673D"}` | `group_channels_saved` or `group_channel_save_failed` |

Use the six-digit hexadecimal physical remote address from discovery. Assign the same address to each blind in a group. Set `address` to an empty string to unlink a blind. Group commands send sequentially through the member blinds' virtual remotes. The primary address identifies the group in the API. Each group can link up to four additional physical channels. `GET /api/blinds` lists the primary and additional channels in `groupChannels`. Commands heard from any channel in that list update the estimates of all members. Linking a channel on the bridge assumes it already controls those motors. A group result summarizes completed bursts. A member whose burst did not complete may still have moved.

Group names are 1 to 32 bytes. Deleting a group clears its name and unlinks its blinds. If deletion fails partway, read the blind list and retry to unlink remaining members.

Removing an entry leaves the motor's pairing in place. The freed blind ID can be assigned to a new blind, so refresh the blind list after removal. Renaming preserves the blind's pairing and address.

Each blind includes `remoteAddress` (empty if unlinked), `groupName` (empty if unnamed), `openMs`, `closeMs`, and `position`. Travel times are full-run durations in integer milliseconds from 1 to 600000. A zero duration in the response means calibration is missing. Position is estimated as percent open from 0 to 100, or `null` if unknown. Run a full Open or Close after calibration or reboot to establish an endpoint estimate. The bridge tracks Open, Close, and My/Stop heard from the linked physical remote, including a channel shared by several blinds. A blind can track its group channel (`remoteAddress`) and up to four other physical channels (`individualAddresses`). Link and unlink requests use a six-digit hexadecimal address. Other channels do not change group membership. The legacy `individualAddress` field and `individual-remote` endpoint still read and write the first individual slot. My while idle tracks the configured favorite when one is set. My without a configured favorite and unsupported commands clear the estimate. Movement the bridge does not hear can make the estimate inaccurate.

## Move to a position

Send `POST /api/blinds/{blindId}/position` with `{"position":50}`, using a whole percentage from 0 (closed) to 100 (open). Targets of 0 and 100 send full Close or Open. Intermediate targets require calibrated opening and closing times and a known estimate. Run a full Open or Close to establish an estimate after reboot or calibration.

Poll the request ID for `position_started`, `position_unchanged`, `position_busy`, `position_unavailable`, `position_too_close`, or `position_not_started`. The started result means the movement command was sent. Read `GET /api/blinds` for the estimate and the active `targetPosition`, which is null when there is no active timed move or tracked move to a My favorite. RTS provides no position feedback.

Only one timed move runs at a time. The bridge reserves the radio for that move until it sends My/Stop or the linked physical remote is heard. My/Stop on the moving blind can end the move early. Moves shorter than 500 ms are rejected. If the scheduled stop fails, the position becomes unknown.

## Track a request

Queued operations return HTTP `202 Accepted` with `{"requestId":42}`. Poll `GET /api/requests/42` with that ID. A successful lookup returns HTTP 200 and the request ID and result.

| Result | Meaning |
| --- | --- |
| `pending` | The request is queued or running |
| `blind_created` | The blind was saved. The response also contains `blindId` |
| `blind_create_failed` | The blind could not be saved |
| `discovery_started` | The discovery window opened |
| `pairing_confirmed` | Pairing confirmation was saved |
| `confirmation_rejected` | Pairing confirmation could not be saved |
| `not_sent` | The operation was skipped or the radio did not transmit |
| `transmitted` | The radio burst completed. RTS provides no motor acknowledgement |
| `transmission_uncertain` | A frame was queued, but the full burst did not complete |
| `transmitted_with_cleanup_error` | The burst completed, but restoring the radio failed |

`GET /api/requests/latest` returns the latest completion or JSON `null`. Use the ID lookup for concurrent clients. The ESP32 retains the last 16 completed requests in RAM. IDs and results reset on reboot. If a result is lost during a reboot or timeout, check the blind before retrying, especially for pairing or My.

Errors normally have the shape `{"error":"radio_queue_full"}`. HTTP 400 indicates invalid input, 403 indicates a cross-origin POST, 404 indicates an unknown path, blind, or request, and 410 indicates an expired request result. HTTP 503 can indicate a full queue or Wi-Fi setup in progress.

## Wi-Fi setup

During setup, join the open `Somfy-Setup-XXXXXX` network and use `http://192.168.4.1`. Blind, discovery, and request endpoints require the device to be online on the LAN.

| Method and path | Body | Result |
| --- | --- | --- |
| `GET /api/network` | Empty | Network mode, error, setup SSID, and address |
| `POST /api/network/setup` | Empty | From the LAN, requests reopening the setup network |
| `POST /api/network/join` | `{"ssid":"Home","password":"example-password"}` | From setup mode, requests joining a Wi-Fi network |

The two Wi-Fi POSTs return HTTP 202 with `{"accepted":true}`. Poll `GET /api/network` for progress instead of looking up a request ID. A state conflict returns HTTP 409.

The network response has fields `mode`, `error`, `setupSsid`, and `address`. Modes are `setup`, `joining`, `joined`, `online`, `reopening`, and `unavailable`. The error is empty or one of `join_failed`, `save_failed`, and `setup_failed`. At `joined`, use `address` to find the device on the LAN. The setup network closes shortly afterward.


## Update firmware

On the LAN, send `POST /api/firmware` with the raw `esp32-espressif.bin`
application bytes, `Content-Type: application/octet-stream`, and a
`Content-Length`. The one-time USB migration must have installed the OTA
partition table. Use an image from this project that fits the inactive slot.

A successful upload returns HTTP 202 with `{"result":"rebooting"}`. This
endpoint does not create a request ID. The bridge pauses radio operations while
writing and waits for any timed blind move to finish before granting access.
A rejected or interrupted upload leaves the current boot slot selected.

On the LAN, `GET /api/firmware` returns `version`, `project`, and `build`.
The build is the full ELF SHA-256 embedded in the running image. The web page
shows its first 12 digits. Set the release version in `version.txt` before
building a new release.

## Blind groups

A blind group is a named set of saved blinds. A blind can belong to
multiple blind groups and keep its physical remote group assignment.

| Request | JSON body | Result |
| --- | --- | --- |
| `GET /api/blind-groups` | Empty | Array of `{"id":1,"name":"Evening","blindIds":[1,3]}` |
| `POST /api/blind-groups` | `{"name":"Evening","blindIds":[1,3]}` | `blind_group_saved` (includes `groupId`) or `blind_group_save_failed` |
| `POST /api/blind-groups/{groupId}/save` | `{"name":"Evening","blindIds":[1,2,3]}` | `blind_group_saved` (includes `groupId`) or `blind_group_save_failed` |
| `POST /api/blind-groups/{groupId}/commands` | `{"command":"close"}` | `group_transmitted`, `group_partially_transmitted`, or `group_not_transmitted` |
| `POST /api/blind-groups/{groupId}/delete` | Empty | `blind_group_deleted` or `blind_group_delete_failed` |

Up to eight blind groups can be saved. A group name is 1 to 32 bytes.
Saving requires at least one distinct saved blind ID. If a blind is
removed from the bridge, it drops out of the effective membership.
Group commands send through each member's paired virtual remote in
blind ID order. A member whose command could not be sent does not
prevent attempts on the other members.

## My favorite position

`GET /api/blinds` includes `favoritePosition`, a whole percent open or
`null` if unset. Send `POST /api/blinds/{blindId}/favorite` with
`{"position":40}` to record an existing motor favorite, or
`{"position":null}` to clear the bridge setting. Poll for
`favorite_saved` or `favorite_save_failed`.

With calibrated travel times, the bridge estimates movement to the
favorite when it sends My or receives My from the linked physical
remote while the blind is idle. The active target is visible even when
the starting position is unknown. In that case the estimate stays
unknown until the longest possible travel time to the favorite has
elapsed. My during tracked movement stops it.
This setting only records the bridge's estimate.

To program the motor, first stop the blind at a new, known position
away from its existing My position. Programming at the existing
favorite can erase it. Send an empty `POST /api/blinds/{blindId}/favorite-program`
and poll the request ID, allowing up to 15 seconds for the long My
burst and queueing. The result is `favorite_jog_pending`,
`favorite_program_busy`, `favorite_program_unavailable`,
`favorite_already_recorded`, or `favorite_program_not_sent`.
Repeating the request while a jog is pending returns
`favorite_jog_pending` without sending another burst.

After observing the motor jog, send an empty
`POST /api/blinds/{blindId}/favorite-confirm` to save the captured
position. If you did not see a jog, send an empty
`POST /api/blinds/{blindId}/favorite-discard` to keep the previous
bridge setting. Poll for `favorite_saved`, `favorite_save_failed`,
`favorite_program_discarded`, or `favorite_no_pending_jog`.

## Back up and restore blinds

`GET /api/backup` downloads a versioned binary `somfy-backup.esb` file.
It contains saved blinds, pairing records, virtual remote addresses and
rolling codes, group names and membership, calibration, and My favorites.
Backups also include individual and group channels and saved blind groups. Older backups restore with no saved blind groups. Backups from versions before 0.9.0 restore with only the primary group channel. Older backups restore with individual channels unset, and backups created before favorites were supported restore with favorites unset. Wi-Fi
credentials and position estimates are excluded.

On the LAN, send the raw file to `POST /api/backup` with a fixed
`Content-Length` and `Content-Type: application/octet-stream`. Poll the
returned request ID. Results are `backup_restored`, `invalid_backup`,
`backup_identity_conflict`, `position_busy`, or `restore_write_failed`.

Restore preserves the higher rolling code for matching identities. An
existing identity that has sent a command must match the backup at the
same blind ID. Restore onto a fresh bridge if identities conflict.
An interrupted flash write can be recovered on restart. After restore,
position is unknown until an endpoint is established again. Retire the
old bridge before controlling motors with the replacement. An older backup
on a fresh bridge may have rolling codes the motors have already passed.
