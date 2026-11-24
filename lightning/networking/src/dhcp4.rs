//! Module: lightning.networking.dhcp4
//! Purpose: Encode DHCPv4 messages and serve the single lease of one VM (RFC 2131/2132).
//! Created: 2026-10-16
//! Architecture: The backend core passes guest UDP payloads for port 67 here and
//! sends the returned reply. The server is stateless: it owns exactly one address
//! (the configured guest lease) for exactly one client MAC (the VM's NIC), so there
//! is no lease table to persist or expire. Requests from other MACs are ignored.

use std::net::Ipv4Addr;

use thiserror::Error;

use crate::config::NetworkConfig;
use crate::packet::ethernet::MacAddress;
use crate::packet::read_array as field;

pub const SERVER_PORT: u16 = 67;
pub const CLIENT_PORT: u16 = 68;
/// Lease length handed to the guest; libslirp also uses one day.
pub const LEASE_SECONDS: u32 = 24 * 60 * 60;

/// The fixed BOOTP fields before the magic cookie.
const FIXED_BYTES: usize = 236;
const MAGIC_COOKIE: [u8; 4] = [99, 130, 83, 99];
const OPTIONS_OFFSET: usize = FIXED_BYTES + MAGIC_COOKIE.len();
/// Replies are padded to the BOOTP minimum, which some clients require (RFC 1542).
const MIN_MESSAGE_BYTES: usize = 300;

const OP_OFFSET: usize = 0;
const HARDWARE_TYPE_OFFSET: usize = 1;
const HARDWARE_LENGTH_OFFSET: usize = 2;
const TRANSACTION_ID_OFFSET: usize = 4;
const FLAGS_OFFSET: usize = 10;
const CLIENT_IP_OFFSET: usize = 12;
const YOUR_IP_OFFSET: usize = 16;
const SERVER_IP_OFFSET: usize = 20;
const CLIENT_HARDWARE_OFFSET: usize = 28;
const COOKIE_OFFSET: usize = FIXED_BYTES;

const OP_REQUEST: u8 = 1;
const OP_REPLY: u8 = 2;
const HARDWARE_TYPE_ETHERNET: u8 = 1;
const HARDWARE_LENGTH_ETHERNET: u8 = 6;
const IPV4_BYTES: usize = 4;
const BROADCAST_FLAG: u16 = 0x8000;

const OPTION_PAD: u8 = 0;
const OPTION_SUBNET_MASK: u8 = 1;
const OPTION_ROUTER: u8 = 3;
const OPTION_DNS: u8 = 6;
const OPTION_REQUESTED_IP: u8 = 50;
const OPTION_LEASE_TIME: u8 = 51;
const OPTION_MESSAGE_TYPE: u8 = 53;
const OPTION_SERVER_ID: u8 = 54;
const OPTION_END: u8 = 255;
/// Code and length bytes before each option value.
const OPTION_PREFIX_BYTES: usize = 2;

/// DHCP message types (option 53).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MessageType {
    Discover,
    Offer,
    Request,
    Decline,
    Ack,
    Nak,
    Release,
    Inform,
}

impl MessageType {
    /// The option 53 value (RFC 2132 section 9.6).
    fn code(self) -> u8 {
        match self {
            Self::Discover => 1,
            Self::Offer => 2,
            Self::Request => 3,
            Self::Decline => 4,
            Self::Ack => 5,
            Self::Nak => 6,
            Self::Release => 7,
            Self::Inform => 8,
        }
    }

    fn from_code(code: u8) -> Option<Self> {
        let kind = match code {
            1 => Self::Discover,
            2 => Self::Offer,
            3 => Self::Request,
            4 => Self::Decline,
            5 => Self::Ack,
            6 => Self::Nak,
            7 => Self::Release,
            8 => Self::Inform,
            _ => return None,
        };

        Some(kind)
    }
}

