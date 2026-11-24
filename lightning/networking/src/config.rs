//! Module: lightning.networking.config
//! Purpose: Hold the validated addressing and upstream settings of one VM network.
//! Created: 2026-10-16
//! Architecture: The binary (and later the manager, through a config file) builds a
//! NetworkConfig once at startup; every other module reads it. The default layout
//! copies libslirp so installed guests and their guest tools keep working after the
//! switch: 10.0.2.0/24, gateway and host alias 10.0.2.2 with MAC 52:55:0a:00:02:02,
//! DNS 10.0.2.3 and the guest lease 10.0.2.15 (DESIGN.md section 5.2).

use std::fmt;
use std::net::{IpAddr, Ipv4Addr, SocketAddr};
use std::str::FromStr;

use thiserror::Error;

use crate::packet::ethernet::MacAddress;
use crate::packet::ipv4;
use crate::policy::Egress;

/// libslirp's default network, kept for compatibility with existing guests.
pub const DEFAULT_NETWORK: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 0);
pub const DEFAULT_PREFIX_LENGTH: u8 = 24;
pub const DEFAULT_GATEWAY: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 2);
pub const DEFAULT_DNS: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 3);
pub const DEFAULT_GUEST: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 15);
/// libslirp's gateway MAC (52:55 then the gateway address). Windows identifies a
/// network by its gateway MAC, so keeping it keeps the guest's network profile.
pub const DEFAULT_GATEWAY_MAC: MacAddress = MacAddress([0x52, 0x55, 0x0a, 0x00, 0x02, 0x02]);
/// Standard Ethernet MTU, used by e1000e and by virtio-net without host_mtu.
pub const DEFAULT_MTU: usize = 1500;
/// Every IPv4 host must accept datagrams of this size (RFC 791).
pub const MIN_MTU: usize = 576;
pub const MAX_MTU: usize = ipv4::MAX_PACKET_BYTES;
/// Shortest and longest prefixes that leave room for network, gateway, DNS, guest
/// and broadcast addresses.
pub const MIN_PREFIX_LENGTH: u8 = 8;
pub const MAX_PREFIX_LENGTH: u8 = 29;
/// glibc reads at most three nameserver lines; more would only delay failover.
pub const MAX_DNS_UPSTREAMS: usize = 3;
pub const DNS_PORT: u16 = 53;

const IPV4_BITS: u32 = 32;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum ConfigError {
    #[error("guest MAC {0} is a group address")]
    GroupGuestMac(MacAddress),
    #[error("gateway MAC must differ from the guest MAC and be a unicast address")]
    BadGatewayMac,
    #[error("prefix length {0} is outside {MIN_PREFIX_LENGTH}..={MAX_PREFIX_LENGTH}")]
    PrefixLength(u8),
    #[error("{name} address {address} is not a usable host address in {network}/{prefix_length}")]
    OutsideNetwork {
        name: &'static str,
        address: Ipv4Addr,
        network: Ipv4Addr,
        prefix_length: u8,
    },
    #[error("gateway, DNS and guest addresses must all differ")]
    DuplicateAddress,
    #[error("MTU {0} is outside {MIN_MTU}..={MAX_MTU}")]
    Mtu(usize),
    #[error("at most {MAX_DNS_UPSTREAMS} DNS upstreams are supported")]
    TooManyUpstreams,
    #[error("invalid {kind} {text:?}: expected {expected}")]
    Syntax {
        kind: &'static str,
        text: String,
        expected: &'static str,
    },
    #[error("ports must be nonzero: {0}")]
    ZeroPort(String),
    #[error("duplicate {0}")]
    Duplicate(String),
    #[error("UDP host services must use an IPv4 address: {0}")]
    UdpServiceNotIpv4(String),
    #[error("flow limits must be at least 1")]
    ZeroFlowLimit,
}

/// Simultaneous flows one VM may hold; new flows beyond them are refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FlowLimits {
    pub tcp_connections: usize,
    pub udp_mappings: usize,
    pub ping_flows: usize,
}

pub const DEFAULT_TCP_CONNECTIONS: usize = 4096;
pub const DEFAULT_UDP_MAPPINGS: usize = 1024;
pub const DEFAULT_PING_FLOWS: usize = 256;

impl Default for FlowLimits {
    fn default() -> Self {
        Self {
            tcp_connections: DEFAULT_TCP_CONNECTIONS,
            udp_mappings: DEFAULT_UDP_MAPPINGS,
            ping_flows: DEFAULT_PING_FLOWS,
        }
    }
}

