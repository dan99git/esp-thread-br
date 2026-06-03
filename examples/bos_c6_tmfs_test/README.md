# BOS Bare ESP32-C6 TMFS Test Firmware

This is a bench-only firmware for a bare XIAO/ESP32-C6 board.

It does not include SD, GLDF package loading, dimmer/output control, Wi-Fi, or
site commissioning. It exists to give the BOS border router a real Thread peer
that advertises `_tmfs._udp` and responds to authenticated ThreadTorrent-lite
HELLO, PING, and HAVE packets.

This image is not package-backed product firmware. It deliberately erases
OpenThread persistent state at boot, applies one fixed test dataset, and reports
an empty ledger state so the BR can prove peer discovery and torrent row
rendering before a real package/SD-backed device is present.

Latest bench recovery handover: `docs/scratch/br-handover-07-current-state-and-next-work-2026-06-03.md`.

Use this firmware for the black C6/XIAO bench peer while restoring the BR UI.
Do not substitute the outer Building OS repo's `firmware/xiao-esp32c6` for this
short-term test; that target is the package-backed production luminaire
firmware and brings in GLDF, SD, dimmer/output, calibration, and site
commissioning concerns.

Short-term goal:

1. Join the same Thread dataset as the BR.
2. Advertise `_tmfs._udp` on UDP `61616`.
3. Appear in the BR `/bos/ledger/torrent`, `/bos/convergence`, `/bos/peers`,
   and Ledger Torrent UI.
4. Continue to ThreadTorrent-lite file-transfer testing only after that peer row
   is visible.

Thread test dataset:

- Network name: `BuildingOS`
- Channel: `15`
- PAN ID: `0xB051`
- Extended PAN ID: `b05f00000000c601`
- Mesh-local prefix: `fd0b:05f0:0000:c601::/64`
- Network key: `b05f1badc0defacedeadbeef01234567`

ThreadTorrent-lite test key:

- `00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff`

Build:

```powershell
idf.py -B build-codex build
```

Flash example:

```powershell
idf.py -B build-codex -p COM12 flash monitor
```

Expected serial evidence:

- `Building OS bare ESP32-C6 TMFS test firmware`
- `SRP staged: bare-c6-..._tmfs._udp`
- `TMFS UDP listening on 61616`
- `Thread role: leader`, `router`, or `child`
- `runtime: role=... node=bare-c6-... service=_tmfs._udp:61616`

Expected BR-side Thread CLI checks after the BR is on the same dataset:

```text
srp server service
dns service bare-c6-<ieee802154-mac> _tmfs._udp.default.service.arpa
```

Expected SRP/TMFS fields:

- service: `_tmfs._udp`
- port: `61616`
- `cat=ledger`
- `caps=p2p,bare-test`
- `role=leech`
- `state=waiting`
- `lv=0`
- `have=0`
- `chunks=0`