/// Whether a message travels from client to server or back.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    ClientToServer,
    ServerToClient,
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum DhcpError {
    #[error("DHCP message truncated: {0} bytes")]
    Truncated(usize),
    #[error("not an Ethernet DHCP message")]
    NotEthernet,
    #[error("unknown BOOTP operation {0}")]
    UnknownOperation(u8),
    #[error("missing DHCP magic cookie")]
    MissingCookie,
    #[error("malformed DHCP option {0}")]
    MalformedOption(u8),
    #[error("missing or unknown DHCP message type")]
    MessageType,
}

/// The fields of a DHCP message the backend reads or writes. Options the backend
/// does not use (host name, client identifier, parameter list...) are skipped on
/// parse; the server always answers with its full option set.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DhcpMessage {
    pub direction: Direction,
    pub message_type: MessageType,
    pub transaction_id: u32,
    pub broadcast: bool,
    pub client_ip: Ipv4Addr,
    pub your_ip: Ipv4Addr,
    pub server_ip: Ipv4Addr,
    pub client_mac: MacAddress,
    pub requested_ip: Option<Ipv4Addr>,
    pub server_id: Option<Ipv4Addr>,
    pub subnet_mask: Option<Ipv4Addr>,
    pub router: Option<Ipv4Addr>,
    pub dns_servers: Vec<Ipv4Addr>,
    pub lease_seconds: Option<u32>,
}

impl DhcpMessage {
    /// A client message with every server-side field empty; fixtures and tests
    /// fill in what they need.
    pub fn client(message_type: MessageType, transaction_id: u32, client_mac: MacAddress) -> Self {
        Self {
            direction: Direction::ClientToServer,
            message_type,
            transaction_id,
            broadcast: false,
            client_ip: Ipv4Addr::UNSPECIFIED,
            your_ip: Ipv4Addr::UNSPECIFIED,
            server_ip: Ipv4Addr::UNSPECIFIED,
            client_mac,
            requested_ip: None,
            server_id: None,
            subnet_mask: None,
            router: None,
            dns_servers: Vec::new(),
            lease_seconds: None,
        }
    }

    pub fn parse(bytes: &[u8]) -> Result<Self, DhcpError> {
        if bytes.len() < OPTIONS_OFFSET {
            return Err(DhcpError::Truncated(bytes.len()));
        }
        if bytes[HARDWARE_TYPE_OFFSET] != HARDWARE_TYPE_ETHERNET
            || bytes[HARDWARE_LENGTH_OFFSET] != HARDWARE_LENGTH_ETHERNET
        {
            return Err(DhcpError::NotEthernet);
        }
        let direction = match bytes[OP_OFFSET] {
            OP_REQUEST => Direction::ClientToServer,
            OP_REPLY => Direction::ServerToClient,
            other => return Err(DhcpError::UnknownOperation(other)),
        };

        if bytes[COOKIE_OFFSET..OPTIONS_OFFSET] != MAGIC_COOKIE {
            return Err(DhcpError::MissingCookie);
        }
        let mut message = Self::client(
            MessageType::Discover,
            u32::from_be_bytes(field(bytes, TRANSACTION_ID_OFFSET)),
            MacAddress(field(bytes, CLIENT_HARDWARE_OFFSET)),
        );
        let mut message_type = None;

        message.direction = direction;
        message.broadcast = u16::from_be_bytes(field(bytes, FLAGS_OFFSET)) & BROADCAST_FLAG != 0;
        message.client_ip = address(bytes, CLIENT_IP_OFFSET);
        message.your_ip = address(bytes, YOUR_IP_OFFSET);
        message.server_ip = address(bytes, SERVER_IP_OFFSET);
        for option in Options::new(&bytes[OPTIONS_OFFSET..]) {
            let (code, value) = option?;
            let single_address = || single_address(code, value);

            match code {
                OPTION_MESSAGE_TYPE => {
                    message_type = value.first().copied().and_then(MessageType::from_code);
                }
                OPTION_REQUESTED_IP => message.requested_ip = Some(single_address()?),
                OPTION_SERVER_ID => message.server_id = Some(single_address()?),
                OPTION_SUBNET_MASK => message.subnet_mask = Some(single_address()?),
                OPTION_ROUTER => {
                    message.router = addresses(code, value)?.first().copied();
                }
                OPTION_DNS => message.dns_servers = addresses(code, value)?,
                OPTION_LEASE_TIME => {
                    let seconds: [u8; 4] = value
                        .try_into()
                        .map_err(|_| DhcpError::MalformedOption(code))?;

                    message.lease_seconds = Some(u32::from_be_bytes(seconds));
                }
                _ => {}
            }
        }
        message.message_type = message_type.ok_or(DhcpError::MessageType)?;
        Ok(message)
    }