/// Transport protocol of a forward or host service.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Protocol {
    Tcp,
    Udp,
}

impl fmt::Display for Protocol {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Tcp => "tcp",
            Self::Udp => "udp",
        })
    }
}

impl FromStr for Protocol {
    type Err = ();

    fn from_str(text: &str) -> Result<Self, Self::Err> {
        match text {
            "tcp" => Ok(Self::Tcp),
            "udp" => Ok(Self::Udp),
            _ => Err(()),
        }
    }
}

/// A host listener whose connections or datagrams reach a guest port. Written
/// "PROTOCOL:HOST_ADDRESS:HOST_PORT=GUEST_PORT", for example tcp:127.0.0.1:2222=22.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PortForward {
    pub protocol: Protocol,
    pub host: SocketAddr,
    pub guest_port: u16,
}

const FORWARD_SYNTAX: &str = "PROTOCOL:HOST_ADDRESS:HOST_PORT=GUEST_PORT";

impl FromStr for PortForward {
    type Err = ConfigError;

    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let syntax = || ConfigError::Syntax {
            kind: "port forward",
            text: text.into(),
            expected: FORWARD_SYNTAX,
        };
        let (protocol, rest) = text.split_once(':').ok_or_else(syntax)?;
        let (host, guest_port) = rest.rsplit_once('=').ok_or_else(syntax)?;

        Ok(Self {
            protocol: protocol.parse().map_err(|()| syntax())?,
            host: host.parse().map_err(|_| syntax())?,
            guest_port: guest_port.parse().map_err(|_| syntax())?,
        })
    }
}

impl fmt::Display for PortForward {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{}:{}={}",
            self.protocol, self.host, self.guest_port
        )
    }
}

/// A host endpoint the guest may reach at gateway:guest_port. Everything else on
/// the gateway address is refused (DESIGN.md decision 3). Written
/// "PROTOCOL:GUEST_PORT=HOST_ADDRESS:HOST_PORT", for example
/// tcp:12345=127.0.0.1:12345 for the guest-tools transfer service.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HostService {
    pub protocol: Protocol,
    pub guest_port: u16,
    pub host: SocketAddr,
}

const SERVICE_SYNTAX: &str = "PROTOCOL:GUEST_PORT=HOST_ADDRESS:HOST_PORT";

impl FromStr for HostService {
    type Err = ConfigError;

    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let syntax = || ConfigError::Syntax {
            kind: "host service",
            text: text.into(),
            expected: SERVICE_SYNTAX,
        };
        let (protocol, rest) = text.split_once(':').ok_or_else(syntax)?;
        let (guest_port, host) = rest.split_once('=').ok_or_else(syntax)?;

        Ok(Self {
            protocol: protocol.parse().map_err(|()| syntax())?,
            guest_port: guest_port.parse().map_err(|_| syntax())?,
            host: host.parse().map_err(|_| syntax())?,
        })
    }
}

impl fmt::Display for HostService {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{}:{}={}",
            self.protocol, self.guest_port, self.host
        )
    }
}

/// Validated settings of one VM network.
///
/// Invariants established by the constructors: the guest and gateway MACs are
/// distinct unicast addresses; the gateway, DNS and guest addresses are distinct
/// host addresses inside the network (neither the network nor the broadcast
/// address); the MTU is within MIN_MTU..=MAX_MTU; at most MAX_DNS_UPSTREAMS.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NetworkConfig {
    guest_mac: MacAddress,
    gateway_mac: MacAddress,
    network: Ipv4Addr,
    prefix_length: u8,
    gateway: Ipv4Addr,
    dns: Ipv4Addr,
    guest: Ipv4Addr,
    mtu: usize,
    dns_upstreams: Vec<SocketAddr>,
    port_forwards: Vec<PortForward>,
    host_services: Vec<HostService>,
    egress: Egress,
    flow_limits: FlowLimits,
}

