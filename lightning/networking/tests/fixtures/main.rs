//! Module: lightning.networking.tests.fixtures
//! Purpose: Host-only acceptance fixtures, one module per milestone.
//! Created: 2026-10-16
//! Architecture: One integration-test binary, so the shared fake guest, TCP endpoint
//! and test servers in common are compiled once and every helper has a caller.
//! A fake guest drives the real event loop over a socketpair, or the real
//! lightning-network binary over its socket, exactly as QEMU's -netdev stream
//! would. No VM and no privileges are involved.

mod common;
mod m1;
mod m2;
mod m3;
mod m4;
