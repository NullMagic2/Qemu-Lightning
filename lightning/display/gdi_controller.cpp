/*
Module: lightning.gdi.controller
Purpose: Snapshot bounded guest commands and publish only observed GPU completion.
Created: 2026-10-21
Architecture: BQL owns queue/state; existing main-loop GL owner calls pump.
License: GPL-2.0-or-later.
*/
#include <algorithm>
#include <memory>
#include <new>
#include "hw/misc/lightning-gdi-controller.h"
#include "gdi_protocol.hpp"
#include "gdi_damage.hpp"
#include "common/byte_view.hpp"

namespace {
using namespace lightning::gdi;
constexpr uint32_t pump_budget = 8;

struct QueuedCommand final {
    Command command{};
    std::unique_ptr<uint8_t[]> pixels{};
    uint32_t retained_bytes = 0;
    uint64_t owner_ticket = 0;
    uint64_t gpu_ticket = 0;
    bool submitted = false;
    // Never issued: a later accepted replaceable copy writes all its pixels.
    bool superseded = false;
};

bool valid_window(const uint8_t* bar4, size_t bytes) noexcept {
    return bar4 != nullptr && LIGHTNING_GDI_TRANSFER_OFFSET <= bytes &&
        LIGHTNING_GDI_TRANSFER_BYTES <= bytes - LIGHTNING_GDI_TRANSFER_OFFSET;
}

bool mutation(uint32_t operation) noexcept {
    return operation == LIGHTNING_GDI_FILL || operation == LIGHTNING_GDI_COPY ||
        operation == LIGHTNING_GDI_UPLOAD || operation == LIGHTNING_GDI_BLEND_UPLOAD ||
        operation == LIGHTNING_GDI_ROP3 || operation == LIGHTNING_GDI_RASTER ||
        operation == LIGHTNING_GDI_GRADIENT || operation == LIGHTNING_GDI_EXTERNAL_IMAGE;
}

bool replaceable(const LightningGdiCommand& header) noexcept {
    return header.opcode == LIGHTNING_GDI_EXTERNAL_IMAGE && (header.flags & LIGHTNING_GDI_EXTERNAL_REPLACEABLE) != 0;
}
} // namespace

struct LightningGdiController final {
    CanvasState state{};
    TransferLease window{};
    QueuedCommand commands[LIGHTNING_GDI_MAX_PENDING_FENCES]{};
    uint32_t head = 0;
    uint32_t count = 0;
    uint32_t retained_bytes = 0;
    uint32_t status = LIGHTNING_GDI_IDLE;
    uint32_t result = LIGHTNING_GDI_OK;
    uint32_t reset_generation = 1;
    uint32_t readback_sequence = 0;
    Mode readback_mode{};
    LightningGdiFrame published{};
    DamageRegion damage{};
    LightningGdiFrame snapshot{};
    bool snapshot_in_flight = false;
    bool available = false;
    bool external_available = false;
    bool mailbox = false;
    bool reset_requested = false;
    bool retired = true;
    uint64_t replaceable_presents = 0;
    uint64_t superseded_presents = 0;
};

