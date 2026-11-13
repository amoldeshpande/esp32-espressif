# Somfy HTTP API

Use `http://<ESP32 LAN address>` after Wi-Fi setup. The LAN API uses no login or token. Send JSON bodies with `Content-Type: application/json`. An empty body means zero bytes. Browser POST requests with an `Origin` header must match the device origin.

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
| `POST /api/blinds/{blindId}/calibrate` | `{"openMs":18000,"closeMs":20000}` | `travel_times_saved` or `travel_times_save_failed` |
| `POST /api/groups/{remoteAddress}/commands` | `{"command":"close"}` | `group_transmitted`, `group_partially_transmitted`, or `group_not_transmitted` |
| `POST /api/groups/{remoteAddress}/rename` | `{"name":"All blinds"}` | `group_renamed` or `group_rename_failed` |
| `POST /api/groups/{remoteAddress}/delete` | Empty | `group_deleted` or `group_delete_failed` |

Use the six-digit hexadecimal physical remote address from discovery. Assign the same address to each blind in a group. Set `address` to an empty string to unlink a blind. Group commands send sequentially through the member blinds' virtual remotes. A group result summarizes completed bursts. A member whose burst did not complete may still have moved.

Group names are 1 to 32 bytes. Deleting a group clears its name and unlinks its blinds. If deletion fails partway, read the blind list and retry to unlink remaining members.

Removing an entry leaves the motor's pairing in place. The freed blind ID can be assigned to a new blind, so refresh the blind list after removal. Renaming preserves the blind's pairing and address.

Each blind includes `remoteAddress` (empty if unlinked), `groupName` (empty if unnamed), `openMs`, `closeMs`, and `position`. Travel times are full-run durations in integer milliseconds from 1 to 600000. A zero duration in the response means calibration is missing. Position is estimated as percent open from 0 to 100, or `null` if unknown. Run a full Open or Close after calibration or reboot to establish an endpoint estimate. A command heard from the linked physical remote invalidates the estimate. Movement the bridge does not hear can make the estimate inaccurate.

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