impl NetworkConfig {
    /// The libslirp-compatible layout for a guest NIC with this MAC.
    pub fn slirp_compatible(
        guest_mac: MacAddress,
        dns_upstreams: Vec<SocketAddr>,
    ) -> Result<Self, ConfigError> {
        let config = Self {
            guest_mac,
            gateway_mac: DEFAULT_GATEWAY_MAC,
            network: DEFAULT_NETWORK,
            prefix_length: DEFAULT_PREFIX_LENGTH,
            gateway: DEFAULT_GATEWAY,
            dns: DEFAULT_DNS,
            guest: DEFAULT_GUEST,
            mtu: DEFAULT_MTU,
            dns_upstreams,
            port_forwards: Vec::new(),
            host_services: Vec::new(),
            egress: Egress::default(),
            flow_limits: FlowLimits::default(),
        };

        config.validate()?;
        Ok(config)
    }

    /// The same configuration with another MTU.
    pub fn with_mtu(mut self, mtu: usize) -> Result<Self, ConfigError> {
        self.mtu = mtu;
        self.validate()?;
        Ok(self)
    }

    /// The same configuration with these port forwards.
    pub fn with_port_forwards(mut self, forwards: Vec<PortForward>) -> Result<Self, ConfigError> {
        self.port_forwards = forwards;
        self.validate()?;
        Ok(self)
    }

    /// The same configuration with these host services.
    /// The same configuration with another egress mode.
    pub fn with_egress(mut self, egress: Egress) -> Self {
        self.egress = egress;
        self
    }

    /// The same configuration with other flow limits.
    pub fn with_flow_limits(mut self, limits: FlowLimits) -> Result<Self, ConfigError> {
        self.flow_limits = limits;
        self.validate()?;
        Ok(self)
    }

    pub fn egress(&self) -> Egress {
        self.egress
    }

    pub fn flow_limits(&self) -> FlowLimits {
        self.flow_limits
    }

    pub fn host_services(&self) -> &[HostService] {
        &self.host_services
    }

    pub fn with_host_services(mut self, services: Vec<HostService>) -> Result<Self, ConfigError> {
        self.host_services = services;
        self.validate()?;
        Ok(self)
    }

    fn validate(&self) -> Result<(), ConfigError> {
        if self.guest_mac.is_group() {
            return Err(ConfigError::GroupGuestMac(self.guest_mac));
        }
        if self.gateway_mac.is_group() || self.gateway_mac == self.guest_mac {
            return Err(ConfigError::BadGatewayMac);
        }
        if !(MIN_PREFIX_LENGTH..=MAX_PREFIX_LENGTH).contains(&self.prefix_length) {
            return Err(ConfigError::PrefixLength(self.prefix_length));
        }
        for (name, address) in [
            ("gateway", self.gateway),
            ("DNS", self.dns),
            ("guest", self.guest),
        ] {
            if !self.is_host_address(address) {
                return Err(ConfigError::OutsideNetwork {
                    name,
                    address,
                    network: self.network,
                    prefix_length: self.prefix_length,
                });
            }
        }
        if self.gateway == self.dns || self.gateway == self.guest || self.dns == self.guest {
            return Err(ConfigError::DuplicateAddress);
        }
        if !(MIN_MTU..=MAX_MTU).contains(&self.mtu) {
            return Err(ConfigError::Mtu(self.mtu));
        }
        if self.dns_upstreams.len() > MAX_DNS_UPSTREAMS {
            return Err(ConfigError::TooManyUpstreams);
        }
        let limits = self.flow_limits;

        if limits.tcp_connections == 0 || limits.udp_mappings == 0 || limits.ping_flows == 0 {
            return Err(ConfigError::ZeroFlowLimit);
        }
        for (index, forward) in self.port_forwards.iter().enumerate() {
            if forward.guest_port == 0 || forward.host.port() == 0 {
                return Err(ConfigError::ZeroPort(forward.to_string()));
            }
            if self.port_forwards[..index]
                .iter()
                .any(|other| other.protocol == forward.protocol && other.host == forward.host)
            {
                return Err(ConfigError::Duplicate(forward.to_string()));
            }
        }
        for (index, service) in self.host_services.iter().enumerate() {
            if service.guest_port == 0 || service.host.port() == 0 {
                return Err(ConfigError::ZeroPort(service.to_string()));
            }
            if service.protocol == Protocol::Udp && !service.host.is_ipv4() {
                return Err(ConfigError::UdpServiceNotIpv4(service.to_string()));
            }
            if self.host_services[..index].iter().any(|other| {
                other.protocol == service.protocol && other.guest_port == service.guest_port
            }) {
                return Err(ConfigError::Duplicate(service.to_string()));
            }
        }
        Ok(())
    }

    pub fn port_forwards(&self) -> &[PortForward] {
        &self.port_forwards
    }

