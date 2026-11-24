//! Module: lightning.networking.policy
//! Purpose: Decide which outside destinations a VM may reach (the user-mode firewall).
//! Created: 2026-10-16
//! Architecture: The backend asks this module once per new flow, before any host
//! socket exists (DESIGN.md section 5.5, decisions 4 and 5). The gateway's host
//! services, the backend's own addresses, DHCP, ARP and port forwards are not
//! egress and are handled by the backend directly; this module only classifies
//! destinations beyond the guest network. The mode can change while the VM runs
//! (control socket); a change applies to new flows, existing ones keep running.

use std::fmt;
use std::net::Ipv4Addr;
use std::str::FromStr;

use thiserror::Error;

/// What a VM may reach outside its own network.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Egress {
    /// The default NAT mode: the internet and the host's LAN.
    #[default]
    InternetAndLan,
    /// The internet only; LAN destinations are refused.
    InternetOnly,
    /// Nothing outside: only the gateway's configured host services (the
    /// "Restricted" mode, which still lets the guest tools reach the host).
    HostServicesOnly,
}

const INTERNET_AND_LAN: &str = "internet-and-lan";
const INTERNET_ONLY: &str = "internet-only";
const HOST_SERVICES_ONLY: &str = "host-services-only";

impl fmt::Display for Egress {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::InternetAndLan => INTERNET_AND_LAN,
            Self::InternetOnly => INTERNET_ONLY,
            Self::HostServicesOnly => HOST_SERVICES_ONLY,
        })
    }
}

#[derive(Debug, Error, PartialEq, Eq)]
#[error(
    "unknown egress mode {0:?}: expected {INTERNET_AND_LAN}, {INTERNET_ONLY} or {HOST_SERVICES_ONLY}"
)]
pub struct ParseEgressError(pub String);

impl FromStr for Egress {
    type Err = ParseEgressError;

    fn from_str(text: &str) -> Result<Self, Self::Err> {
        match text {
            INTERNET_AND_LAN => Ok(Self::InternetAndLan),
            INTERNET_ONLY => Ok(Self::InternetOnly),
            HOST_SERVICES_ONLY => Ok(Self::HostServicesOnly),
            _ => Err(ParseEgressError(text.into())),
        }
    }
}

impl Egress {
    /// May a new flow go to this outside address (already known to be outside the
    /// guest network and not loopback, link-local, multicast or broadcast)?
    pub fn allows(self, address: Ipv4Addr, lan: &LocalNetworks) -> bool {
        match self {
            Self::InternetAndLan => true,
            Self::InternetOnly => !lan.contains(address),
            Self::HostServicesOnly => false,
        }
    }

    /// May guest DNS be relayed to the host's resolvers? Not when nothing outside
    /// is reachable: the resolvers are themselves an outside service.
    pub fn allows_dns(self) -> bool {
        self != Self::HostServicesOnly
    }
}

/// An IPv4 network as address and prefix length.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ipv4Network {
    pub address: Ipv4Addr,
    pub prefix_length: u8,
}

const IPV4_BITS: u32 = 32;

impl Ipv4Network {
    pub const fn new(address: Ipv4Addr, prefix_length: u8) -> Self {
        Self {
            address,
            prefix_length,
        }
    }

    fn mask(self) -> u32 {
        match self.prefix_length {
            0 => 0,
            length => u32::MAX << (IPV4_BITS - u32::from(length.min(IPV4_BITS as u8))),
        }
    }

    pub fn contains(self, address: Ipv4Addr) -> bool {
        u32::from(address) & self.mask() == u32::from(self.address) & self.mask()
    }
}

/// Private-use ranges that are LAN wherever they appear (RFC 1918) and the shared
/// carrier-grade NAT range (RFC 6598), which is never the public internet.
pub const PRIVATE_NETWORKS: [Ipv4Network; 4] = [
    Ipv4Network::new(Ipv4Addr::new(10, 0, 0, 0), 8),
    Ipv4Network::new(Ipv4Addr::new(172, 16, 0, 0), 12),
    Ipv4Network::new(Ipv4Addr::new(192, 168, 0, 0), 16),
    Ipv4Network::new(Ipv4Addr::new(100, 64, 0, 0), 10),
];

/// The networks that count as "LAN" for InternetOnly: the private ranges plus
/// every IPv4 network the host is directly attached to (a public LAN is LAN too).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LocalNetworks {
    networks: Vec<Ipv4Network>,
}

impl LocalNetworks {
    /// Only the private ranges.
    pub fn private() -> Self {
        Self {
            networks: PRIVATE_NETWORKS.to_vec(),
        }
    }

    /// The private ranges plus the host's directly attached IPv4 networks, read
    /// once with getifaddrs. Interfaces that appear later are not included; a
    /// backend restart (or the private ranges) covers them.
    pub fn of_host() -> Self {
        let mut local = Self::private();

        local.networks.extend(host_interface_networks());
        local
    }

