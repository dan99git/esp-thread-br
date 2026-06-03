# Building OS Border Router Firmware

Building OS fork of the Espressif Thread Border Router firmware. Targets the ESP Thread BR-Zigbee GW main board plus the Sub-Ethernet daughter board.

Reference spec: the outer Building OS repository `docs/08.8-border-router.md`.
That document has been reconciled for the current BOS fork direction, but it
still separates implemented source from target production behavior.

## Status

Bench-flashed and network-proven on 2026-06-02 with an earlier two-port build. Current source targets a single port 80 BR surface: it bundles the H2 RCP image, brings up the W5500 Ethernet backbone, serves the BOS-branded BR UI on port 80, and registers Building OS `/bos/*` JSON routes on the same port. Port 7086 is deprecated in source and should not be used for new verification.

Latest bench recovery handover: `docs/scratch/br-handover-07-current-state-and-next-work-2026-06-03.md`.

Short-term target: restore and prove the port-80 UI from this firmware, then
show the bare black C6/XIAO peer from `examples/bos_c6_tmfs_test` in the
Ledger Torrent UI and `/bos/ledger/torrent`. Do not use production
`firmware/xiao-esp32c6` for that peer test.

In this BR fork repository, the bare C6/XIAO peer lives at
`examples/bos_c6_tmfs_test`.

The Building OS components are still partial. Diagnostics, protected ledger push, active-ledger metadata, BR seed CoAP manifest/chunk/have resources, a torrent-style BR ledger snapshot, SRP/TMFS peer discovery, XIAO BR-to-leaf CoAP ledger pull and activation, XIAO staging/active TXT progress publication, firmware convergence posting, and site-server latest convergence storage are present in source and have built or passed host tests. Multi-peer TMFS swarm scheduling, hardware verification of peer rows, and Thread/RCP live polling are not complete.

This repository is the Building OS fork of `esp-thread-br`. The BR app firmware
and bench peer firmware live in this repo so BR software stays separate from the
outer Building OS monorepo.

## Layout

```
examples/building_os_border_router/
  CMakeLists.txt              top-level ESP-IDF project, references this fork repo
  sdkconfig.defaults          pins W5500 GPIOs and Thread BR settings
  partitions.csv              standard ESP32-S3 layout plus ledger_a and ledger_b
  main/
    app_main.c                wires the upstream stack and the bos_* components
  components/
    bos_diagnostics_server/   Building OS status and ledger metadata routes on BR port 80
    bos_server_registration/  site server discovery, registration, heartbeat
    bos_ledger_ingress/       LAN HTTP endpoints under /bos/
    bos_ledger_mesh_serve/    Thread CoAP resources under /mesh/ledger/
    bos_convergence_aggregator/   SRP browse, peer table, JSON view
    bos_thread_dataset_anchor/    Thread operational dataset persistence
```

## Build prerequisites

1. ESP-IDF v5.5.2 installed and exported to the shell.
2. This repository cloned from `git@github.com:dan99git/esp-thread-br.git`.
3. Python 3 with `cbor2`, `pyyaml`, `regex` for shared ledger tooling.

## Build

```
cd examples/building_os_border_router
idf.py set-target esp32s3
idf.py build
```

The H2 RCP firmware is bundled inside the S3 image by the upstream and flashed automatically on first boot. No separate H2 flash step.

## Flash

```
cd examples/building_os_border_router
idf.py -p COM11 flash
```

COM11 was the S3 USB-serial port on the 2026-06-02 bench board. Use the detected S3 port for other boards.

## Bench verification, 2026-06-02

For the current 2026-06-03 bench recovery, prefer the latest handover above.
The original 2026-06-02 addresses below are retained as old proof points and
may not match the current DHCP lease.

Current board:

- S3 MAC: `9c:13:9e:0a:47:44`
- W5500 Ethernet MAC: `9c:13:9e:0a:47:47`
- H2 RCP target: `esp32h2`
- BOS BR UI and diagnostics: `http://[fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747]/`
- Link-local fallback for tools that support scoped IPv6 literals: `http://[fe80::9e13:9eff:fe0a:4747%25Ethernet]/`
- Firmware-reported Ethernet IPv4: `192.168.88.254`
- Firmware-reported Ethernet IPv6 ULA: `fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747`