    /// The host endpoint behind gateway:guest_port, if the user allowed one.
    pub fn host_service(&self, protocol: Protocol, guest_port: u16) -> Option<SocketAddr> {
        self.host_services
            .iter()
            .find(|service| service.protocol == protocol && service.guest_port == guest_port)
            .map(|service| service.host)
    }

    pub fn guest_mac(&self) -> MacAddress {
        self.guest_mac
    }

    pub fn gateway_mac(&self) -> MacAddress {
        self.gateway_mac
    }

    pub fn gateway(&self) -> Ipv4Addr {
        self.gateway
    }

    pub fn dns(&self) -> Ipv4Addr {
        self.dns
    }

    pub fn guest(&self) -> Ipv4Addr {
        self.guest
    }

    pub fn mtu(&self) -> usize {
        self.mtu
    }

    pub fn dns_upstreams(&self) -> &[SocketAddr] {
        &self.dns_upstreams
    }

    pub fn netmask(&self) -> Ipv4Addr {
        Ipv4Addr::from(self.mask_bits())
    }

    pub fn broadcast(&self) -> Ipv4Addr {
        Ipv4Addr::from(u32::from(self.network) | !self.mask_bits())
    }

    /// True for addresses inside the network, including network and broadcast.
    pub fn contains(&self, address: Ipv4Addr) -> bool {
        u32::from(address) & self.mask_bits() == u32::from(self.network) & self.mask_bits()
    }

    /// The backend's own addresses: the gateway (host alias) and the DNS relay.
    pub fn is_backend_address(&self, address: Ipv4Addr) -> bool {
        address == self.gateway || address == self.dns
    }

    /// Addresses a guest may use as its own source: host addresses of the network
    /// other than the backend's.
    pub fn is_guest_address(&self, address: Ipv4Addr) -> bool {
        self.is_host_address(address) && !self.is_backend_address(address)
    }

    fn is_host_address(&self, address: Ipv4Addr) -> bool {
        let network = u32::from(self.network) & self.mask_bits();

        self.contains(address) && u32::from(address) != network && address != self.broadcast()
    }

    fn mask_bits(&self) -> u32 {
        u32::MAX << (IPV4_BITS - u32::from(self.prefix_length))
    }
}

