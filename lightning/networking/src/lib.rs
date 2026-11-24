//! Module: lightning.networking
//! Purpose: Provide the user-mode network backend that QEMU Lightning guests attach to.
//! Created: 2026-10-16
//! Architecture: QEMU owns the emulated NIC and forwards Ethernet frames over a
//! -netdev stream Unix socket. This crate owns the framing on that socket (wire),
//! validated packet views and builders (packet), ARP/DHCP/DNS for the guest
//! network, the TCP/UDP/ICMP translation onto host sockets (tcp_nat, udp_nat,
//! ping), the per-VM egress policy (policy) and the live control socket
//! (control). The manager spawns the backend, passes it a validated
//! configuration and may adjust forwards and egress through the control socket.
//! DESIGN.md in this folder records the plan, milestones and the user's decisions.

pub mod backend;
pub mod config;
pub mod control;
pub mod dhcp4;
pub mod dns;
pub mod entropy;
pub mod event_loop;
pub mod guest_link;
pub mod host_socket;
pub mod packet;
pub mod ping;
pub mod policy;
pub mod tcp_nat;
pub mod tokens;
pub mod udp_nat;
pub mod wire;