    /// Append the encoded message, padded to the BOOTP minimum.
    pub fn write(&self, output: &mut Vec<u8>) {
        let start = output.len();
        let op = match self.direction {
            Direction::ClientToServer => OP_REQUEST,
            Direction::ServerToClient => OP_REPLY,
        };
        let flags = if self.broadcast { BROADCAST_FLAG } else { 0 };

        output.resize(start + FIXED_BYTES, 0);

        let fixed = &mut output[start..];

        fixed[OP_OFFSET] = op;
        fixed[HARDWARE_TYPE_OFFSET] = HARDWARE_TYPE_ETHERNET;
        fixed[HARDWARE_LENGTH_OFFSET] = HARDWARE_LENGTH_ETHERNET;
        put(
            fixed,
            TRANSACTION_ID_OFFSET,
            &self.transaction_id.to_be_bytes(),
        );
        put(fixed, FLAGS_OFFSET, &flags.to_be_bytes());
        put(fixed, CLIENT_IP_OFFSET, &self.client_ip.octets());
        put(fixed, YOUR_IP_OFFSET, &self.your_ip.octets());
        put(fixed, SERVER_IP_OFFSET, &self.server_ip.octets());
        put(fixed, CLIENT_HARDWARE_OFFSET, &self.client_mac.octets());
        output.extend_from_slice(&MAGIC_COOKIE);
        write_option(output, OPTION_MESSAGE_TYPE, &[self.message_type.code()]);

        let single = [
            (OPTION_SERVER_ID, self.server_id),
            (OPTION_REQUESTED_IP, self.requested_ip),
            (OPTION_SUBNET_MASK, self.subnet_mask),
            (OPTION_ROUTER, self.router),
        ];

        for (code, value) in single {
            if let Some(value) = value {
                write_option(output, code, &value.octets());
            }
        }
        if !self.dns_servers.is_empty() {
            let servers: Vec<u8> = self
                .dns_servers
                .iter()
                .flat_map(|server| server.octets())
                .collect();

            write_option(output, OPTION_DNS, &servers);
        }
        if let Some(seconds) = self.lease_seconds {
            write_option(output, OPTION_LEASE_TIME, &seconds.to_be_bytes());
        }
        output.push(OPTION_END);
        if output.len() - start < MIN_MESSAGE_BYTES {
            output.resize(start + MIN_MESSAGE_BYTES, OPTION_PAD);
        }
    }
}

/// Where the server must send a reply (RFC 2131 section 4.1).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReplyDestination {
    pub ip: Ipv4Addr,
    pub mac: MacAddress,
}

/// The single-lease server for one VM.
#[derive(Clone, Debug)]
pub struct DhcpServer {
    client_mac: MacAddress,
    lease: Ipv4Addr,
    server: Ipv4Addr,
    netmask: Ipv4Addr,
    dns: Ipv4Addr,
}

impl DhcpServer {
    /// The gateway acts as the DHCP server identifier and router, as in libslirp.
    pub fn new(config: &NetworkConfig) -> Self {
        Self {
            client_mac: config.guest_mac(),
            lease: config.guest(),
            server: config.gateway(),
            netmask: config.netmask(),
            dns: config.dns(),
        }
    }

