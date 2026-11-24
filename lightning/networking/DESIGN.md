<!--
Module: qemu_lightning.networking.design
Purpose: Inventory VM networking and propose a Rust user-mode network backend for QEMU Lightning.
Created: 2026-10-16
Architecture: The manager (lightning/manager) owns VM lifecycle and launches QEMU without libvirt.
This folder will own every networking policy and packet path; the manager only spawns the
backend, passes it a validated configuration and connects QEMU to it.
Status: decisions recorded in section 9 (2026-10-16); M0-M5 done, backend published to Binaries/bin and the .deb; M6 functional checks complete 2026-10-17 (all items pass, guest-to-host by the guest tools' PUT; seamless drag-out needs the manager console, not in the validation fixture), upload about 10x libslirp; download still open: the 128-segment in-flight cap (published 046a5423...) is effective in the VM but did not remove the slow periods (see the M6 row); next: attack the guest RSS-CPU bottleneck (e.g. virtio-net, M9) rather than in-flight size.
-->

# QEMU Lightning networking design

Owner: the lightning-networking agent. All networking code lives in `lightning/networking/`
and is written only in Rust (2024 edition). Edits outside this folder are minimal, claimed in
`agent_notes.txt` first, and limited to the manager's launch/model glue and the build tool.

## 1. Current state (inventory, 2026-10-16)

| Area | Where | What it does today |
|---|---|---|
| Model | `manager/src/model.rs:43-69` | `Forward { host_port, guest_port, udp }`; `enum Network { Nat { forwards }, Bridge { name }, Isolated, None }`; `Machine.network_connected` (default true). New machines default to `Nat` with no forwards. |
| Validation | `model.rs:199-221` | Bridge name is `[A-Za-z0-9_-]+` (blocks `br0,helper=...` injection); forwards nonzero and unique per (host port, protocol). |
| Launch | `manager/src/runtime.rs:649-689` | `Nat` -> `-netdev user,id=net0` + `hostfwd=tcp|udp:127.0.0.1:H-:G`; `Isolated` -> `user,restrict=on`; `Bridge` -> `-netdev bridge,br=NAME` (default helper); NIC `virtio-net-pci` when `paravirtualized`, else `e1000e`; MAC `52:54:00` + first 3 bytes of the VM UUID (a clone gets a new MAC). |
| Link control | `runtime.rs:268`, `manager/src/backend.rs:143-157` | QMP `set_link net0` at start (if saved disconnected) and from the console footer toggle (`console.rs:118`, `ui.rs:934-1025`). |
| UI | `manager/src/forms.rs:479-545` | Combo: "Shared internet · NAT", "Host bridge", "Restricted network", "Disconnected", plus a bridge-name entry. No port-forward editor: forwards are only editable as raw JSON in "Advanced configuration" (`ui.rs:1170`). |
| File transfers | `runtime.rs:624-628`, `backend.rs:158-164, 414-430` | Transfers and the guest-tools CD require `Nat`. Starting a NAT VM auto-provisions the tools. |
| Transfer service | `manager/src/transfers.rs:68, 181-186` | Host listens on `127.0.0.1:<port>` (token-authenticated). The guest is told `10.0.2.2:<port>`; slirp maps 10.0.2.2 to the host loopback. |
| Guest tools | `transfers/src/setup.rs:181-195`, `guest.rs:39` | The config baked into the CD is `"<ip>:<port>\n<token>\n"`, parsed as a `SocketAddr`; the guest connects over plain TCP. Already-installed guests keep `10.0.2.2`. |
| Tests | `manager/src/tests.rs:28-59, 71-76` | Bridge-name injection and duplicate-forward rejection; `e1000e` vs `virtio-net-pci` selection. No test covers forwards on the command line, `Isolated`, `Bridge` or link state. |
| Validation VM | `../native-graphics-validation/library/machines.json` | The shared Windows test VM runs `-netdev user,id=net0` + `e1000e` (slirp, no VirtIO network driver in use). |

## 2. Host measurements (no VM involved)

Host: Ubuntu 26.04, kernel 7.0.0-38, Ryzen 7 8700G (16 threads), wired `enp13s0`
(192.168.0.62/24 plus global IPv6), DNS through the systemd-resolved stub at 127.0.0.53.
QEMU: the packaged `/usr/lib/qemu-lightning/bin/qemu-system-x86_64` 11.1.2 (the gpu-revoke build is
the same version). The measurement harness was a throwaway Rust program (scratchpad): a minimal fake
guest on a `-netdev stream` Unix socket, joined to the backend under test through a QEMU hub
(`-M none`, no guest OS, no NIC emulation).

**Netdev backends compiled in:** `socket stream dgram hubport tap passt user l2tpv3 bridge
vhost-user vhost-vdpa`. `-netdev stream` and `vhost-user` are available, so no QEMU C changes are
needed for either attachment. (`-netdev passt` exists but the passt binary is not installed.)

**libslirp 4.9.1 throughput** (TCP through `hostfwd`, 1460-byte MSS, zero-cost fake guest, two runs each):

| Direction | Throughput |
|---|---|
| host -> guest (download) | 4.57 / 4.72 Gbit/s |
| guest -> host (upload) | 1.69 / 1.66 Gbit/s |

slirp offers no window scaling (SYN carries only MSS 1460, window 65535), so the guest-side
window is capped at 64 KiB. Real Windows numbers will be lower: these exclude e1000e/virtio
emulation and guest costs. Upload is the clear slirp-side limit. The slirp gateway MAC is
`52:55:0a:00:02:02`; forwarded connections appear to the guest as coming from 10.0.2.2.

**`-netdev stream` transport ceiling** (stream -> hub -> stream, frames only):

| Frame size | Rate |
|---|---|
| 1514 B | 687 kframes/s = 8.3 Gbit/s |
| 9014 B | 263 kframes/s = 19 Gbit/s |

So a stream-attached backend is not limited by QEMU's transport at 1500 MTU before roughly
8 Gbit/s, well above what e1000e delivers. (A 65535-byte frame run stalled; not investigated.)

**Bridge mode is broken on this host today**, for four independent reasons:
1. The packaged QEMU's built-in default helper path is a stale build path,
   `Binaries/libexec/qemu-bridge-helper`
   (missing). `-netdev bridge,br=br0` fails with "bridge helper failed". The gpu-revoke build has
   the same problem with another scratch path.
2. The system `/usr/lib/qemu/qemu-bridge-helper` is not setuid (`-rwxr-xr-x root`) and has no
   file capabilities.
3. `/etc/qemu/bridge.conf` does not exist, so even a setuid helper refuses every bridge.
4. No bridge device exists; `enp13s0` holds the host IP directly.

**Privileges that matter:**
- `/dev/net/tun` is 0666, but creating a tap needs CAP_NET_ADMIN (`-netdev tap,ifname=...` ->
  "Operation not permitted"). A persistent tap created once by root with `user x` can then be
  opened by QEMU without privileges.
- `/dev/vhost-net` is `root:kvm 0660` with no ACL, and the user is not in group `kvm` (`/dev/kvm`
  works through an ACL). So in-kernel vhost-net for tap is unavailable without a group change.
- `net.ipv4.ping_group_range = 0 2147483647`: unprivileged ICMP echo sockets work for IPv4 and IPv6
  (checked), so a user-mode NAT can translate ping without raw sockets.
- TCP `SO_PEEK_OFF` works on this kernel (checked: two successive `MSG_PEEK` reads return
  consecutive bytes). The proposed TCP design relies on it.
- `kernel.apparmor_restrict_unprivileged_userns = 1`: unprivileged user namespaces are restricted.
  The proposed design does not need namespaces, unlike slirp4netns or pasta-style setups.
- `net.ipv4.ip_forward = 0`: kernel NAT through a tap would need root to enable forwarding and
  nftables masquerade, which is exactly what libvirt does with a root daemon.

## 3. Where libvirt is stronger, and where we can do better

| Concern | libvirt (root daemon) | QEMU Lightning today | This design |
|---|---|---|---|
| NAT | Kernel NAT on `virbr0`, dnsmasq DHCP/DNS | slirp: slow upload, 64 KiB window, no stats, no live forward changes | Rust user-mode NAT; host sockets; live control |
| Port forwards | iptables/nftables DNAT, hooks | hostfwd at launch only, loopback only | Live add/remove, choose bind address, per-rule log |
| Host isolation | nwfilter | slirp exposes all of host loopback through 10.0.2.2 | 10.0.2.2 reaches only allowlisted host services |
| Firewall | nwfilter/nftables (root) | `restrict=on` only | Per-VM egress policy in user space (internet / LAN / host / ports) |
| Bridge / LAN visibility | Bridges, macvtap, managed | Broken (section 2) | One-time Rust privileged setup, unprivileged runtime (section 5.6) |
| IPv6 | Full | slirp fec0::/64, translated | ULA + RA, translated to host IPv6 sockets |
| Performance | Kernel datapath + vhost-net | slirp + emulated NIC | stream now, optional vhost-user with offloads |

Honest limits: a user-mode backend can never put the guest on the LAN as its own L2 host
without privileges, and cannot firewall bridged traffic without root (nftables). It also adds
one per-VM process. Those are the costs of avoiding a root daemon.

## 4. Options considered

1. **Keep slirp, fix around it.** Cheapest. Keeps the loopback exposure, the 64 KiB window and the
   slow upload, and gives no live control. Rejected as the end state; it stays as a fallback.
2. **Use passt (C) through `-netdev passt`.** Mature and fast, but not installed, C, and outside
   the user's Rust-only rule. Useful only as an external reference and benchmark.
3. **Kernel NAT with a tap and nftables.** Fastest datapath, but it needs root at every start
   (forwarding, masquerade, tap creation) or a root daemon. This is the libvirt model we are
   avoiding.
4. **Rust user-mode backend over `-netdev stream` (recommended first).** No privileges, no QEMU C
   changes, measured transport headroom, full policy control, crash-isolated per VM.
5. **Rust vhost-user-net backend (recommended later, optional).** The backend reads guest
   virtqueues directly, bypassing QEMU's main loop, and can negotiate TSO/checksum offloads
   (64 KiB segments instead of 1460-byte ones). Needs `virtio-net` in the guest (NetKVM driver)
   and guest RAM on a shared memfd backend, which touches the Lightning/Venus memory setup and
   must be coordinated. Builds on option 4: the NAT/flow core is the same, only the attachment differs.

## 5. Proposed architecture

### 5.1 Process model

- One `lightning-network` process per running VM, built from `lightning/networking/`, published
  next to `lightning-venus-host` in `Binaries/bin`, and spawned by the manager's runtime with the
  existing `managed_command` pattern (PR_SET_PDEATHSIG, owner check), exactly like the Venus worker.
- Unprivileged. It never needs CAP_NET_ADMIN, CAP_NET_RAW, namespaces or setuid.
- It listens on `<private runtime dir>/<vm uuid>/net` (the 0700 `qlm-*` directory the runtime already
  validates; about 70 characters, inside the 108-byte Unix path limit) and checks SO_PEERCRED.
- QEMU attaches with `-netdev stream,id=net0,server=off,addr.type=unix,addr.path=...,reconnect-ms=N`.
  `id=net0` is kept, so QMP `set_link net0` and the console toggle work unchanged. With
  `reconnect-ms` QEMU reconnects if the backend restarts; the guest sees a link flap, not a dead NIC.
- A second Unix socket, `<dir>/net-control`, carries a small JSON-lines control protocol
  (add/remove forward, policy change, statistics, active flows, DHCP lease) for the manager.
- If the binary is missing, the runtime falls back to today's slirp arguments (same pattern as
  `has_venus_worker`), so builds without the backend keep working.