namespace {

void reject(LightningGdiController& controller, Status reason) noexcept {
    controller.result = static_cast<uint32_t>(reason);
    controller.status = controller.state.poisoned() ? LIGHTNING_GDI_LOST : LIGHTNING_GDI_REJECTED;
}

void lost(LightningGdiController& controller) noexcept {
    controller.available = false;
    controller.state.poison();
    controller.window.uncertain();
    controller.status = LIGHTNING_GDI_LOST;
    controller.result = LIGHTNING_GDI_GPU_FAILURE;
    controller.retired = false;
}

void clear_retired(LightningGdiController& controller) noexcept {
    for (auto& command : controller.commands) {
        command = {};
    }

    controller.state.retire_after_backend_release();
    controller.window.retire_after_backend_release();
    controller.head = 0;
    controller.count = 0;
    controller.retained_bytes = 0;
    controller.readback_sequence = 0;
    controller.readback_mode = {};
    controller.published = {};
    controller.damage = {};
    controller.snapshot = {};
    controller.snapshot_in_flight = false;
    controller.reset_requested = false;

    if (controller.reset_generation == UINT32_MAX) {
        // Never wrap a retirement token into an old native lease's identity.
        lost(controller);
        return;
    }

    ++controller.reset_generation;
    controller.status = LIGHTNING_GDI_IDLE;
    controller.result = LIGHTNING_GDI_OK;
    controller.retired = true;
}

enum class Retirement { retired, waiting, failed };

// Retire the oldest command once its actual completion is observed. A
// superseded copy has no GPU ticket and completes in its FIFO position, so the
// ordered prefix still proves every earlier source read retired. It adds no
// damage: the copy that superseded it covers every pixel it would have written.
Retirement retire_front(LightningGdiController& controller, LightningGdiGpu* gpu,
    uint8_t* bar4, size_t bar4_bytes) noexcept {
    auto& entry = controller.commands[controller.head];
    const auto& command = entry.command.header;
    LightningGdiGpuResult completion;

    if (controller.count == 0 || !entry.submitted || (!entry.superseded && gpu == nullptr)) {
        return Retirement::waiting;
    }

    if (entry.superseded) {
        completion = LIGHTNING_GDI_GPU_READY;
    } else if (command.opcode == LIGHTNING_GDI_READBACK) {
        if (!valid_window(bar4, bar4_bytes)) {
            lost(controller);
            return Retirement::failed;
        }

        completion = lightning_gdi_gpu_readback_copy(gpu, entry.gpu_ticket,
            bar4 + LIGHTNING_GDI_TRANSFER_OFFSET + LIGHTNING_GDI_PIXEL_OFFSET,
            LIGHTNING_GDI_MAX_PIXEL_BYTES);
    } else {
        completion = lightning_gdi_gpu_poll(gpu, entry.gpu_ticket);
    }

    if (completion == LIGHTNING_GDI_GPU_PENDING || completion == LIGHTNING_GDI_GPU_BUSY) {
        return Retirement::waiting;
    }

    if (completion != LIGHTNING_GDI_GPU_READY ||
        controller.state.completed(command.sequence, entry.owner_ticket) != Status::validated) {
        lost(controller);
        return Retirement::failed;
    }

    // Surface mutations are never console damage: only the canvas publishes.
    if (mutation(command.opcode) && !entry.superseded && target_surface(command.surfaces) == 0 &&
        !controller.damage.add_batch(command.destination, entry.command.clips, command.clip_count)) {
        lost(controller);
        return Retirement::failed;
    }
    if (command.opcode == LIGHTNING_GDI_READBACK) {
        controller.readback_mode = controller.state.mode();
        controller.readback_sequence = command.sequence;

        if (!controller.window.completed(controller.readback_mode, command.sequence)) {
            lost(controller);
            return Retirement::failed;
        }
    }

    entry = {};
    controller.head = (controller.head + 1) % LIGHTNING_GDI_MAX_PENDING_FENCES;
    --controller.count;
    return Retirement::retired;
}

// Latest wins only between two consecutive accepted replaceable copies: no
// other command may observe the canvas between them, and the newer one must
// write every pixel of the older one, which was never issued. The canvas after
// the newer copy is then identical with or without the older one.
void supersede(LightningGdiController& controller, const QueuedCommand& latest) noexcept {
    if (controller.count < 2) {
        return;
    }

    auto& previous = controller.commands[(controller.head + controller.count - 2) % LIGHTNING_GDI_MAX_PENDING_FENCES];
    const auto& older = previous.command.header;
    const auto& newer = latest.command.header;
    const Mode mode = controller.state.mode();

    if (previous.submitted || !replaceable(older) ||
        !batch_covers(mode.width, mode.height, newer.destination, latest.command.clips, newer.clip_count,
            older.destination, previous.command.clips, older.clip_count)) {
        return;
    }

    previous.submitted = true;
    previous.superseded = true;
    ++controller.superseded_presents;

    // Without earlier GPU work in flight the receipt retires inside this SUBMIT.
    while (controller.commands[controller.head].superseded &&
           retire_front(controller, nullptr, nullptr, 0) == Retirement::retired) {
    }
}

void submit(LightningGdiController& controller, const uint8_t* bar4, size_t bar4_bytes,
            uint32_t actual_width, uint32_t actual_height) noexcept {
    if (!controller.available || controller.reset_requested || controller.state.poisoned()) {
        reject(controller, controller.state.poisoned() ? Status::gpu_failure : Status::not_ready);
        return;
    }

    if (!valid_window(bar4, bar4_bytes)) {
        reject(controller, Status::bad_packet);
        return;
    }

    if (controller.window.phase() == LeasePhase::readback_ready) {
        // A subsequent native doorbell explicitly releases its previous output.
        // Actual GPU writes retired before this phase; the guest must have copied
        // that output before reusing the fixed transfer window for another input.
        if (!controller.window.copied(controller.readback_mode, controller.readback_sequence)) {
            lost(controller);
            return;
        }
    }

    if (controller.window.phase() != LeasePhase::free || controller.count == LIGHTNING_GDI_MAX_PENDING_FENCES) {
        reject(controller, Status::busy);
        return;
    }

    // This is the native shared-memory boundary. Bounds cover the complete fixed
    // source window before forming its pointer. Only private initialized copies
    // escape this function; a racing guest can corrupt its own image, not lengths.
    const uint8_t* transfer = bar4 + LIGHTNING_GDI_TRANSFER_OFFSET;
    uint8_t metadata[LIGHTNING_GDI_MAX_METADATA_BYTES]{};
    std::copy_n(transfer, LIGHTNING_GDI_COMMAND_BYTES, metadata);
    const rx7900::ByteView header(metadata, LIGHTNING_GDI_COMMAND_BYTES);
    uint32_t clip_count = 0;
    uint32_t metadata_bytes = 0;
    uint32_t opcode = 0;
    uint32_t expected_metadata = 0;

    if (!header.read_u32(offsetof(LightningGdiCommand, clip_count), clip_count) ||
        !header.read_u32(offsetof(LightningGdiCommand, metadata_bytes), metadata_bytes) ||
        !header.read_u32(offsetof(LightningGdiCommand, opcode), opcode) ||
        !metadata_size(opcode, clip_count, expected_metadata) || metadata_bytes != expected_metadata) {
        reject(controller, Status::bad_packet);
        return;
    }

    std::copy_n(transfer + LIGHTNING_GDI_COMMAND_BYTES, metadata_bytes - LIGHTNING_GDI_COMMAND_BYTES,
        metadata + LIGHTNING_GDI_COMMAND_BYTES);
    Command command;
    const Status decoded = decode_command(metadata, metadata_bytes, command);

    if (decoded != Status::validated) {
        reject(controller, decoded);
        return;
    }

    const auto& packet = command.header;
    if (packet.opcode == LIGHTNING_GDI_EXTERNAL_IMAGE &&
        (!controller.external_available || (replaceable(packet) && !controller.mailbox))) {
        reject(controller, Status::unsupported);
        return;
    }
    CanvasState candidate = controller.state;

    if (packet.opcode == LIGHTNING_GDI_BIND) {
        // A new canvas cannot replace an initialized GPU owner or a pending UI
        // lease merely because its command queue drained. Explicit real reset
        // is the only ownership transition, including same-size/new-key BIND.
        if (!controller.retired) { reject(controller, Status::busy); return; }
        if (packet.destination.x != 0 || packet.destination.y != 0 ||
            packet.destination.width != actual_width || packet.destination.height != actual_height) {
            reject(controller, Status::stale_mode);
            return;
        }

        const Status configured = candidate.configure({packet.mode_generation, packet.epoch, actual_width, actual_height});

        if (configured != Status::validated) {
            reject(controller, configured);
            return;
        }
    }

    const Status validated = candidate.validate(command);

    if (validated != Status::validated) {
        reject(controller, validated);
        return;
    }

    const bool input_pixels = packet.opcode == LIGHTNING_GDI_UPLOAD || packet.opcode == LIGHTNING_GDI_BLEND_UPLOAD ||
        (packet.opcode == LIGHTNING_GDI_ROP3 && packet.flags == LIGHTNING_GDI_ROP_SOURCE_UPLOAD) ||
        packet.opcode == LIGHTNING_GDI_RASTER;
    const uint32_t pixel_bytes = input_pixels ? packet.pixel_bytes : 0;

    if (pixel_bytes > LIGHTNING_GDI_MAX_PENDING_PIXEL_BYTES - controller.retained_bytes) {
        reject(controller, Status::busy);
        return;
    }

    std::unique_ptr<uint8_t[]> pixels;

    if (pixel_bytes != 0) {
        pixels.reset(new (std::nothrow) uint8_t[pixel_bytes]);

        if (!pixels) {
            reject(controller, Status::no_memory);
            return;
        }

        std::copy_n(transfer + LIGHTNING_GDI_PIXEL_OFFSET, pixel_bytes, pixels.get());
    }

    const uint64_t owner_ticket = (static_cast<uint64_t>(packet.epoch) << 32) | packet.sequence;

    if (candidate.accepted(command, owner_ticket) != Status::validated ||
        !controller.window.reserve(candidate.mode(), packet.sequence, packet.opcode == LIGHTNING_GDI_READBACK) ||
        !controller.window.accepted(candidate.mode(), packet.sequence)) {
        lost(controller);
        return;
    }

    auto& entry = controller.commands[(controller.head + controller.count) % LIGHTNING_GDI_MAX_PENDING_FENCES];
    entry.command = command;
    entry.pixels = std::move(pixels);
    entry.retained_bytes = pixel_bytes;
    entry.owner_ticket = owner_ticket;
    controller.retained_bytes += pixel_bytes;
    ++controller.count;
    controller.state = candidate;
    controller.retired = false;
    controller.status = LIGHTNING_GDI_ACCEPTED;
    controller.result = LIGHTNING_GDI_OK;

    if (packet.opcode == LIGHTNING_GDI_BIND) {
        if (!controller.damage.configure(actual_width, actual_height)) { lost(controller); return; }
        controller.published = {};
        controller.snapshot = {};
        controller.snapshot_in_flight = false;
    }

    if (replaceable(packet)) {
        ++controller.replaceable_presents;
        supersede(controller, entry);
    }
}

LightningGdiGpuResult issue(QueuedCommand& entry, LightningGdiGpu* gpu) noexcept {
    const auto& command = entry.command.header;
    const auto* clips = entry.command.clips;

    // The validated selectors apply to exactly the call issued below.
    if (!lightning_gdi_gpu_select(gpu, target_surface(command.surfaces), source_surface(command.surfaces))) {
        return LIGHTNING_GDI_GPU_INVALID;
    }
    switch (command.opcode) {
    case LIGHTNING_GDI_SURFACE_CREATE:
        return lightning_gdi_gpu_surface_create(gpu, target_surface(command.surfaces),
            command.destination.width, command.destination.height, &entry.gpu_ticket);
    case LIGHTNING_GDI_SURFACE_DESTROY:
        return lightning_gdi_gpu_surface_destroy(gpu, target_surface(command.surfaces), &entry.gpu_ticket);
    case LIGHTNING_GDI_BIND:
        {
            const auto reset = lightning_gdi_gpu_reset(gpu);

            if (reset != LIGHTNING_GDI_GPU_READY) {
                return reset;
            }

            return lightning_gdi_gpu_initialize(gpu, command.destination.width, command.destination.height, &entry.gpu_ticket);
        }
    case LIGHTNING_GDI_FILL:
        return lightning_gdi_gpu_fill(gpu, command.destination, clips, command.clip_count, command.color, &entry.gpu_ticket);
    case LIGHTNING_GDI_COPY:
        return lightning_gdi_gpu_copy(gpu, command.destination, command.source_x, command.source_y,
            clips, command.clip_count, &entry.gpu_ticket);
    case LIGHTNING_GDI_UPLOAD:
        return lightning_gdi_gpu_upload(gpu, command.destination, clips, command.clip_count,
            entry.pixels.get(), entry.retained_bytes, &entry.gpu_ticket);
    case LIGHTNING_GDI_BLEND_UPLOAD:
        return lightning_gdi_gpu_blend(gpu, command.destination, clips, command.clip_count,
            entry.pixels.get(), entry.retained_bytes, command.constant_alpha, command.flags, &entry.gpu_ticket);
    case LIGHTNING_GDI_ROP3:
        return lightning_gdi_gpu_rop3(gpu, command.destination, clips, command.clip_count,
            command.source_x, command.source_y, entry.pixels.get(), entry.retained_bytes,
            command.flags, command.color, command.constant_alpha, &entry.gpu_ticket);
    case LIGHTNING_GDI_RASTER:
        return lightning_gdi_gpu_raster(gpu, command.destination, clips, command.clip_count,
            &entry.command.raster, entry.pixels.get(), entry.retained_bytes, &entry.gpu_ticket);
    case LIGHTNING_GDI_GRADIENT:
        return lightning_gdi_gpu_gradient(gpu, command.destination, clips, command.clip_count,
            &entry.command.gradient, &entry.gpu_ticket);
    case LIGHTNING_GDI_EXTERNAL_IMAGE: {
        const auto copied = lightning_gdi_gpu_external_image(gpu, command.destination, command.source_x,
            command.source_y, clips, command.clip_count, &entry.command.external, &entry.gpu_ticket);
        // venus-supervise: a source revoked by its host owner (lost worker)
        // is an ordered no-op, never a canvas loss: the command still takes a
        // real fence so FIFO completion, damage and its receipt stay exact,
        // and the guest's pinned read retires instead of becoming unprovable.
        return copied == LIGHTNING_GDI_GPU_SOURCE_GONE ?
            lightning_gdi_gpu_checkpoint(gpu, &entry.gpu_ticket) : copied;
    }
    case LIGHTNING_GDI_READBACK:
        return lightning_gdi_gpu_readback_begin(gpu, command.destination, &entry.gpu_ticket);
    case LIGHTNING_GDI_FENCE:
    case LIGHTNING_GDI_BEGIN_GROUP:
    case LIGHTNING_GDI_END_GROUP:
        return lightning_gdi_gpu_checkpoint(gpu, &entry.gpu_ticket);
    default:
        return LIGHTNING_GDI_GPU_INVALID;
    }
}

LightningGdiGpuResult issue_fills(LightningGdiController& controller, uint32_t first,
    uint32_t budget, LightningGdiGpu* gpu, uint32_t& count, uint64_t& ticket) noexcept {
    count = 0;
    ticket = 0;
    FillBatch batch;
    if (!batch.configure(controller.state.mode())) { return LIGHTNING_GDI_GPU_INVALID; }
    for (uint32_t index = first; index < controller.count && count < budget; ++index) {
        const auto& entry = controller.commands[(controller.head + index) % LIGHTNING_GDI_MAX_PENDING_FENCES];
        if (entry.submitted || !batch.append(entry.command)) { break; }
        ++count;
    }
    if (count == 0 || !lightning_gdi_gpu_select(gpu, 0, 0)) { return LIGHTNING_GDI_GPU_INVALID; }
    return lightning_gdi_gpu_fill(gpu, batch.destination(), batch.clips(), batch.clip_count(),
        batch.color(), &ticket);
}
} // namespace