/// Upstream resolvers from resolv.conf text: "nameserver" lines with an IPv4 or
/// IPv6 address (an IPv6 zone suffix is not supported and skipped), port 53, at
/// most MAX_DNS_UPSTREAMS, in file order.
pub fn resolv_conf_upstreams(text: &str) -> Vec<SocketAddr> {
    text.lines()
        .filter_map(|line| {
            let mut words = line.split_whitespace();

            match (words.next(), words.next()) {
                (Some("nameserver"), Some(address)) => address.parse::<IpAddr>().ok(),
                _ => None,
            }
        })
        .map(|address| SocketAddr::new(address, DNS_PORT))
        .take(MAX_DNS_UPSTREAMS)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    const GUEST_MAC: MacAddress = MacAddress([0x52, 0x54, 0x00, 0x12, 0x34, 0x56]);
    /// systemd-resolved's stub resolver, as on the development host.
    const RESOLVED_STUB: Ipv4Addr = Ipv4Addr::new(127, 0, 0, 53);
    const SAMPLE_RESOLV_CONF: &str = "# generated\n\
        nameserver 127.0.0.53\n\
        options edns0 trust-ad\n\
        nameserver   2001:db8::53\n\
        nameserver fe80::1%eth0\n\
        nameserver not-an-address\n\
        search home\n\
        nameserver 192.0.2.1\n\
        nameserver 192.0.2.2\n";

    fn config() -> NetworkConfig {
        NetworkConfig::slirp_compatible(GUEST_MAC, Vec::new()).unwrap()
    }

    #[test]
    fn slirp_layout_is_valid_and_derived_values_match() {
        let config = config();

        assert_eq!(config.netmask(), Ipv4Addr::new(255, 255, 255, 0));
        assert_eq!(config.broadcast(), Ipv4Addr::new(10, 0, 2, 255));
        assert!(config.contains(DEFAULT_GUEST));
        assert!(!config.contains(Ipv4Addr::new(10, 0, 3, 1)));
        assert!(config.is_backend_address(DEFAULT_GATEWAY));
        assert!(config.is_backend_address(DEFAULT_DNS));
        assert!(!config.is_backend_address(DEFAULT_GUEST));
        assert_eq!(config.mtu(), DEFAULT_MTU);
    }

    #[test]
    fn rejects_invalid_settings() {
        assert_eq!(
            NetworkConfig::slirp_compatible(MacAddress::BROADCAST, Vec::new()),
            Err(ConfigError::GroupGuestMac(MacAddress::BROADCAST))
        );
        assert_eq!(
            NetworkConfig::slirp_compatible(DEFAULT_GATEWAY_MAC, Vec::new()),
            Err(ConfigError::BadGatewayMac)
        );
        assert_eq!(
            config().with_mtu(MIN_MTU - 1),
            Err(ConfigError::Mtu(MIN_MTU - 1))
        );
        assert_eq!(
            config().with_mtu(MAX_MTU + 1),
            Err(ConfigError::Mtu(MAX_MTU + 1))
        );
        assert!(config().with_mtu(MAX_MTU).is_ok());
        let upstreams = vec![SocketAddr::new(IpAddr::V4(RESOLVED_STUB), DNS_PORT); 4];

        assert_eq!(
            NetworkConfig::slirp_compatible(GUEST_MAC, upstreams),
            Err(ConfigError::TooManyUpstreams)
        );
        let mut outside = config();

        outside.guest = Ipv4Addr::new(10, 0, 2, 255);
        assert!(matches!(
            outside.validate(),
            Err(ConfigError::OutsideNetwork { .. })
        ));
        let mut duplicate = config();

        duplicate.dns = DEFAULT_GATEWAY;
        assert_eq!(duplicate.validate(), Err(ConfigError::DuplicateAddress));
        let mut prefix = config();

        prefix.prefix_length = MAX_PREFIX_LENGTH + 1;
        assert_eq!(
            prefix.validate(),
            Err(ConfigError::PrefixLength(MAX_PREFIX_LENGTH + 1))
        );
    }

    #[test]
    fn parses_and_validates_forwards_and_services() {
        let forward: PortForward = "tcp:127.0.0.1:2222=22".parse().unwrap();
        let v6_forward: PortForward = "udp:[::1]:5353=53".parse().unwrap();
        let service: HostService = "tcp:12345=127.0.0.1:40000".parse().unwrap();

        assert_eq!(forward.protocol, Protocol::Tcp);
        assert_eq!(forward.host, "127.0.0.1:2222".parse().unwrap());
        assert_eq!(forward.guest_port, 22);
        assert_eq!(v6_forward.to_string(), "udp:[::1]:5353=53");
        assert_eq!(service.to_string(), "tcp:12345=127.0.0.1:40000");
        for bad in [
            "tcp:127.0.0.1:2222",
            "sctp:127.0.0.1:1=2",
            "tcp:localhost:1=2",
            "tcp:127.0.0.1:1=x",
        ] {
            assert!(bad.parse::<PortForward>().is_err(), "{bad}");
        }
        assert!("udp:53=[::1]:53".parse::<HostService>().is_ok());
        assert!("tcp:x=127.0.0.1:1".parse::<HostService>().is_err());

        let configured = config()
            .with_port_forwards(vec![forward, v6_forward])
            .unwrap()
            .with_host_services(vec![service])
            .unwrap();

        assert_eq!(configured.port_forwards().len(), 2);
        assert_eq!(
            configured.host_service(Protocol::Tcp, 12345),
            Some(service.host)
        );
        assert_eq!(configured.host_service(Protocol::Udp, 12345), None);
        assert!(matches!(
            config().with_port_forwards(vec![forward, forward]),
            Err(ConfigError::Duplicate(_))
        ));
        assert!(matches!(
            config().with_port_forwards(vec!["tcp:127.0.0.1:0=22".parse().unwrap()]),
            Err(ConfigError::ZeroPort(_))
        ));
        assert!(matches!(
            config().with_host_services(vec!["udp:53=[::1]:53".parse().unwrap()]),
            Err(ConfigError::UdpServiceNotIpv4(_))
        ));
    }

    #[test]
    fn reads_resolv_conf_upstreams() {
        let upstreams = resolv_conf_upstreams(SAMPLE_RESOLV_CONF);

        assert_eq!(
            upstreams,
            [
                "127.0.0.53:53".parse().unwrap(),
                "[2001:db8::53]:53".parse().unwrap(),
                "192.0.2.1:53".parse().unwrap(),
            ]
        );
        assert!(resolv_conf_upstreams("search home\n").is_empty());
    }
}
