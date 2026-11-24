/*
Module: qemu_lightning.opengl.reactos_venus_exchange
Purpose: Bound the native Venus request snapshot and exact reply copy.
Created: 2026-10-21
Architecture: CPU-only framing for the owned NT5 exchange; it does not open a session or issue I/O.
*/
#pragma once
#include "transport.h"
#include <array>
#include <cstring>

namespace qemu_lightning::opengl::nt5 {
static_assert(sizeof(QLV_HEADER) == QLV_HEADER_BYTES, "Shared fixed QLV header layout");
struct MutableBytes final { unsigned char* data; size_t size; };
struct Bytes final { const unsigned char* data; size_t size; };
struct ExchangeLengths final { size_t storage; size_t input; size_t output; };
enum class FrameStatus { valid, invalid_request, insufficient_storage, invalid_reply };

// Checked IOCTL lengths for private storage. Input and output spans include the
// fixed header; no pointer or native-size field is serialized into the packet.
bool exchange_lengths(const QLV_HEADER& request, size_t output_capacity, ExchangeLengths& result) noexcept;

// The caller owns readable payload[request.payload_bytes] and private writable
// storage[lengths.storage]; these spans do not overlap. Storage is zeroed before
// publishing the exact input snapshot. No pointer is retained.
FrameStatus snapshot_exchange(const QLV_HEADER& request, const void* payload,
    size_t output_capacity, MutableBytes storage, ExchangeLengths& lengths) noexcept;

// The OS returned only packet.size initialized bytes. The output is independently
// owned writable storage and cannot alias packet/header. Nothing is copied on
// failure. Success here is framing only: wire status and operation-specific
// resource/mapping ownership still belong to the existing policy and kernel.
FrameStatus copy_exchange_reply(const QLV_HEADER& request, Bytes packet,
    MutableBytes output, QLV_HEADER& response) noexcept;

// Ring_batching (the kernel's ioctl_exchange_batch, lightning_abi.hpp). A
// deferrable request needs no reply before the runtime's next exchange: a
// SUBMIT that signals syncs (Venus always follows it at once with its
// roundtrip SUBMIT, and every observer of those syncs exchanges first) or a
// WRITE_SYNC. Holding it changes when the kernel sees it, never the order.
bool deferrable(const QLV_HEADER& request, size_t output_capacity) noexcept;
// A request that may end a batch: its reply is header only.
bool batch_trigger(const QLV_HEADER& request, size_t output_capacity) noexcept;
// What a held request reports at once: the kernel's success reply for it.
// A later failure surfaces as DEVICE_LOST on the exchange that sends it.
QLV_HEADER held_reply(const QLV_HEADER& request) noexcept;

// Exact snapshots of held requests, oldest first, in fixed storage.
template <size_t Packets, size_t Storage>
class HeldRequests final {
public:
    size_t count() const noexcept { return count_; }
    Bytes bytes() const noexcept { return {bytes_.data(), size_}; }
    const QLV_HEADER* headers() const noexcept { return headers_.data(); }
    void clear() noexcept { count_ = 0; size_ = 0; }
    // False (nothing held) when it is not deferrable or does not fit.
    bool hold(const QLV_HEADER& request, const void* payload) noexcept {
        ExchangeLengths lengths{};

        if (count_ == Packets || !deferrable(request, 0) || !exchange_lengths(request, 0, lengths) ||
            lengths.input > Storage - size_ || (request.payload_bytes != 0 && payload == nullptr)) {
            return false;
        }
        std::memcpy(bytes_.data() + size_, &request, sizeof(request));
        if (request.payload_bytes != 0) {
            std::memcpy(bytes_.data() + size_ + sizeof(request), payload, request.payload_bytes);
        }
        headers_[count_++] = request;
        size_ += lengths.input;
        return true;
    }

private:
    std::array<unsigned char, Storage> bytes_{};
    std::array<QLV_HEADER, Packets> headers_{};
    size_t count_ = 0, size_ = 0;
};

// Held bytes, then the trigger's snapshot (none for a flush), into private
// storage. lengths.input is the IOCTL input, lengths.output one reply header
// per packet and lengths.storage the larger; storage is zeroed first.
FrameStatus snapshot_batch(Bytes held, size_t held_count, const QLV_HEADER* trigger,
    const void* payload, MutableBytes storage, ExchangeLengths& lengths) noexcept;

enum class BatchStatus { valid, held_failed, invalid_reply };
// replies holds the initialized bytes the kernel returned: one header per
// executed packet. held_failed: a held request failed or never ran (the
// trigger then did not run either). valid: every held request succeeded and,
// with a trigger, its reply was copied as copy_exchange_reply does.
BatchStatus copy_batch_reply(const QLV_HEADER* held, size_t held_count, const QLV_HEADER* trigger,
    Bytes replies, MutableBytes output, QLV_HEADER& response) noexcept;
}