extern "C" LightningGdiController* lightning_gdi_controller_new(void) {
    return new (std::nothrow) LightningGdiController{};
}

extern "C" void lightning_gdi_controller_set_available(LightningGdiController* controller, bool available) {
    if (controller == nullptr) {
        return;
    }

    if (!available && !controller->retired) {
        lost(*controller);
    } else {
        controller->available = available && !controller->state.poisoned() && !controller->reset_requested;
    }
}

extern "C" void lightning_gdi_controller_set_external_available(LightningGdiController* controller,
    bool available) {
    if (controller != nullptr) controller->external_available = available;
}

extern "C" void lightning_gdi_controller_set_mailbox(LightningGdiController* controller, bool enabled) {
    if (controller != nullptr) controller->mailbox = enabled;
}

extern "C" uint32_t lightning_gdi_controller_held(const LightningGdiController* controller) {
    if (controller == nullptr || controller->count == 0 || controller->reset_requested ||
        controller->state.poisoned()) {
        return 0;
    }

    const auto& latest = controller->commands[(controller->head + controller->count - 1) % LIGHTNING_GDI_MAX_PENDING_FENCES];
    return !latest.submitted && replaceable(latest.command.header) ? latest.command.header.sequence : 0;
}

extern "C" bool lightning_gdi_controller_needs_work_beyond_hold(const LightningGdiController* controller) {
    return lightning_gdi_controller_needs_work(controller) &&
        !(controller->count == 1 && lightning_gdi_controller_held(controller) != 0);
}