    pub fn with(mut self, network: Ipv4Network) -> Self {
        self.networks.push(network);
        self
    }

    pub fn contains(&self, address: Ipv4Addr) -> bool {
        self.networks
            .iter()
            .any(|network| network.contains(address))
    }
}

/// IPv4 networks of the host's non-loopback interfaces.
fn host_interface_networks() -> Vec<Ipv4Network> {
    let mut list: *mut libc::ifaddrs = std::ptr::null_mut();

    // SAFETY: getifaddrs stores a list it allocated in list, released below.
    if unsafe { libc::getifaddrs(&mut list) } != 0 {
        return Vec::new();
    }
    let mut networks = Vec::new();
    let mut entry = list;

    while !entry.is_null() {
        // SAFETY: entry is a node of the list getifaddrs returned and has not been
        // freed; its address pointers are null or point to sockaddrs whose family
        // field says how to read them.
        let (address, netmask) = unsafe { ((*entry).ifa_addr, (*entry).ifa_netmask) };

        if let (Some(address), Some(netmask)) = (ipv4_of(address), ipv4_of(netmask)) {
            if !address.is_loopback() {
                networks.push(Ipv4Network::new(
                    address,
                    u32::from(netmask).count_ones() as u8,
                ));
            }
        }
        // SAFETY: as above; ifa_next is the next node or null.
        entry = unsafe { (*entry).ifa_next };
    }
    // SAFETY: list came from getifaddrs and is freed exactly once.
    unsafe { libc::freeifaddrs(list) };
    networks
}

/// The IPv4 address in a sockaddr, if it is one.
fn ipv4_of(address: *const libc::sockaddr) -> Option<Ipv4Addr> {
    if address.is_null() {
        return None;
    }
    // SAFETY: a non-null sockaddr from getifaddrs is readable at least up to its
    // family field, and an AF_INET one is a full sockaddr_in.
    unsafe {
        if i32::from((*address).sa_family) != libc::AF_INET {
            return None;
        }
        let address = &*address.cast::<libc::sockaddr_in>();

        Some(Ipv4Addr::from(u32::from_be(address.sin_addr.s_addr)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const PUBLIC: Ipv4Addr = Ipv4Addr::new(8, 8, 8, 8);
    const HOME_ROUTER: Ipv4Addr = Ipv4Addr::new(192, 168, 0, 1);
    const CORPORATE: Ipv4Addr = Ipv4Addr::new(172, 20, 1, 1);
    const CARRIER_NAT: Ipv4Addr = Ipv4Addr::new(100, 100, 1, 1);
    /// A public range standing in for a host attached to a public LAN.
    const PUBLIC_LAN: Ipv4Network = Ipv4Network::new(Ipv4Addr::new(198, 51, 100, 0), 24);
    const PUBLIC_LAN_HOST: Ipv4Addr = Ipv4Addr::new(198, 51, 100, 7);

    #[test]
    fn modes_parse_and_print() {
        for mode in [
            Egress::InternetAndLan,
            Egress::InternetOnly,
            Egress::HostServicesOnly,
        ] {
            assert_eq!(mode.to_string().parse::<Egress>(), Ok(mode));
        }
        assert!("everything".parse::<Egress>().is_err());
        assert_eq!(Egress::default(), Egress::InternetAndLan);
    }

    #[test]
    fn internet_only_refuses_private_and_attached_networks() {
        let lan = LocalNetworks::private().with(PUBLIC_LAN);

        for address in [HOME_ROUTER, CORPORATE, CARRIER_NAT, PUBLIC_LAN_HOST] {
            assert!(!Egress::InternetOnly.allows(address, &lan), "{address}");
            assert!(Egress::InternetAndLan.allows(address, &lan), "{address}");
        }
        assert!(Egress::InternetOnly.allows(PUBLIC, &lan));
        assert!(!Egress::HostServicesOnly.allows(PUBLIC, &lan));
        assert!(!Egress::HostServicesOnly.allows_dns());
        assert!(Egress::InternetOnly.allows_dns());
    }

    #[test]
    fn network_edges() {
        let network = Ipv4Network::new(Ipv4Addr::new(172, 16, 0, 0), 12);

        assert!(network.contains(Ipv4Addr::new(172, 31, 255, 255)));
        assert!(!network.contains(Ipv4Addr::new(172, 32, 0, 0)));
        assert!(Ipv4Network::new(Ipv4Addr::UNSPECIFIED, 0).contains(PUBLIC));
    }

    #[test]
    fn host_networks_include_the_default_route_address() {
        // Every non-loopback IPv4 address of this host must be classified as LAN.
        let host = LocalNetworks::of_host();

        for network in host_interface_networks() {
            assert!(host.contains(network.address), "{network:?}");
        }
    }
}
