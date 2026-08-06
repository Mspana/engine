# Plan — LAN address picker + QR pairing

Two changes that together make phone pairing a single scan instead of typing a URL and a
43-character code.

## Part A — LAN address picker (do first)

**The bug:** `AIRemoteServer::get_client_url()` returns the *first* private IPv4 it finds.
This machine has seven non-loopback addresses, including `10.5.0.2` on NordLynx (NordVPN)
alongside the real `192.168.0.88`. `10.5.0.2` satisfies the `10.0.0.0/8` check, so the URL —
and therefore any QR built from it — can point at a VPN interface the phone cannot reach.

**Fix:** rank candidates instead of taking the first.

- Reject loopback, link-local (`169.254/16`, `fe80::/10`) and anything non-private.
- Score by address class, because home LANs are overwhelmingly `192.168`:
  `192.168/16` = 300, `172.16/12` = 200, `10/8` = 100.
- Penalise adapters whose friendly name looks virtual or VPN-ish (`nordlynx`, `openvpn`,
  `wireguard`, `vethernet`, `hyper-v`, `wsl`, `docker`, `vmware`, `virtualbox`, `tap`,
  `tun`, `bluetooth`, `loopback`, `virtual`). `IP::get_local_interfaces()` gives us
  `name_friendly`, which on Windows is the adapter description.
- Highest score wins; ties break on enumeration order.

Because a heuristic can still guess wrong, the dialog gets an **override dropdown** listing
every candidate as `192.168.0.88 — Ethernet 5`. The choice persists in project metadata and
takes precedence whenever that address is still present.

New API on `AIRemoteServer`: `list_lan_addresses()`, `set_preferred_address()`,
`get_preferred_address()`. `get_client_url()` consults the preference, then the ranking.

## Part B — QR pairing

**Payload:** the client URL with the pairing code in the **fragment**:

```
https://192.168.0.88:8420/#c=<43-char code>
```

The fragment matters. A query string would put the code in the request line and the server's
view; a fragment is never sent to the server. The client reads it, then immediately calls
`history.replaceState` so it does not linger in the address bar or session history. Combined
with the existing single-use and two-minute expiry, a code that leaks into history is
already spent.

Security is otherwise unchanged — the code is still 32 random bytes, single-use,
attempt-limited, and expiring. The only real shift is exposure: a QR is grabbed by a camera
faster than text, but it is also on screen for about a second rather than the half-minute it
takes to type 43 characters. Net roughly neutral.

**Encoder:** no QR generator exists in the tree, and hand-rolling Reed–Solomon, mask scoring
and BCH format bits is exactly the kind of thing that fails silently. So the new
`modules/ai/remote/ai_remote_qr.{h,cpp}` is written to a tight spec (byte mode, EC level M,
versions 1–10) and **verified against golden vectors produced by Python's `qrcode`
library** — byte-for-byte module comparison in a doctest, so a subtle placement or masking
error cannot pass.

**Display:** the dialog renders the module bitmap into an `ImageTexture` at integer scale
(nearest-neighbour, so it stays crisp) with the mandatory 4-module quiet zone, ~260 px. The
text code stays visible underneath as a fallback for manual entry and for desktop browsers.

**Client:** on load, if `location.hash` carries `c=`, prefill the pairing field, strip the
fragment, and pair automatically.

## Out of scope

The self-signed certificate warning on first connection remains — QR does not change it.
The picker becomes dead code once relay mode is the default path (no local address is
involved), but LAN mode stays worth keeping as the offline route.

## Test plan

- Doctest: address ranking against a synthetic interface list that reproduces this machine
  (NordLynx `10.5.0.2` + Ethernet `192.168.0.88` + five link-locals) — expects `192.168.0.88`.
- Doctest: QR golden vectors for several payload lengths, spanning a version bump.
- Live: pair a phone by scanning.
