//! Module: lightning.networking.tokens
//! Purpose: Allocate mio tokens for host sockets and remember which flow owns each.
//! Created: 2026-10-16
//! Architecture: The event loop keeps tokens below FIRST_DYNAMIC_TOKEN for its own
//! sockets (QEMU stream, DNS upstreams). Every translator socket gets a token here,
//! and the event loop hands readiness for such tokens back to the backend, which
//! looks up the owner. Freed tokens are reused.

use std::collections::HashMap;

use mio::Token;

/// Tokens below this belong to the event loop.
pub const FIRST_DYNAMIC_TOKEN: usize = 16;

/// What a translator socket belongs to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Owner {
    TcpConnection(u64),
    TcpListener(u64),
    UdpMapping(u64),
    UdpListener(u64),
    Ping(u64),
}

#[derive(Debug, Default)]
pub struct TokenMap {
    owners: HashMap<usize, Owner>,
    free: Vec<usize>,
    next: usize,
}

impl TokenMap {
    pub fn new() -> Self {
        Self {
            next: FIRST_DYNAMIC_TOKEN,
            ..Self::default()
        }
    }

    pub fn allocate(&mut self, owner: Owner) -> Token {
        let index = self.free.pop().unwrap_or_else(|| {
            self.next += 1;
            self.next - 1
        });

        self.owners.insert(index, owner);
        Token(index)
    }

    pub fn release(&mut self, token: Token) {
        if self.owners.remove(&token.0).is_some() {
            self.free.push(token.0);
        }
    }

    pub fn owner(&self, token: Token) -> Option<Owner> {
        self.owners.get(&token.0).copied()
    }

    pub fn len(&self) -> usize {
        self.owners.len()
    }

    pub fn is_empty(&self) -> bool {
        self.owners.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allocates_above_reserved_range_and_reuses() {
        let mut tokens = TokenMap::new();
        let first = tokens.allocate(Owner::Ping(1));
        let second = tokens.allocate(Owner::TcpConnection(2));

        assert_eq!(first, Token(FIRST_DYNAMIC_TOKEN));
        assert_eq!(second, Token(FIRST_DYNAMIC_TOKEN + 1));
        assert_eq!(tokens.owner(second), Some(Owner::TcpConnection(2)));
        tokens.release(first);
        tokens.release(first);
        assert_eq!(tokens.owner(first), None);
        assert_eq!(tokens.allocate(Owner::UdpMapping(3)), first);
        assert_eq!(tokens.len(), 2);
        assert!(!tokens.is_empty());
    }
}