After flashing the current single-port source, verify from the Windows host:

```
ping -6 -n 3 fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747
curl.exe -s -g "http://[fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747]/"
curl.exe -s -g "http://[fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747]/static/style.css"
curl.exe -s -g "http://[fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747]/bos/status"
curl.exe -s -g "http://[fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747]/bos/ledger/active"
curl.exe -s -g "http://[fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747]/bos/ledger/torrent"
curl.exe -s -g "http://[fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747]/bos/convergence"
curl.exe -s -g "http://[fd9f:5e0b:cb5c:00f6:9e13:9eff:fe0a:4747]/bos/peers"
```

If `/` renders as browser-default HTML, check `/static/style.css`. The current
source serves CSS from the `web_storage` SPIFFS image. A stale or missing CSS
route on hardware normally means the board was flashed app-only or with an old
`web_storage.bin`; re-flash the full project or write the `web_storage`
partition at `0x420000`.

Observed `/bos/status` payload reports:

- `device_class`: `border_router`
- `firmware`: `building-os-border-router`
- `device_id`: BR claim/status/convergence device id, or `null` before it can be read
- `site_server_url`: configured site-server URL used by the BR registration component, or `null`
- `backbone.connected`: `true`
- `backbone.interface`: `example_netif_eth`
- `registered`: `false`
- `thread.role`: `not_polled`
- `rcp.version`: `not_polled`
- `ledger.state`: `none`

## Components, implementation status

| Component | Status |
|-----------|-------------|
| `bos_diagnostics_server` | Registers JSON endpoints on BR port 80. `/bos/status` includes read-only BR registration metadata so the UI can join site-server placement and convergence records. Thread/RCP fields deliberately not polled in request handlers yet |
| `bos_server_registration` | Claims the BR with the configured site server URL, persists `device_token`, sends status heartbeat with `X-Device-Token`, and posts convergence snapshots to `/api/border-routers/:id/convergence`; mDNS discovery is not implemented |
| `bos_ledger_ingress` | Implements protected `POST /bos/ledger/push`, CBOR envelope body-digest validation, inactive-partition write, and active metadata swap |
| `bos_ledger_mesh_serve` | Registers BR CoAP `/mesh/ledger/manifest`, `/mesh/ledger/chunk`, and `/mesh/have`; SRP TXT publish is still a warning stub |
| `bos_convergence_aggregator` | Starts a DNS-SD browse task for `_mesh._udp` and `_tmfs._udp`, parses TXT into a bounded peer table, and renders `/bos/convergence`, `/bos/peers`, and `/bos/ledger/torrent` peer rows |
| `bos_thread_dataset_anchor` | NVS read and write signatures present. Dataset load and save not implemented |

Remaining stubs log warnings so the runtime does not silently pretend to provide BR SRP TXT publication or dataset anchoring.

## Spec sections this firmware implements

- Section 6.1: LAN HTTP endpoints under `/bos/`. Owner: `bos_ledger_ingress`, `bos_server_registration`.
- Section 6.2: Mesh CoAP resources under `/mesh/ledger/`. Owner: `bos_ledger_mesh_serve`.
- Section 6.3: BR SRP TXT publish is target behavior only. Subscribe/browse is wired through OpenThread DNS-SD for `_mesh._udp` and `_tmfs._udp`.
- Section 7: On-flash layout. Owners: `partitions.csv`, NVS namespaces in each component.
- Section 8: State machines. Owners: `bos_server_registration` (boot), `bos_ledger_ingress` (ledger lifecycle).
- Section 9.1: Ledger ingest sequence. Owner: `bos_ledger_ingress` plus `bos_ledger_mesh_serve`.
- Section 9.2: Convergence aggregation peer discovery is implemented from SRP/TMFS browse data. Firmware posts the rendered convergence snapshot to `/api/border-routers/:id/convergence`, and the site server stores the latest snapshot with read-side freshness.
- Section 9.3: Current crash recovery reads active metadata from NVS. Full partition digest revalidation on boot is not implemented.