### 5.2 Compatibility addressing

Keep slirp's layout so existing Windows installations and their guest tools work without change:
network 10.0.2.0/24, gateway and host alias 10.0.2.2 with MAC `52:55:0a:00:02:02`, DNS 10.0.2.3,
guest lease 10.0.2.15. Windows identifies a network by its gateway MAC, so keeping it also keeps
the guest's network profile (Private/Public) and avoids a "new network" prompt after the switch.
The guest MAC stays the UUID-derived `52:54:00:xx:xx:xx`. The subnet becomes configurable later.

### 5.3 Modules (crate `lightning-networking`, library plus one binary)

| Module | Responsibility |
|---|---|
| `wire` | QEMU stream framing (4-byte big-endian length + Ethernet frame), bounds, max frame. |
| `packet` | Validated, zero-copy views and builders for Ethernet, ARP, IPv4, IPv6, ICMPv4/v6, UDP, TCP; checksums. Guest bytes are untrusted. |
| `arp`, `ndp` | Answer for the gateway, DNS and host aliases; learn the guest MAC. |
| `dhcp4` | DHCPv4 server for one lease (DISCOVER/OFFER/REQUEST/ACK/RELEASE/INFORM; router, DNS, mask, lease time, domain). |
| `ra6` | Router advertisements with a ULA /64 and RDNSS (Windows 10+ honours RDNSS); no DHCPv6 needed. |
| `dns` | Relay guest queries to the host resolvers from `/etc/resolv.conf` (127.0.0.53 here), UDP and TCP; optional local names (for example the host). |
| `tcp` | TCP translation (5.4). |
| `udp`, `icmp` | UDP flows on connected host sockets with idle timeouts; ping through unprivileged ICMP sockets. |
| `forward` | Host listeners (TCP/UDP) for port forwards, bind address per rule, live changes. |
| `policy` | Per-VM egress/ingress rules evaluated once per new flow (section 5.5). |
| `services` | The 10.0.2.2 host-service map: guest port -> host endpoint, allowlist only. |
| `control` | JSON-lines control socket, typed requests and replies (serde). |
| `config` | Validated launch configuration shared with the manager (the manager's `Network` maps onto it). |
| `event_loop` | Single-threaded mio (epoll) loop and timers; one per VM process. |
| `main.rs` | Thin CLI: parse arguments, load config, run. |

### 5.4 TCP translation without a full TCP stack

The backend terminates guest TCP and maps each connection onto a host socket, so the host
kernel's TCP (window scaling, congestion control, retransmission on the real network) does the
heavy lifting. To avoid a second buffering TCP stack, it follows the "socket as buffer" approach
(the idea passt popularised; implemented from scratch here):

- Guest -> host: payload is written straight into the host socket. The backend ACKs only bytes the
  kernel accepted and advertises a window equal to the host socket's free send space, so it never
  buffers guest data itself. While that window is below one segment the backend re-checks the
  free space every 10 ms and sends a window update when it reopens (a full socket does not
  always raise a new writable edge; found and fixed in M2, with a negative-control fixture).
- Host -> guest: data stays in the host socket's receive queue. The backend reads it with
  `MSG_PEEK` at `SO_PEEK_OFF`, sends segments to the guest, and consumes bytes from the kernel only
  when the guest ACKs them (`MSG_TRUNC`). Three duplicate ACKs resend only the head segment
  (NewReno partial-ACK recovery); a timeout rewinds the peek offset and resends everything
  (go-back-N). ACKs up to the highest sequence ever sent stay valid after a rewind. Guest RX
  rings do drop frames under load, so this is required even on the lossless Unix socket.
- Window scaling and a large window on the guest side (slirp has neither), MSS from the configured
  MTU, SACK not required initially. Toward the guest the backend runs NewReno congestion
  control (RFC 5681/6582, initial window 10 segments, Limited Transmit RFC 3042): a guest NIC
  whose receive ring overflows drops frames like a congested link (added in M3).
- Host sockets get a 4 MiB receive buffer when net.core.rmem_max allows it: unacknowledged
  host-to-guest data lives there, and autotuning does not grow it for a peeking reader (L1).
- Per batch of events, not per segment: in-order guest data is written to the host socket with
  one write, acknowledged bytes are discarded with one MSG_TRUNC, and host data is peeked once.
- Connect: a guest SYN starts a nonblocking `connect`; success -> SYN-ACK, any failure -> RST
  (ICMP unreachable is not generated yet). Port forwards run the same machine in the other direction.
- Bounded state: per-VM flow limits, SYN timeouts, idle and FIN_WAIT/TIME_WAIT timers, all named
  constants.

### 5.5 Policy (user-mode firewall)

Every new flow passes `policy` before a host socket exists. Proposed rule set per VM:
- Destination classes: internet, LAN (RFC 1918/ULA/link-local and the host's own subnets), host
  loopback, host services (10.0.2.2 allowlist).
- Modes map onto today's choices: **NAT** = internet + LAN + host services; **Restricted** = host
  services only (today `restrict=on` also blocks the transfer service, so transfers could now work
  in Restricted mode); optional **Internet only** = internet + host services, LAN blocked.
- Optional per-rule port allow/deny lists and a flow log on the control socket.
- 10.0.2.2 never maps to arbitrary host loopback ports: only the transfer service (and anything
  the user adds explicitly). This closes the slirp hole where any guest process can reach every
  127.0.0.1 service on the host.
- Port forwards bind 127.0.0.1 by default; binding a LAN address is an explicit per-rule choice.
- Bridged/macvtap traffic does not pass through this process, so it gets no user-mode firewall;
  filtering there would need root nftables (not proposed).

### 5.6 Bridging and LAN visibility (privileges stated plainly)

A guest that appears on the LAN with its own MAC and DHCP lease needs a kernel L2 attachment, and
creating one needs CAP_NET_ADMIN once. The proposal keeps privileges to a **one-time setup**, never
per start:

- **macvtap (recommended for wired NICs).** A small Rust setup tool, `lightning-network-setup`, run
  once through `pkexec`, creates a persistent macvtap on `enp13s0` via netlink (no `ip` shell-outs)
  and hands the `/dev/tapN` node to the user. The manager opens it unprivileged and passes the fd to
  `-netdev tap,fd=N`. Host networking is untouched (no bridge, no IP move). Limitation: the host
  cannot talk to the guest through macvtap, so in this mode the guest also gets the user-mode backend
  as a second, route-less NIC that reaches only host services (transfers keep working).
- **Bridge.** The same tool can create a persistent tap owned by the user and enslave it to an
  existing bridge. Creating the bridge itself moves the host IP and is left to NetworkManager/the
  user; the tool only verifies it. Replaces the broken `-netdev bridge` helper path.
- **Wi-Fi hosts** cannot bridge or macvtap reliably (802.11 client mode); there the answer is NAT
  with explicit forwards.
- Without vhost-net access (section 2) the tap datapath runs in QEMU userspace. Adding the user to
  group `kvm` would enable `vhost=on`; that is a host security setting and the user's call.
- The setup tool also gets an `undo` command that removes everything it created.

### 5.7 Performance plan

- M2/M3 target: match slirp download and beat its 1.7 Gbit/s upload in the host fixture, with
  1460 MSS; publish numbers from the same harness.
- Larger MTU on virtio-net (`host_mtu`) reduces frames per byte; the stream transport measured
  19 Gbit/s at 9014-byte frames. e1000e guests stay at 1500.
- vhost-user (M7, optional) for offloads and to leave QEMU's main loop out of the datapath.
- The emulated NIC is likely the real limit for the current e1000e guest; switching the guest to
  virtio-net (NetKVM) is a separate, guest-side decision.

### 5.8 Lifecycle and failure

- Backend crash: QEMU's stream reconnects (`reconnect-ms`); the manager restarts the backend
  under the same socket path; open TCP connections are lost, DHCP lease state is re-created from
  the config (single static lease).
- VM stop: the backend exits on QEMU disconnect plus PR_SET_PDEATHSIG from the manager.
- Link toggle: unchanged (QMP `set_link` on the NIC).
- Hardening after M3: `PR_SET_NO_NEW_PRIVS`, a seccomp filter and Landlock through raw `libc`
  syscalls (no extra crates), since the process parses untrusted guest frames.

### 5.9 Dependencies (cargo --offline, checked)

Available and test-built offline with edition 2024: `libc 0.2`, `mio 1.2` (os-poll, net, os-ext),
`socket2 0.6`, `serde 1` + `serde_json 1`, `thiserror 2`, `anyhow 1`. Also cached: `rustix`, `nix`,
`polling`, `tokio`, `zerocopy`, `bytes`, `heapless`, `ipnet`.
**Not available:** `smoltcp`, `etherparse`, `pnet`, `dhcproto`, `hickory-dns`, any vhost/virtio
crate. So packet codecs, DHCP, RA and (later) vhost-user/virtqueue handling are written here.
That is deliberate scope, not a gap: the codecs are small, and the TCP design above avoids a
general-purpose stack. Proposed dependency set: `libc`, `mio`, `serde`, `serde_json`, `thiserror`.

### 5.10 Manager integration (claimed, minimal)

- `runtime.rs`: spawn the backend and emit `-netdev stream` when the binary exists; otherwise the
  current slirp arguments. Pass the forwards and policy through a config file in the private dir.
- `model.rs`: keep the serialized `Network` variants readable (existing machines.json files must
  load). New fields only with `#[serde(default)]`; any new variant (for example macvtap) per the
  user's decisions.
- `backend.rs`: route live forward changes to the control socket; relax the "transfers require NAT"
  checks once Restricted mode allows the transfer service.
- `forms.rs`: a small forward editor and the policy choice (UI work is shared; minimal and claimed).
- `tools/lightning`: build and publish `lightning-network` (shared build tool, claimed).
- Architecture.md map and HISTORY.md entry when code lands (Architecture.md is currently claimed
  by d3d11-scope, so this waits).

## 6. Milestones and acceptance tests

Host-only fixtures first; the shared VM only at the end and only under a VM claim.

| # | Milestone | Acceptance |
|---|---|---|
| M0 | Crate skeleton, `wire`, `packet` | Unit tests: framing round-trip, truncated/oversized frames rejected, checksum vectors, malformed IP/TCP headers rejected; `cargo test --offline`, clippy clean. **Done 2026-10-16:** 47 tests pass (debug and release), including byte-for-byte rebuilds of three libslirp-captured frames (ARP, UDP, TCP SYN); rustfmt clean; no warnings with `-D warnings` plus extra rustc lints; rustdoc clean. Clippy is not installed on this host (the rustup toolchain lacks the component and installing it needs the network), so the clippy run is still open. The `lightning-network` binary arrives with M1. |
| M1 | ARP, DHCPv4, DNS relay | Rust fake-guest fixture over a socketpair: ARP for 10.0.2.2/10.0.2.3 answered; DORA yields 10.0.2.15 with router/DNS/mask; a DNS query through 10.0.2.3 resolves via the host resolver. | **Done 2026-10-16:** modules config, dhcp4, dns, entropy, backend (no-I/O core), event_loop (mio) and the lightning-network binary (--socket, --guest-mac, --dns, --mtu; accepts one same-uid connection, then unlinks the path). tests/fixtures.rs drives the real event loop and the real binary with a fake guest and a fake resolver: DORA lease 10.0.2.15 with router/DNS/mask/lease, ARP for 10.0.2.2/10.0.2.3, DNS answered with the guest's ID, ping to the gateway, failover to a second resolver after 2 s, clean exit on disconnect. The ignored test resolves localhost through the host's real systemd-resolved stub (passes). Large DNS answers are IPv4-fragmented to the MTU. 78 tests pass (debug and release, strict lints); clippy still unavailable. |
| M2 | TCP translation + port forwards + UDP + ICMP | Fake-guest fixture: outbound TCP to a local test server (data integrity both directions, 1 GiB, with injected guest-side drops to exercise retransmission); inbound forward; UDP echo; ping 127.0.0.1-mapped host service refused, ping internet host via ICMP socket. | **Done 2026-10-16:** tcp_nat (translator above), udp_nat (endpoint-independent mapping, address/port-filtered replies, idle expiry, UDP forwards), ping (unprivileged ICMP sockets), host_socket, tokens, guest_link; gateway reaches only configured host services (`--host-service tcp:G=HOST:P`), 10.0.2.3:53/TCP goes to the first upstream; forwards via `--forward tcp:ADDR:P=G` bind before QEMU is accepted. Fixtures (tests/fixtures/m2.rs): 8 MiB up+down via a host service, 4 MiB download with every 7th new segment dropped (retransmissions > 0), 32 MiB upload through a 2 s stalled reader, 2 MiB echo with half-close, refusals (unmapped gateway port, 127.0.0.1, unused 10.0.2.77, 10.0.2.3:80, closed LAN port), LAN-address path, TCP DNS, TCP and UDP forwards, UDP stranger filtering, ping to the LAN address and loopback refused. 94 tests pass (5 release runs with strict lints + debug). Ignored release test: 1 GiB each way byte-exact, 2.3 Gbit/s up and 2.5 Gbit/s down with the fake guest's own TCP in the loop (not comparable with the slirp harness; M3 measures like for like). |
| M3 | QEMU interop + performance | `-M none` QEMU, `-netdev stream` to the backend through a hub (the harness from section 2): same tests through real QEMU framing; throughput >= slirp download and > 1.7 Gbit/s upload; backend restart with `reconnect-ms` recovers. | **Released 2026-10-16; the user accepted the download result as measured and asked for a later optimization task (row L1):** tests/fixtures/m3.rs runs QEMU 11.1.2 `-M none` with a hub joining the fake guest's `-netdev stream` to the real binary. Passing: DHCP, DNS and an 8 MiB byte-exact upload through real QEMU framing; backend killed and restarted on the same path, QEMU reconnects (`reconnect-ms=100`) and DHCP/DNS work again. Throughput, ignored release test, 256 MiB each way, same fake guest for all (capped at 64 KiB in flight, because the hub drops frames under larger bursts), three runs: libslirp inside QEMU 1.8 up / 4.4-4.6 down Gbit/s; libslirp in a second QEMU over the same stream hop 1.9-2.0 / 3.3-3.4; lightning-network 4.9-5.1 / 2.9-3.0. Upload target met (2.6x libslirp); download target (>= libslirp) not met: 12% below libslirp over the same path, 35% below in-QEMU libslirp (which skips one QEMU stream crossing per frame). Without QEMU (fake guest on a socketpair) the backend moves 7.1 Gbit/s up and 4.1 down. Changes made during M3: the missing `--forward`/`--host-service` flags, guest-side congestion control, batched host writes (upload 2.8 to 7.1 Gbit/s direct), deferred discard/peek, Limited Transmit (loss fixture 44 s to 3 s), vectored output queue, frame buffer pool, allocation-free frame decoder, TCP/UDP counters in the exit summary. |
| M4 | Policy + host-service allowlist + control socket | Fixture: 10.0.2.2 reaches only the allowlisted port; LAN-block mode refuses RFC 1918 targets; live forward add/remove via control socket; flow limits enforced. | **Done 2026-10-16:** src/policy.rs (egress modes internet-and-lan, internet-only, host-services-only; LAN = RFC 1918 + RFC 6598 + the host's attached IPv4 networks via getifaddrs; host-services-only also stops the DNS relay and ping), configurable flow limits (`--max-tcp/--max-udp/--max-ping`, defaults 4096/1024/256), forwards on stable listener ids so they can be removed live, src/control.rs (`--control PATH`, same-uid JSON lines: status, add_forward, remove_forward, set_egress; unknown requests or fields rejected). Fixtures (tests/fixtures/m4.rs): internet-only refuses a listening LAN port and LAN ping but keeps host services; host-services-only keeps DHCP and the host service, refuses TEST-NET, TCP/UDP DNS and ping; limits of 2 TCP / 1 UDP enforced and freed after close; the binary's control socket adds a forward that a host client then uses, removes it (host connect refused), switches egress live, reports counters and is removed at exit. Negative control: with the policy forced open, the three policy fixtures fail. 108 tests pass (3 release runs with strict lints + debug); throughput unchanged (download 3.76-3.80 vs libslirp over the same path 3.35-3.41 Gbit/s). |
| M5 | Manager integration (claimed) | Manager tests: launch plan emits `-netdev stream` with the backend present and slirp without it; old machines.json loads; transfers config unchanged. | **Done 2026-10-16 (runtime/model/tests only, as the user scoped it):** runtime.rs spawns `bin/lightning-network` (when published) before QEMU with `--socket/--control` in the private runtime dir, `--guest-mac`, `--forward` per rule, `--egress host-services-only` for Restricted and `internet-only` when set, and the transfer service as the only `--host-service` (port read from transfers/connection.json); QEMU gets `-netdev stream,...,reconnect-ms=1000`; without the binary everything stays on libslirp; the Venus worker's reaping was generalised (OwnedWorker) and also reaps the backend. model.rs: serde-default `Machine.internet_only` and `Forward.lan` (old catalogs load unchanged; slirp honours `lan` too). Tests: launch plan with/without the backend, backend arguments per mode, old catalogs; ignored end-to-end test (diskless BIOS VM, e1000e iPXE network boot) gets a DHCP offer from the backend through QEMU, 3/3. Publishing (2026-10-16): tools/lightning/network.rs tests and publishes `bin/lightning-network` (`build.sh --network`; `--gui` builds it too); deb.rs requires and packages it (package version 0.3.0-6); published to Binaries/bin after 108 passing tests; full build + package on 2026-10-16: qemu-lightning_0.3.0-6 (adds bin/lightning-network; not installed). Not yet done: the Settings UI (Internet-only, forward editor with LAN option) and relaxing the transfers-require-NAT checks for Restricted in backend.rs; each needs its own claim. |
| M6 | Windows VM validation (VM claim) | Guest gets DHCP lease, DNS works, Edge loads a page, guest tools transfer a file both ways, port forward reaches a guest listener, `set_link` toggle works, network profile unchanged; throughput with a Rust Windows test tool (built for x86_64-pc-windows-gnu) vs the slirp baseline on the same guest. Never while another agent holds the VM. **Known limitation to re-measure here (user, 2026-10-16):** in the M3 harness download is 3.7-3.9 Gbit/s, above libslirp over the same stream path (3.0-3.4) but below libslirp inside QEMU (4.3-4.6), which skips one QEMU stream crossing per frame; the VM run (NIC straight to `-netdev stream`) decides whether any download gap remains. **Result (2026-10-16):** validated with gaps. Passed: DHCP, DNS, NCSI (fetched with PowerShell, not Edge), unchanged profile and gateway MAC, guest tools host to guest (8 MiB, sha256 match; guest to host not run), live port forward and `set_link` all pass. Upload 3.7-3.9 Gbit/s vs libslirp 0.38. Download is bimodal per connection: 3.6-4.2 Gbit/s or 0.34-0.37, vs libslirp 1.0-1.8 steady. Not loss or interrupt moderation; with RSS off it is always fast. The backend keeps 360-440 KB in flight (about the 256-descriptor e1000e RX ring) and on one RSS queue Windows then refills 64 frames per ~2 ms, while libslirp keeps under 65 KB in flight. Cap implemented (`MAX_FLIGHT_SEGMENTS` = 128, test `tcp_download_in_flight_is_capped`; host harness unchanged). Next: re-measure in the VM; in the same VM slot run a guest-to-host transfer, Edge, and the Rust Windows tool (throughput here used PowerShell sockets). **Re-test (2026-10-17, m6-gaps; backend with the cap, RSS on, ITR Adaptive; user signed in and using the guest: Edge video + Solitaire/WebView2, CPUs 1-4 34-48% busy when idle):** guest-to-host: the guest tools' PUT over 10.0.2.2:41569 (the exact `Client::send` exchange, driven over the serial agent) delivered 8 MiB, status 0, sha256 match; the user's drag out of the VM window did not work because the fixture shows the VM in QEMU's GTK window, and seamless drag lives in the manager console (`console.rs`), so drag-out was not tested. Edge: example.com rendered in a visible window, page title after 4.7 s. Rust Windows tool (`m6-wintool.exe`, x86_64-pc-windows-gnu, delivered host to guest by the guest tools in 2.2 s): download 3.0-4.07 Gbit/s on 15/15 runs in a fast period, upload 3.54-3.78. Cap verified in the guest capture: at most 186,880 B in flight (128 x 1460; was 439 KB). **Download did not hold:** it alternates between fast periods (3.0-4.1 Gbit/s, both clients) and slow periods lasting minutes (0.41-0.79 Gbit/s, both clients equally, Rust/PowerShell alternated run by run). The PowerShell 8-run probe was 1/8 >= 1.8 right after boot, 7/8 later (1.80-3.96, lowest 1.795). In a slow period guest CPU 1, one of the two RSS queue CPUs (indirection 0x64/1x64), is 99.5% busy while the guest overall is 54% busy; host QEMU main thread 36%, backend 18%. Same-day libslirp on the same loaded guest: download 0.57-0.71 (M6: 1.03-1.28), upload 0.36. So in-flight size was not the cause; slow periods track the guest's receive CPU saturating under user load. Upload with PowerShell 2.2-2.9 under that load (Rust tool 3.5-3.8). The cap stays (smaller guest queue, host harness unchanged or better). Evidence: work/m6-network/ (download-probe-capped*.json, lightning-capped*.json, wintool-*.json, ab-a.json, percpu-slow.json, load-a.json, captures-gaps/, edge-lightning.*, gaps-state.json, slirp-gaps.json). |
| M7 | Optional: IPv6 (RA + translation) | Fixture + VM: guest gets a ULA address via SLAAC, reaches an IPv6 host service, `ping -6` works. |
| M8 | Optional: bridging/macvtap setup tool | Host-only: dry-run plan and netlink request encoding unit-tested; real setup only after explicit user approval (pkexec), then a VM check that the guest gets a LAN lease. |
| M9 | Optional: vhost-user-net | Host fixture with a Rust vhost-user front-end test; VM run with virtio-net/NetKVM; throughput vs M6. Requires a shared-memory backend decision with the graphics agents. |
| L1 | Download throughput (requested by the user when accepting M3) | Target: download >= libslirp over the same stream path in the M3 harness. **Done 2026-10-16.** Cause, found with per-phase CPU accounting added to the M3 benchmark plus temporary counters: host-to-guest bytes stay in the host socket's receive queue until the guest ACKs them, and Linux autotuning never grows that buffer past its 128 KiB default because peeking is not consumption, so at most 93 KiB were ever in flight (send attempts ended for lack of host data 53k times, for the congestion window 12 times, for the guest window never); nobody was CPU-bound and the hub QEMU idled 14% of the time. Fix (as passt does): host TCP sockets get SO_RCVBUF = 4 MiB when net.core.rmem_max allows it (4 MiB on this host), else autotuning is kept. Result, three runs, 256 MiB: download 3.67-3.88 Gbit/s vs libslirp over the same path 3.02-3.44 (target met) and in-QEMU libslirp 4.30-4.64; the hub QEMU is now about 97% busy, so the harness, not the backend, limits it. Without QEMU: download 4.1 to 5.3 Gbit/s. Remaining leads if more is needed: one read per guest frame when the backend keeps up, the double map lookup per segment, frames built in place in the output queue; a larger MTU (virtio-net host_mtu with `--mtu`) cuts frames per byte in the VM. Hosts with a small rmem_max keep the old limit; raising it is a host setting (root). |

## 7. Risks and open questions

- Windows NIC drop behaviour under bursts (e1000e RX ring) drives retransmission tuning; only M6
  shows it.
- Switching an existing guest from slirp to the backend must not trigger a new Windows network
  profile; the gateway MAC choice should prevent it, to be verified in M6.
- vhost-user needs `-object memory-backend-memfd,share=on` for all guest RAM; its interaction
  with the Lightning PCI device and Venus mappings is unknown.
- The stale bridge-helper path is baked into the packaged QEMU binary; independent of this design,
  `-netdev bridge` should pass `helper=` explicitly or be replaced (section 5.6).

## 8. What this design does not do

No libvirt, no root daemon, no per-start privilege escalation, no C/shell/Python in product
code, and no change to QEMU's C sources (both attachments use netdevs QEMU already has).

## 9. Decisions needed from the user

**Decided 2026-10-16: the user chose option (a), the recommendation, for all eight.** In short:
`-netdev stream` first (no vhost-user plan yet); NAT only for now, bridging revisited later;
10.0.2.2 restricted to the transfer service plus explicit additions; NAT allows internet + LAN
with an optional "Internet only" mode; Restricted mode allows the guest-tools service; IPv6
after M6; forwards bind loopback by default with an explicit LAN option and a Settings editor;
slirp stays as the fallback when the backend binary is absent. M7 (IPv6) stays planned; M8 and
M9 are deferred until the user revisits them.

The options as they were offered (the recommendation is listed first):

1. **Attachment and performance goal.** (a) `-netdev stream` only, M0-M6 (recommended start);
   (b) also plan vhost-user (M9), which needs NetKVM in the guest and shared guest RAM.
2. **Bridging / LAN visibility.** (a) NAT only for now, revisit later; (b) macvtap via a one-time
   `pkexec` Rust setup tool; (c) tap on a user-managed bridge; (d) both b and c.
3. **Host loopback exposure.** (a) 10.0.2.2 reaches only the transfer service plus explicit
   additions (recommended; breaks any guest use of other host loopback ports); (b) keep slirp's
   full loopback access.
4. **Default egress for NAT.** (a) internet + LAN, as today, with an optional "Internet only" mode
   (recommended); (b) internet only by default.
5. **Restricted mode.** (a) allow the guest-tools service so transfers work there (recommended);
   (b) no host access at all, as today.
6. **IPv6.** (a) in scope after M6 (M7); (b) IPv4 only; (c) before M6.
7. **Port forwards.** (a) loopback by default, LAN binding as an explicit per-rule option, plus a
   small forward editor in Settings (recommended); (b) loopback only.
8. **slirp fallback.** (a) keep it when the backend binary is absent (recommended); (b) remove it
   after M6.