extern "C" void lightning_gdi_controller_present_counts(const LightningGdiController* controller,
    uint64_t* accepted, uint64_t* dropped) {
    if (accepted != nullptr) *accepted = controller != nullptr ? controller->replaceable_presents : 0;
    if (dropped != nullptr) *dropped = controller != nullptr ? controller->superseded_presents : 0;
}

extern "C" bool lightning_gdi_controller_free(LightningGdiController* controller) {
    if (controller == nullptr) {
        return true;
    }

    if (!controller->retired || controller->count != 0 || controller->reset_requested) {
        return false;
    }

    delete controller;
    return true;
}

extern "C" bool lightning_gdi_controller_read(const LightningGdiController* controller,
    uint32_t address, uint32_t* value) {
    if (controller == nullptr || value == nullptr) {
        return false;
    }

    const auto& state = controller->state;
    const Mode mode = state.mode();

    switch (address) {
    case LIGHTNING_GDI_REG_MAGIC: *value = LIGHTNING_GDI_MAGIC; break;
    case LIGHTNING_GDI_REG_VERSION: *value = LIGHTNING_GDI_VERSION; break;
    case LIGHTNING_GDI_REG_CAPABILITIES:
        *value = controller->available && !state.poisoned() && !controller->reset_requested ?
            LIGHTNING_GDI_CAPABILITIES | (controller->external_available ? LIGHTNING_GDI_CAP_EXTERNAL_IMAGE : 0) |
            (controller->external_available && controller->mailbox ? LIGHTNING_GDI_CAP_EXTERNAL_MAILBOX : 0) : 0;
        break;
    case LIGHTNING_GDI_REG_COMMAND: *value = 0; break;
    case LIGHTNING_GDI_REG_STATUS: *value = state.poisoned() ? LIGHTNING_GDI_LOST : controller->status; break;
    case LIGHTNING_GDI_REG_RESULT: *value = controller->result; break;
    case LIGHTNING_GDI_REG_ACCEPTED_SEQUENCE: *value = state.accepted_sequence(); break;
    case LIGHTNING_GDI_REG_COMPLETED_SEQUENCE: *value = state.completed_sequence(); break;
    case LIGHTNING_GDI_REG_EPOCH: *value = mode.epoch; break;
    case LIGHTNING_GDI_REG_MODE_GENERATION: *value = mode.generation; break;
    case LIGHTNING_GDI_REG_CANVAS_REVISION: *value = state.canvas_revision(); break;
    case LIGHTNING_GDI_REG_WIDTH: *value = mode.width; break;
    case LIGHTNING_GDI_REG_HEIGHT: *value = mode.height; break;
    case LIGHTNING_GDI_REG_MAX_PIXEL_BYTES: *value = LIGHTNING_GDI_MAX_PIXEL_BYTES; break;
    case LIGHTNING_GDI_REG_READBACK_SEQUENCE: *value = controller->readback_sequence; break;
    case LIGHTNING_GDI_REG_TRANSFER_BYTES: *value = LIGHTNING_GDI_TRANSFER_BYTES; break;
    case LIGHTNING_GDI_REG_PUBLISHED_SEQUENCE: *value = controller->published.revision; break;
    case LIGHTNING_GDI_REG_PUBLISHED_EPOCH: *value = controller->published.epoch; break;
    case LIGHTNING_GDI_REG_PUBLISHED_MODE_GENERATION: *value = controller->published.mode_generation; break;
    case LIGHTNING_GDI_REG_RESET_GENERATION: *value = controller->reset_generation; break;
    case LIGHTNING_GDI_REG_FLAGS:
        *value = (state.publication_ready() ? LIGHTNING_GDI_FLAG_READY : 0) |
            (controller->reset_requested ? LIGHTNING_GDI_FLAG_RESETTING : 0) |
            (state.poisoned() ? LIGHTNING_GDI_FLAG_POISONED : 0) |
            (controller->retired ? LIGHTNING_GDI_FLAG_RETIRED : 0);
        break;
    default:
        return false;
    }

    return true;
}

