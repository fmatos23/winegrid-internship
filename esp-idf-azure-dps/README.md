# ESP-IDF + Azure IoT Hub Device Provisioning Service (DPS)

PoC for the Azure leg of the Watgrid device-management platform evaluation
(see `relatorio.tex`). Complements the ThingsBoard and ESP RainMaker PoCs
with actual hands-on testing of Azure DPS + IoT Hub, instead of the
documentation-only assessment in the original report.

## What this validates

A factory-fresh ESP32, with no per-device identity baked in, provisions
itself against Azure DPS using a **Symmetric Key Individual Enrollment**,
gets handed the IoT Hub + Device ID it was assigned to, caches that
assignment in NVS, and then publishes fake telemetry to that Hub -
conceptually the same "generic credential in -> per-device identity out"
flow already validated against ThingsBoard's device provisioning
(`esp-idf-thingsboard`), but split across Azure's two separate services
(DPS decides *where*, IoT Hub is *where* the device actually operates).

## Azure-side prerequisites

Already created for this PoC (Azure Portal, `Azure for Students`
subscription, resource group `winegrid-poc`, region `France Central`):

1. DPS instance `watgrid-dps`.
2. IoT Hub `watgrid-hub` (Free tier), linked to `watgrid-dps` under
   **Settings -> Linked IoT hubs**.
3. Individual Enrollment `esp32-winegrid` on `watgrid-dps`
   (**Settings -> Manage enrollments**), attestation mechanism
   **Symmetric Key**, target hub `watgrid-hub`.

## Configure the project

```
idf.py menuconfig
```

Under **Azure IoT DPS Configuration**, fill in:

- `AZURE_DPS_ID_SCOPE` - from `watgrid-dps` -> Overview -> ID Scope.
- `AZURE_DPS_REGISTRATION_ID` - must match the enrollment's Registration ID
  (`esp32-winegrid`).
- `AZURE_DPS_SYMMETRIC_KEY` - the enrollment's Primary Key.

Under **Example Connection Configuration**, set the Wi-Fi SSID/password.

These all land in `sdkconfig`, which is gitignored - never commit real
values (see `.gitignore` and `sdkconfig.defaults`, which only ship
`CHANGE_ME_*` placeholders).

## Build and flash

```
idf.py -p PORT flash monitor
```

## How the provisioning flow works (protocol level)

Unlike ThingsBoard's MQTT provisioning (single request, static
username/password), Azure's device-facing auth is a **locally-computed SAS
token**: an HMAC-SHA256 signature over the target resource URI + an expiry
timestamp, signed with the enrollment's Symmetric Key (decoded from
base64) and never sent over the wire itself. This is why the firmware
performs an SNTP time sync right after Wi-Fi comes up - without a real
clock, every token's expiry would already be in the past (device boots at
Unix epoch 1970), and Azure would reject it outright. This wasn't a
concern with ThingsBoard's static access-token auth.

1. **Register** - connect to the fixed global endpoint
   `global.azure-devices-provisioning.net:8883`, authenticate with a SAS
   token scoped to `{idScope}/registrations/{registrationId}`, publish an
   empty registration request.
2. **Poll** - Azure responds `202` (still assigning) while it allocates a
   hub; the firmware re-requests the operation status every ~2s until a
   terminal response arrives.
3. **Assigned** - the response's `registrationState` carries the
   `assignedHub` hostname and final `deviceId`. Both are cached in NVS so
   subsequent boots skip DPS entirely and connect straight to the Hub
   (same "provision once, reuse afterwards" pattern as the ThingsBoard
   access-token cache).
4. **Telemetry** - reconnect with a *new* SAS token, this time scoped to
   `{assignedHub}/devices/{deviceId}`, and publish to
   `devices/{deviceId}/messages/events/`.

## Direct Methods (RPC)

Equivalent to ThingsBoard's `getStatus`/`reboot` RPC methods
(`esp-idf-thingsboard`). Invoke from the Portal (**Devices ->
esp32-winegrid -> Direct method**):

- `getStatus` - returns `{"temperature":.., "freeHeap":..}`.
- `reboot` - acknowledges, then restarts the device.
- any other name - returns HTTP-style `404` with `{"error":"unknown method"}`.

## Device Twin (remote config)

Equivalent to ThingsBoard's `publishInterval` shared attribute. On
connect, the device requests the full twin (`$iothub/twin/GET`), applies
any already-set `desired.publishInterval`, and reports back its current
firmware version + effective interval under `properties.reported`. Edit
`desired.publishInterval` (milliseconds) from the Portal's Device Twin
editor to change the telemetry rate live, without a reflash - values
below 1000ms are rejected (logged, ignored) as a sanity floor, same as
the ThingsBoard PoC.

## Known scope limitations (intentional, for this PoC)

- No OTA - out of scope; `Device Update for IoT Hub` isn't available on
  the Free tier Hub used here.
- SAS tokens are generated with a fixed 1-hour validity and never
  refreshed - fine for a short test run, not for a long-lived deployment.
- The DPS operation-status poll interval is a fixed ~2s rather than
  parsed from Azure's `retry-after`, matching the SDKs' own default.
- Telemetry defaults to a 60s interval (`TELEMETRY_PUBLISH_INTERVAL_MS_DEFAULT`)
  - the original 10s demo interval alone exceeded the Free tier's
    8000 msgs/day quota.