/*
Module: qemu_lightning.opengl.reactos_venus_exchange
Purpose: Snapshot bounded private IOCTL bytes and reject malformed replies before publication.
Created: 2026-10-21
Architecture: Reuses the fixed QLV header and existing Rust operation policy; no kernel pointers or I/O.
*/
#include "reactos_venus_exchange.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace qemu_lightning::opengl::nt5 {
namespace {
bool framed(const QLV_HEADER& header) noexcept {
    return header.magic == QLV_MAGIC && header.version == QLV_VERSION &&
        header.reserved == 0 && header.payload_bytes <= QLV_MAX_PAYLOAD_BYTES;
}
bool header_plus(size_t payload, size_t& total) noexcept {
    if (payload > QLV_MAX_PAYLOAD_BYTES || payload > std::numeric_limits<size_t>::max() - sizeof(QLV_HEADER)) {
        return false;
    }
    total = sizeof(QLV_HEADER) + payload;
    return true;
}
}

bool exchange_lengths(const QLV_HEADER& request, size_t output_capacity, ExchangeLengths& result) noexcept {
    ExchangeLengths candidate{};

    if (!framed(request) || !header_plus(request.payload_bytes, candidate.input) ||
        !header_plus(output_capacity, candidate.output)) {
        return false;
    }
    candidate.storage = std::max(candidate.input, candidate.output);
    result = candidate;
    return true;
}

FrameStatus snapshot_exchange(const QLV_HEADER& request, const void* payload,
    size_t output_capacity, MutableBytes storage, ExchangeLengths& lengths) noexcept {
    ExchangeLengths candidate{};

    if (!exchange_lengths(request, output_capacity, candidate) ||
        (request.payload_bytes != 0 && payload == nullptr)) {
        return FrameStatus::invalid_request;
    }
    if (storage.data == nullptr || storage.size < candidate.storage) {
        return FrameStatus::insufficient_storage;
    }
    std::fill_n(storage.data, candidate.storage, static_cast<unsigned char>(0));
    std::memcpy(storage.data, &request, sizeof(request));

    if (request.payload_bytes != 0) {
        std::memcpy(storage.data + sizeof(request), payload, request.payload_bytes);
    }
    lengths = candidate;
    return FrameStatus::valid;
}

FrameStatus copy_exchange_reply(const QLV_HEADER& request, Bytes packet,
    MutableBytes output, QLV_HEADER& response) noexcept {
    if (!framed(request) || packet.data == nullptr || packet.size < sizeof(QLV_HEADER) ||
        output.size > QLV_MAX_PAYLOAD_BYTES || (output.size != 0 && output.data == nullptr)) {
        return FrameStatus::invalid_reply;
    }
    QLV_HEADER candidate{};
    std::memcpy(&candidate, packet.data, sizeof(candidate));
    size_t expected = 0;

    if (!framed(candidate) || candidate.operation != request.operation ||
        candidate.status < static_cast<int32_t>(QLV_STATUS_SUCCESS) ||
        candidate.status > static_cast<int32_t>(QLV_STATUS_TIMEOUT) ||
        (candidate.status != QLV_STATUS_SUCCESS && candidate.payload_bytes != 0) ||
        candidate.payload_bytes > output.size || !header_plus(candidate.payload_bytes, expected) ||
        packet.size != expected) {
        return FrameStatus::invalid_reply;
    }
    if (request.operation == QLV_OPEN) {
        if (request.context != 0 || (candidate.status == QLV_STATUS_SUCCESS && candidate.context == 0)) {
            return FrameStatus::invalid_reply;
        }
    } else if (candidate.context != request.context) {
        return FrameStatus::invalid_reply;
    }
    if (candidate.payload_bytes != 0) {
        std::memcpy(output.data, packet.data + sizeof(candidate), candidate.payload_bytes);
    }
    response = candidate;
    return FrameStatus::valid;
}

bool deferrable(const QLV_HEADER& request, size_t output_capacity) noexcept {
    return framed(request) && output_capacity == 0 &&
        ((request.operation == QLV_SUBMIT && request.offset != 0) || request.operation == QLV_WRITE_SYNC);
}