extern "C" bool lightning_gdi_controller_write(LightningGdiController* controller,
    uint32_t address, uint32_t value, const uint8_t* bar4, size_t bar4_bytes,
    uint32_t actual_width, uint32_t actual_height) {
    if (controller == nullptr || address != LIGHTNING_GDI_REG_COMMAND) {
        return false;
    }

    if (value == LIGHTNING_GDI_SUBMIT) {
        submit(*controller, bar4, bar4_bytes, actual_width, actual_height);
    } else if (value == LIGHTNING_GDI_RESET) {
        lightning_gdi_controller_request_reset(controller);
    } else {
        reject(*controller, Status::bad_packet);
    }

    return true;
}

extern "C" bool lightning_gdi_controller_needs_work(const LightningGdiController* controller) {
    return controller != nullptr && (controller->reset_requested ||
        (!controller->state.poisoned() && controller->count != 0));
}

extern "C" void lightning_gdi_controller_pump(LightningGdiController* controller,
    LightningGdiGpu* gpu, uint8_t* bar4, size_t bar4_bytes) {
    lightning_gdi_controller_pump_holding(controller, gpu, bar4, bar4_bytes, false);
}

extern "C" void lightning_gdi_controller_pump_holding(LightningGdiController* controller,
    LightningGdiGpu* gpu, uint8_t* bar4, size_t bar4_bytes, bool hold_latest) {
    if (controller == nullptr || gpu == nullptr) {
        return;
    }

    if (controller->reset_requested) {
        const auto reset = lightning_gdi_gpu_reset(gpu);

        if (reset == LIGHTNING_GDI_GPU_READY) {
            clear_retired(*controller);
        } else if (reset != LIGHTNING_GDI_GPU_BUSY && reset != LIGHTNING_GDI_GPU_PENDING) {
            lost(*controller);
        }

        return;
    }

    if (controller->state.poisoned()) {
        return;
    }

    for (uint32_t index = 0, issued = 0; index < controller->count && issued < pump_budget; ++index) {
        auto& entry = controller->commands[(controller->head + index) % LIGHTNING_GDI_MAX_PENDING_FENCES];

        if (entry.submitted) {
            continue;
        }

        // Only the newest command can be held; anything accepted after a copy
        // makes it older and issues it here in order.
        if (hold_latest && index + 1 == controller->count && replaceable(entry.command.header)) {
            break;
        }

        uint32_t grouped = 1;
        uint64_t ticket = 0;
        // Only canvas fills share one flattened draw; surface fills issue alone.
        const bool fill = entry.command.header.opcode == LIGHTNING_GDI_FILL && entry.command.header.surfaces == 0;
        const auto submitted = fill ? issue_fills(*controller, index, pump_budget - issued, gpu, grouped, ticket) :
            issue(entry, gpu);
        if (!fill) { ticket = entry.gpu_ticket; }

        if (submitted == LIGHTNING_GDI_GPU_BUSY) {
            break;
        }

        if (submitted != LIGHTNING_GDI_GPU_PENDING || ticket == 0) {
            lost(*controller);
            return;
        }

        // Same-color, disjoint fills have no source dependencies. One real GPU
        // fence covers the exact flattened clips, while every accepted sequence
        // retains its own FIFO completion and damage receipt. BUSY commits none.
        for (uint32_t offset = 0; offset < grouped; ++offset) {
            auto& grouped_entry = controller->commands[(controller->head + index + offset) % LIGHTNING_GDI_MAX_PENDING_FENCES];
            grouped_entry.gpu_ticket = ticket;
            grouped_entry.submitted = true;
            controller->retained_bytes -= grouped_entry.retained_bytes;
            grouped_entry.retained_bytes = 0;
            grouped_entry.pixels.reset();
        }
        issued += grouped;
        index += grouped - 1;
    }

    for (uint32_t retired = 0; controller->count != 0 && retired < pump_budget; ++retired) {
        if (retire_front(*controller, gpu, bar4, bar4_bytes) != Retirement::retired) {
            break;
        }
    }
}