    /// The server's answer to a client message, if any. Messages from other MACs,
    /// for other servers, server-to-client messages, DECLINE and RELEASE get none.
    pub fn respond(&self, request: &DhcpMessage) -> Option<(DhcpMessage, ReplyDestination)> {
        if request.direction != Direction::ClientToServer || request.client_mac != self.client_mac {
            return None;
        }
        let reply_type = match request.message_type {
            MessageType::Discover => MessageType::Offer,
            MessageType::Request => {
                if request
                    .server_id
                    .is_some_and(|server| server != self.server)
                {
                    return None;
                }
                let wanted = request
                    .requested_ip
                    .or((request.client_ip != Ipv4Addr::UNSPECIFIED).then_some(request.client_ip));

                if wanted == Some(self.lease) {
                    MessageType::Ack
                } else {
                    MessageType::Nak
                }
            }
            MessageType::Inform => MessageType::Ack,
            _ => return None,
        };
        let mut reply = DhcpMessage::client(reply_type, request.transaction_id, self.client_mac);

        reply.direction = Direction::ServerToClient;
        reply.broadcast = request.broadcast;
        reply.server_id = Some(self.server);
        if reply_type != MessageType::Nak {
            let informing = request.message_type == MessageType::Inform;

            reply.client_ip = request.client_ip;
            reply.your_ip = if informing {
                Ipv4Addr::UNSPECIFIED
            } else {
                self.lease
            };
            reply.server_ip = self.server;
            reply.subnet_mask = Some(self.netmask);
            reply.router = Some(self.server);
            reply.dns_servers = vec![self.dns];
            reply.lease_seconds = (!informing).then_some(LEASE_SECONDS);
        }
        let destination = self.destination(request, &reply);

        Some((reply, destination))
    }

    fn destination(&self, request: &DhcpMessage, reply: &DhcpMessage) -> ReplyDestination {
        let broadcast = ReplyDestination {
            ip: Ipv4Addr::BROADCAST,
            mac: MacAddress::BROADCAST,
        };

        if reply.message_type == MessageType::Nak {
            broadcast
        } else if request.client_ip != Ipv4Addr::UNSPECIFIED {
            ReplyDestination {
                ip: request.client_ip,
                mac: self.client_mac,
            }
        } else if request.broadcast {
            broadcast
        } else {
            ReplyDestination {
                ip: reply.your_ip,
                mac: self.client_mac,
            }
        }
    }
}

/// Iterator over (code, value) pairs of the options area. Stops at END; yields an
/// error and stops at an option whose length runs past the message.
struct Options<'a> {
    remaining: &'a [u8],
}

impl<'a> Options<'a> {
    fn new(remaining: &'a [u8]) -> Self {
        Self { remaining }
    }
}

impl<'a> Iterator for Options<'a> {
    type Item = Result<(u8, &'a [u8]), DhcpError>;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            let (&code, rest) = self.remaining.split_first()?;

            match code {
                OPTION_END => {
                    self.remaining = &[];
                    return None;
                }
                OPTION_PAD => self.remaining = rest,
                _ => {
                    let length = rest.first().map(|&length| usize::from(length));
                    let end = length.map(|length| OPTION_PREFIX_BYTES + length);

                    let Some(end) = end.filter(|&end| end <= self.remaining.len()) else {
                        self.remaining = &[];
                        return Some(Err(DhcpError::MalformedOption(code)));
                    };
                    let value = &self.remaining[OPTION_PREFIX_BYTES..end];

                    self.remaining = &self.remaining[end..];
                    return Some(Ok((code, value)));
                }
            }
        }
    }
}

fn address(bytes: &[u8], offset: usize) -> Ipv4Addr {
    Ipv4Addr::from(field::<IPV4_BYTES>(bytes, offset))
}

fn put(fixed: &mut [u8], offset: usize, value: &[u8]) {
    fixed[offset..offset + value.len()].copy_from_slice(value);
}