bool batch_trigger(const QLV_HEADER& request, size_t output_capacity) noexcept {
    return framed(request) && output_capacity == 0 && request.operation != QLV_OPEN &&
        request.operation != QLV_CLOSE && request.operation != QLV_GET_CAPS;
}

QLV_HEADER held_reply(const QLV_HEADER& request) noexcept {
    QLV_HEADER reply = request;
    reply.value = 0;
    reply.status = QLV_STATUS_SUCCESS;
    reply.payload_bytes = 0;
    return reply;
}

FrameStatus snapshot_batch(Bytes held, size_t held_count, const QLV_HEADER* trigger,
    const void* payload, MutableBytes storage, ExchangeLengths& lengths) noexcept {
    ExchangeLengths candidate{};
    ExchangeLengths last{};
    const size_t count = held_count + (trigger != nullptr ? 1 : 0);

    if (count == 0 || (held_count != 0 && held.data == nullptr) || held.size < held_count * sizeof(QLV_HEADER) ||
        held.size > sizeof(QLV_HEADER) + QLV_MAX_PAYLOAD_BYTES ||
        (trigger != nullptr && (!batch_trigger(*trigger, 0) || !exchange_lengths(*trigger, 0, last) ||
                                (trigger->payload_bytes != 0 && payload == nullptr))) ||
        last.input > sizeof(QLV_HEADER) + QLV_MAX_PAYLOAD_BYTES - held.size) {
        return FrameStatus::invalid_request;
    }
    candidate.input = held.size + last.input;
    candidate.output = count * sizeof(QLV_HEADER);
    candidate.storage = std::max(candidate.input, candidate.output);

    if (storage.data == nullptr || storage.size < candidate.storage) {
        return FrameStatus::insufficient_storage;
    }
    std::fill_n(storage.data, candidate.storage, static_cast<unsigned char>(0));
    if (held.size != 0) {
        std::memcpy(storage.data, held.data, held.size);
    }
    if (trigger != nullptr) {
        std::memcpy(storage.data + held.size, trigger, sizeof(*trigger));
        if (trigger->payload_bytes != 0) {
            std::memcpy(storage.data + held.size + sizeof(*trigger), payload, trigger->payload_bytes);
        }
    }
    lengths = candidate;
    return FrameStatus::valid;
}

BatchStatus copy_batch_reply(const QLV_HEADER* held, size_t held_count, const QLV_HEADER* trigger,
    Bytes replies, MutableBytes output, QLV_HEADER& response) noexcept {
    const size_t count = held_count + (trigger != nullptr ? 1 : 0);

    if (count == 0 || (held_count != 0 && held == nullptr) || replies.data == nullptr ||
        replies.size % sizeof(QLV_HEADER) != 0 || replies.size > count * sizeof(QLV_HEADER)) {
        return BatchStatus::invalid_reply;
    }
    for (size_t index = 0; index < held_count; ++index) {
        QLV_HEADER reply{};

        if ((index + 1) * sizeof(QLV_HEADER) > replies.size) {
            return BatchStatus::held_failed;
        }
        if (copy_exchange_reply(held[index], {replies.data + index * sizeof(QLV_HEADER), sizeof(QLV_HEADER)},
            {nullptr, 0}, reply) != FrameStatus::valid) {
            return BatchStatus::invalid_reply;
        }
        if (reply.status != QLV_STATUS_SUCCESS) {
            return BatchStatus::held_failed;
        }
    }
    if (trigger == nullptr) {
        return BatchStatus::valid;
    }
    if (replies.size != count * sizeof(QLV_HEADER)) {
        return BatchStatus::invalid_reply;
    }
    return copy_exchange_reply(*trigger, {replies.data + held_count * sizeof(QLV_HEADER), sizeof(QLV_HEADER)},
        output, response) == FrameStatus::valid ? BatchStatus::valid : BatchStatus::invalid_reply;
}
}