namespace {
bool same_frame(const LightningGdiFrame &a, const LightningGdiFrame &b) noexcept {
    if (a.mode_generation != b.mode_generation || a.epoch != b.epoch || a.revision != b.revision ||
        a.width != b.width || a.height != b.height || !same_rect(a.damage, b.damage) ||
        a.damage_count != b.damage_count || a.damage_count > LIGHTNING_GDI_MAX_RECTS) { return false; }
    for (uint32_t index = 0; index < a.damage_count; ++index) {
        if (!same_rect(a.damage_rects[index], b.damage_rects[index])) { return false; }
    }
    return true;
}
}
extern "C" bool lightning_gdi_controller_frame(const LightningGdiController* controller,
    LightningGdiFrame* frame) {
    if (controller == nullptr || frame == nullptr || !controller->available || controller->reset_requested ||
        controller->snapshot_in_flight || !controller->state.publication_ready() ||
        controller->state.canvas_revision() == controller->published.revision || controller->damage.empty()) { return false; }
    const Mode mode = controller->state.mode();
    *frame = {};
    frame->mode_generation = mode.generation; frame->epoch = mode.epoch;
    frame->revision = controller->state.canvas_revision(); frame->width = mode.width; frame->height = mode.height;
    controller->damage.export_frame(*frame);
    return true;
}
extern "C" bool lightning_gdi_controller_snapshot(LightningGdiController* controller,
    const LightningGdiFrame* frame) {
    LightningGdiFrame current{};
    if (frame == nullptr || !lightning_gdi_controller_frame(controller, &current) || !same_frame(current, *frame)) { return false; }
    controller->snapshot = *frame;
    controller->snapshot_in_flight = true;
    controller->damage.clear();
    return true;
}
extern "C" bool lightning_gdi_controller_snapshot_current(const LightningGdiController* controller,
    const LightningGdiFrame* frame) {
    if (controller == nullptr || frame == nullptr || !controller->snapshot_in_flight || controller->reset_requested ||
        controller->state.poisoned() || !same_frame(controller->snapshot, *frame)) { return false; }
    const Mode mode = controller->state.mode();
    return frame->mode_generation == mode.generation && frame->epoch == mode.epoch &&
        frame->width == mode.width && frame->height == mode.height &&
        frame->revision <= controller->state.completed_sequence();
}
extern "C" bool lightning_gdi_controller_published(LightningGdiController* controller,
    const LightningGdiFrame* frame, bool success) {
    if (!lightning_gdi_controller_snapshot_current(controller, frame)) { return false; }
    if (!success) { lost(*controller); return false; }
    controller->published = *frame;
    controller->snapshot = {};
    controller->snapshot_in_flight = false;
    return true;
}

extern "C" void lightning_gdi_controller_request_reset(LightningGdiController* controller) {
    if (controller != nullptr) {
        controller->reset_requested = true;
        controller->retired = false;
        controller->state.poison();
        controller->window.uncertain();
    }
}

extern "C" bool lightning_gdi_controller_reset_complete(const LightningGdiController* controller) {
    return controller != nullptr && controller->retired && !controller->reset_requested && controller->count == 0;
}

extern "C" void lightning_gdi_controller_retire_after_context_destroy(LightningGdiController* controller) {
    if (controller != nullptr) {
        controller->available = false;
        clear_retired(*controller);
    }
}