fn single_address(code: u8, value: &[u8]) -> Result<Ipv4Addr, DhcpError> {
    let octets: [u8; IPV4_BYTES] = value
        .try_into()
        .map_err(|_| DhcpError::MalformedOption(code))?;

    Ok(Ipv4Addr::from(octets))
}

fn addresses(code: u8, value: &[u8]) -> Result<Vec<Ipv4Addr>, DhcpError> {
    if value.is_empty() || value.len() % IPV4_BYTES != 0 {
        return Err(DhcpError::MalformedOption(code));
    }
    Ok(value
        .chunks_exact(IPV4_BYTES)
        .map(|octets| Ipv4Addr::new(octets[0], octets[1], octets[2], octets[3]))
        .collect())
}

fn write_option(output: &mut Vec<u8>, code: u8, value: &[u8]) {
    output.push(code);
    output.push(value.len() as u8);
    output.extend_from_slice(value);
}

#[cfg(test)]
mod tests {
    use super::*;

    const GUEST_MAC: MacAddress = MacAddress([0x52, 0x54, 0x00, 0x12, 0x34, 0x56]);
    const OTHER_MAC: MacAddress = MacAddress([0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc]);
    const TRANSACTION: u32 = 0x3903_f326;
    const STALE_ADDRESS: Ipv4Addr = Ipv4Addr::new(192, 168, 122, 50);
    const OTHER_SERVER: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 99);

    fn server() -> (DhcpServer, NetworkConfig) {
        let config = NetworkConfig::slirp_compatible(GUEST_MAC, Vec::new()).unwrap();

        (DhcpServer::new(&config), config)
    }

    fn round_trip(message: &DhcpMessage) -> DhcpMessage {
        let mut bytes = Vec::new();

        message.write(&mut bytes);
        assert!(bytes.len() >= MIN_MESSAGE_BYTES);
        DhcpMessage::parse(&bytes).unwrap()
    }

    #[test]
    fn message_types_number_one_to_eight() {
        assert_eq!(MessageType::Discover.code(), 1);
        assert_eq!(MessageType::Inform.code(), 8);
        assert_eq!(MessageType::from_code(5), Some(MessageType::Ack));
        assert_eq!(MessageType::from_code(0), None);
        assert_eq!(MessageType::from_code(9), None);
    }

    #[test]
    fn discover_request_ack_cycle() {
        let (server, config) = server();
        let mut discover = DhcpMessage::client(MessageType::Discover, TRANSACTION, GUEST_MAC);

        discover.broadcast = true;
        let (offer, destination) = server.respond(&round_trip(&discover)).unwrap();
        let offer = round_trip(&offer);

        assert_eq!(offer.message_type, MessageType::Offer);
        assert_eq!(offer.direction, Direction::ServerToClient);
        assert_eq!(offer.transaction_id, TRANSACTION);
        assert_eq!(offer.your_ip, config.guest());
        assert_eq!(offer.server_id, Some(config.gateway()));
        assert_eq!(offer.router, Some(config.gateway()));
        assert_eq!(offer.subnet_mask, Some(config.netmask()));
        assert_eq!(offer.dns_servers, [config.dns()]);
        assert_eq!(offer.lease_seconds, Some(LEASE_SECONDS));
        assert_eq!(destination.ip, Ipv4Addr::BROADCAST);

        let mut request = DhcpMessage::client(MessageType::Request, TRANSACTION, GUEST_MAC);

        request.requested_ip = Some(offer.your_ip);
        request.server_id = offer.server_id;
        let (ack, destination) = server.respond(&round_trip(&request)).unwrap();

        assert_eq!(ack.message_type, MessageType::Ack);
        assert_eq!(ack.your_ip, config.guest());
        assert_eq!(
            destination,
            ReplyDestination {
                ip: config.guest(),
                mac: GUEST_MAC
            }
        );
    }

    #[test]
    fn renewal_is_unicast_to_client_address() {
        let (server, config) = server();
        let mut renew = DhcpMessage::client(MessageType::Request, TRANSACTION, GUEST_MAC);

        renew.client_ip = config.guest();
        let (ack, destination) = server.respond(&renew).unwrap();

        assert_eq!(ack.message_type, MessageType::Ack);
        assert_eq!(
            destination,
            ReplyDestination {
                ip: config.guest(),
                mac: GUEST_MAC
            }
        );
    }

    #[test]
    fn wrong_address_is_refused_with_broadcast_nak() {
        let (server, _) = server();
        let mut request = DhcpMessage::client(MessageType::Request, TRANSACTION, GUEST_MAC);

        request.requested_ip = Some(STALE_ADDRESS);
        let (nak, destination) = server.respond(&request).unwrap();

        assert_eq!(nak.message_type, MessageType::Nak);
        assert_eq!(nak.your_ip, Ipv4Addr::UNSPECIFIED);
        assert_eq!(nak.lease_seconds, None);
        assert_eq!(destination.mac, MacAddress::BROADCAST);
    }

    #[test]
    fn inform_gets_options_without_a_lease() {
        let (server, config) = server();
        let mut inform = DhcpMessage::client(MessageType::Inform, TRANSACTION, GUEST_MAC);

        inform.client_ip = config.guest();
        let (ack, _) = server.respond(&inform).unwrap();

        assert_eq!(ack.message_type, MessageType::Ack);
        assert_eq!(ack.your_ip, Ipv4Addr::UNSPECIFIED);
        assert_eq!(ack.lease_seconds, None);
        assert_eq!(ack.dns_servers, [config.dns()]);
    }

    #[test]
    fn ignores_foreign_clients_servers_and_releases() {
        let (server, config) = server();
        let foreign = DhcpMessage::client(MessageType::Discover, TRANSACTION, OTHER_MAC);
        let mut elsewhere = DhcpMessage::client(MessageType::Request, TRANSACTION, GUEST_MAC);
        let release = DhcpMessage::client(MessageType::Release, TRANSACTION, GUEST_MAC);
        let mut reply = DhcpMessage::client(MessageType::Offer, TRANSACTION, GUEST_MAC);

        elsewhere.requested_ip = Some(config.guest());
        elsewhere.server_id = Some(OTHER_SERVER);
        reply.direction = Direction::ServerToClient;
        for message in [foreign, elsewhere, release, reply] {
            assert!(server.respond(&message).is_none(), "{message:?}");
        }
    }

    #[test]
    fn rejects_malformed_messages() {
        let discover = DhcpMessage::client(MessageType::Discover, TRANSACTION, GUEST_MAC);
        let mut bytes = Vec::new();

        discover.write(&mut bytes);
        assert_eq!(
            DhcpMessage::parse(&bytes[..OPTIONS_OFFSET - 1]),
            Err(DhcpError::Truncated(OPTIONS_OFFSET - 1))
        );
        let mut cookie = bytes.clone();

        cookie[COOKIE_OFFSET] = 0;
        assert_eq!(DhcpMessage::parse(&cookie), Err(DhcpError::MissingCookie));
        let mut overrun = bytes[..OPTIONS_OFFSET].to_vec();

        overrun.extend_from_slice(&[OPTION_MESSAGE_TYPE, 1, 1, OPTION_DNS, 8, 10, 0]);
        assert_eq!(
            DhcpMessage::parse(&overrun),
            Err(DhcpError::MalformedOption(OPTION_DNS))
        );
        let mut untyped = bytes[..OPTIONS_OFFSET].to_vec();

        untyped.push(OPTION_END);
        assert_eq!(DhcpMessage::parse(&untyped), Err(DhcpError::MessageType));
        let mut operation = bytes;

        operation[OP_OFFSET] = 3;
        assert_eq!(
            DhcpMessage::parse(&operation),
            Err(DhcpError::UnknownOperation(3))
        );
    }
}
