/*
Module: qemu_lightning.opengl.reactos_venus_exchange_tests
Purpose: Exercise exact IOCTL snapshots, response bounds and context identity without native I/O.
Created: 2026-10-21
Architecture: CPU-only framing tests; fixtures do not simulate GPU completion or a usable renderer.
*/
#include "reactos_venus_exchange.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace qemu_lightning::opengl::nt5;
namespace {
constexpr uint64_t fixture_context = UINT64_C(0x0102030405060708);
constexpr unsigned char guard = 0xad;
constexpr size_t fixture_payload_bytes = 7;
constexpr size_t maximum_fixture_bytes = sizeof(QLV_HEADER) + 2 * fixture_payload_bytes;
enum class Alteration { context, operation, reserved, version, negative_status, unknown_status };
QLV_HEADER request() {
    QLV_HEADER value{};
    value.magic = QLV_MAGIC;
    value.version = QLV_VERSION;
    value.operation = QLV_GET_CAPS;
    value.context = fixture_context;
    value.status = QLV_STATUS_UNSUPPORTED;
    return value;
}
}

int main() {
    unsigned checks = 0;
    unsigned failed = 0;
    const auto check = [&](bool value, const char* text = "") {
        ++checks;
        if (!value) { ++failed; std::printf("FAIL %s\n", text); }
    };
    const std::array<unsigned char, fixture_payload_bytes> payload{1, 4, 9, 16, 25, 36, 49};
    auto source = request();
    source.payload_bytes = fixture_payload_bytes;
    ExchangeLengths lengths{};

    for (size_t output_capacity = 0; output_capacity <= 2 * fixture_payload_bytes; ++output_capacity) {
        const size_t needed = sizeof(QLV_HEADER) + std::max(payload.size(), output_capacity);

        for (size_t available = 0; available <= maximum_fixture_bytes; ++available) {
            std::array<unsigned char, maximum_fixture_bytes + 2> storage{};
            storage.fill(guard);
            const auto status = snapshot_exchange(source, payload.data(), output_capacity,
                {storage.data() + 1, available}, lengths);

            check(storage.front() == guard && storage[available + 1] == guard);
            if (available < needed) {
                check(status == FrameStatus::insufficient_storage && storage[1] == guard);
            } else {
                check(status == FrameStatus::valid && lengths.storage == needed &&
                    lengths.input == sizeof(QLV_HEADER) + payload.size() &&
                    lengths.output == sizeof(QLV_HEADER) + output_capacity);
                check(std::memcmp(storage.data() + 1, &source, sizeof(source)) == 0 &&
                    std::memcmp(storage.data() + 1 + sizeof(source), payload.data(), payload.size()) == 0);
                for (size_t index = lengths.input; index < lengths.storage; ++index) {
                    check(storage[1 + index] == 0);
                }
            }
        }
    }
    auto malformed = source;
    malformed.magic = 0;
    check(!exchange_lengths(malformed, 0, lengths));
    check(!exchange_lengths(source, std::numeric_limits<size_t>::max(), lengths));
    malformed = source;
    malformed.payload_bytes = QLV_MAX_PAYLOAD_BYTES + 1;
    check(!exchange_lengths(malformed, 0, lengths));
    check(snapshot_exchange(source, nullptr, 0, {nullptr, 0}, lengths) == FrameStatus::invalid_request);

    std::array<unsigned char, maximum_fixture_bytes> packet{};
    auto reply = request();
    reply.payload_bytes = fixture_payload_bytes;
    reply.status = QLV_STATUS_SUCCESS;
    std::memcpy(packet.data(), &reply, sizeof(reply));
    std::memcpy(packet.data() + sizeof(reply), payload.data(), payload.size());
    source = request();

    for (size_t returned = 0; returned <= packet.size(); ++returned) {
        for (size_t capacity = 0; capacity <= 2 * fixture_payload_bytes; ++capacity) {
            std::array<unsigned char, 2 * fixture_payload_bytes + 2> output{};
            output.fill(guard);
            QLV_HEADER decoded{};
            const auto status = copy_exchange_reply(source, {packet.data(), returned},
                {output.data() + 1, capacity}, decoded);
            const bool valid = returned == sizeof(reply) + payload.size() && capacity >= payload.size();

            check((status == FrameStatus::valid) == valid);
            check(output.front() == guard && output[capacity + 1] == guard);
            if (valid) {
                check(decoded.context == fixture_context && std::memcmp(output.data() + 1, payload.data(), payload.size()) == 0);
            } else {
                check(decoded.magic == 0 && output[1] == guard);
            }
        }
    }
    constexpr std::array<Alteration, 6> alterations{Alteration::context, Alteration::operation,
        Alteration::reserved, Alteration::version, Alteration::negative_status, Alteration::unknown_status};

    for (auto mutation : alterations) {
        auto altered = reply;

        switch (mutation) {
        case Alteration::context: altered.context ^= 1; break;
        case Alteration::operation: altered.operation = QLV_SUBMIT; break;
        case Alteration::reserved: altered.reserved = 1; break;
        case Alteration::version: altered.version = 0; break;
        case Alteration::negative_status: altered.status = -1; break;
        case Alteration::unknown_status: altered.status = QLV_STATUS_TIMEOUT + 1; break;
        }
        std::memcpy(packet.data(), &altered, sizeof(altered));
        std::array<unsigned char, fixture_payload_bytes> output{};
        QLV_HEADER decoded{};
        check(copy_exchange_reply(source, {packet.data(), sizeof(reply) + payload.size()},
            {output.data(), output.size()}, decoded) == FrameStatus::invalid_reply);
        check(decoded.magic == 0 && output[0] == 0);
    }
    source = request();
    source.operation = QLV_OPEN;
    source.context = 0;
    reply = source;
    reply.status = QLV_STATUS_SUCCESS;
    std::memcpy(packet.data(), &reply, sizeof(reply));
    QLV_HEADER decoded{};
    check(copy_exchange_reply(source, {packet.data(), sizeof(reply)}, {nullptr, 0}, decoded) == FrameStatus::invalid_reply);
    reply.context = fixture_context;
    std::memcpy(packet.data(), &reply, sizeof(reply));
    check(copy_exchange_reply(source, {packet.data(), sizeof(reply)}, {nullptr, 0}, decoded) == FrameStatus::valid && decoded.context == fixture_context);
    source = request();
    source.operation = QLV_WAIT_SYNCS;
    reply = source;
    reply.status = QLV_STATUS_TIMEOUT;
    std::memcpy(packet.data(), &reply, sizeof(reply));
    check(copy_exchange_reply(source, {packet.data(), sizeof(reply)}, {nullptr, 0}, decoded) == FrameStatus::valid && decoded.status == QLV_STATUS_TIMEOUT);
    source.operation = QLV_WATCH_SYNCS;
    reply = source;
    reply.status = QLV_STATUS_UNSUPPORTED;
    std::memcpy(packet.data(), &reply, sizeof(reply));
    check(copy_exchange_reply(source, {packet.data(), sizeof(reply)}, {nullptr, 0}, decoded) == FrameStatus::valid && decoded.status == QLV_STATUS_UNSUPPORTED);
    reply.payload_bytes = fixture_payload_bytes;
    std::memcpy(packet.data(), &reply, sizeof(reply));
    std::array<unsigned char, fixture_payload_bytes> error_payload{};
    check(copy_exchange_reply(source, {packet.data(), sizeof(reply) + payload.size()},
        {error_payload.data(), error_payload.size()}, decoded) == FrameStatus::invalid_reply);

    // Ring_batching: what may be held, what may end a batch, and the framing
    // of the batch IOCTL in both directions.
    auto submit = request();
    submit.operation = QLV_SUBMIT;
    submit.offset = 1;
    submit.payload_bytes = sizeof(QLV_SYNC_POINT);
    auto roundtrip = request();
    roundtrip.operation = QLV_SUBMIT;
    auto reset = request();
    reset.operation = QLV_WRITE_SYNC;
    reset.resource = 0x55;
    check(deferrable(submit, 0) && deferrable(reset, 0) && !deferrable(roundtrip, 0) && !deferrable(submit, 1));
    auto wait = request();
    wait.operation = QLV_WAIT_SYNCS;
    check(!deferrable(wait, 0) && !deferrable(request(), 0));
    malformed = reset;
    malformed.reserved = 1;
    check(!deferrable(malformed, 0) && !batch_trigger(malformed, 0));
    check(batch_trigger(roundtrip, 0) && batch_trigger(wait, 0) && batch_trigger(reset, 0) &&
        !batch_trigger(roundtrip, 1) && !batch_trigger(request(), 0));
    auto open_request = request();
    open_request.operation = QLV_OPEN;
    auto close_request = request();
    close_request.operation = QLV_CLOSE;
    check(!batch_trigger(open_request, 0) && !batch_trigger(close_request, 0));
    reset.value = 9;
    const QLV_HEADER held_answer = held_reply(reset);
    check(held_answer.status == QLV_STATUS_SUCCESS && held_answer.value == 0 && held_answer.payload_bytes == 0 &&
        held_answer.context == fixture_context && held_answer.resource == 0x55 && held_answer.operation == QLV_WRITE_SYNC);

    const QLV_SYNC_POINT point{0x77, 3};
    HeldRequests<2, 2 * sizeof(QLV_HEADER) + sizeof(QLV_SYNC_POINT)> held;
    check(!held.hold(roundtrip, nullptr) && !held.hold(submit, nullptr) && held.count() == 0,
        "only framed deferrable requests with their payload are held");
    check(held.hold(submit, &point) && held.hold(reset, nullptr) && held.count() == 2 &&
        held.bytes().size == 2 * sizeof(QLV_HEADER) + sizeof(point));
    check(!held.hold(reset, nullptr) && held.count() == 2, "a full store holds nothing more");
    check(std::memcmp(held.bytes().data, &submit, sizeof(submit)) == 0 &&
        std::memcmp(held.bytes().data + sizeof(submit), &point, sizeof(point)) == 0 &&
        std::memcmp(held.bytes().data + sizeof(submit) + sizeof(point), &reset, sizeof(reset)) == 0 &&
        held.headers()[1].operation == QLV_WRITE_SYNC, "held bytes are exact snapshots, oldest first");
    HeldRequests<4, sizeof(QLV_HEADER) + 8> small;
    check(!small.hold(submit, &point) && small.hold(reset, nullptr) && !small.hold(reset, nullptr),
        "the byte bound is exact");

    wait.bytes = 1;
    wait.payload_bytes = sizeof(QLV_SYNC_POINT);
    std::array<unsigned char, 6 * sizeof(QLV_HEADER)> batch{};
    batch.fill(guard);
    ExchangeLengths batch_lengths{};
    check(snapshot_batch(held.bytes(), held.count(), &wait, &point, {batch.data(), batch.size() - 1},
        batch_lengths) == FrameStatus::valid);
    check(batch_lengths.input == held.bytes().size + sizeof(QLV_HEADER) + sizeof(point) &&
        batch_lengths.output == 3 * sizeof(QLV_HEADER) && batch_lengths.storage == batch_lengths.input &&
        std::memcmp(batch.data(), held.bytes().data, held.bytes().size) == 0 &&
        std::memcmp(batch.data() + held.bytes().size, &wait, sizeof(wait)) == 0 &&
        std::memcmp(batch.data() + held.bytes().size + sizeof(wait), &point, sizeof(point)) == 0 &&
        batch[batch_lengths.storage] == guard, "held packets, then the trigger, then nothing else");
    check(snapshot_batch(held.bytes(), held.count(), &wait, &point, {batch.data(), batch_lengths.storage - 1},
        batch_lengths) == FrameStatus::insufficient_storage);
    check(snapshot_batch(held.bytes(), held.count(), &wait, nullptr, {batch.data(), batch.size()},
        batch_lengths) == FrameStatus::invalid_request);
    check(snapshot_batch(held.bytes(), held.count(), &close_request, nullptr, {batch.data(), batch.size()},
        batch_lengths) == FrameStatus::invalid_request, "CLOSE never ends a batch");
    check(snapshot_batch({nullptr, 0}, 0, nullptr, nullptr, {batch.data(), batch.size()},
        batch_lengths) == FrameStatus::invalid_request, "an empty batch is refused");
    check(snapshot_batch(held.bytes(), held.count(), nullptr, nullptr, {batch.data(), batch.size()},
        batch_lengths) == FrameStatus::valid && batch_lengths.input == held.bytes().size &&
        batch_lengths.output == 2 * sizeof(QLV_HEADER), "a flush sends the held packets alone");
    batch.fill(0);
    check(snapshot_batch(held.bytes(), held.count(), nullptr, nullptr, {batch.data(), batch.size()},
        batch_lengths) == FrameStatus::valid && batch[batch_lengths.storage - 1] == 0,
        "unused reply room is zeroed");

    // Replies: one header per executed packet.
    std::array<QLV_HEADER, 3> answers{held_reply(held.headers()[0]), held_reply(held.headers()[1]), wait};
    answers[2].status = QLV_STATUS_TIMEOUT;
    answers[2].payload_bytes = 0;
    std::array<unsigned char, 3 * sizeof(QLV_HEADER)> returned{};
    std::memcpy(returned.data(), answers.data(), sizeof(answers));
    QLV_HEADER batch_answer{};
    check(copy_batch_reply(held.headers(), 2, &wait, {returned.data(), returned.size()}, {nullptr, 0},
        batch_answer) == BatchStatus::valid && batch_answer.status == QLV_STATUS_TIMEOUT &&
        batch_answer.operation == QLV_WAIT_SYNCS, "the trigger's own reply is returned");
    check(copy_batch_reply(held.headers(), 2, nullptr, {returned.data(), 2 * sizeof(QLV_HEADER)}, {nullptr, 0},
        batch_answer) == BatchStatus::valid, "a flush succeeds when every held request did");
    check(copy_batch_reply(held.headers(), 2, &wait, {returned.data(), 2 * sizeof(QLV_HEADER)}, {nullptr, 0},
        batch_answer) == BatchStatus::invalid_reply, "a missing trigger reply is never invented");
    check(copy_batch_reply(held.headers(), 2, &wait, {returned.data(), sizeof(QLV_HEADER)}, {nullptr, 0},
        batch_answer) == BatchStatus::held_failed, "a held request that never ran is a failure");
    check(copy_batch_reply(held.headers(), 2, &wait, {returned.data(), returned.size() - 1}, {nullptr, 0},
        batch_answer) == BatchStatus::invalid_reply, "torn reply bytes are refused");
    answers[1].status = QLV_STATUS_INVALID;
    std::memcpy(returned.data(), answers.data(), 2 * sizeof(QLV_HEADER));
    check(copy_batch_reply(held.headers(), 2, &wait, {returned.data(), 2 * sizeof(QLV_HEADER)}, {nullptr, 0},
        batch_answer) == BatchStatus::held_failed, "a held request's host error is reported");
    answers[1].status = QLV_STATUS_SUCCESS;
    answers[1].context ^= 1;
    std::memcpy(returned.data(), answers.data(), 2 * sizeof(QLV_HEADER));
    check(copy_batch_reply(held.headers(), 2, &wait, {returned.data(), returned.size()}, {nullptr, 0},
        batch_answer) == BatchStatus::invalid_reply, "a reply for another context is refused");
    held.clear();
    check(held.count() == 0 && held.bytes().size == 0 && held.hold(reset, nullptr), "clear empties the store");
    std::printf("NT5 exchange framing: %u checks, %u failures\n", checks, failed);
    return failed == 0 ? 0 : 1;
}
