# Module: qemu_lightning.opengl.reactos_venus_adapter_sync
# Purpose: Update owned C++ adapters and record exact dependency boundary inputs before compiling.
# Created: 2026-10-21
# Architecture: Copy only known files into the private export; never modify shared source caches.
set(overlay "${QL_STAGING}/src/virtio/vulkan/lightning")
set(QL_PHASE_REMOVE ON)
include("${QL_MODULE}/reactos-phase-adapter.cmake")
set(QL_PHASE_REMOVE OFF)
include("${QL_MODULE}/reactos-wsi-adapter.cmake")
include("${QL_MODULE}/reactos-condition-adapter.cmake")
set(provenance "NT5 Venus adapter inputs; rendering requires the published kernel exchange capability and a real host OPEN.\n")
foreach(file reactos_venus_platform.cpp reactos_venus_renderer.cpp reactos_venus_exchange.cpp
        reactos_venus_exchange.hpp reactos_venus_present.hpp deployment.hpp runtime.hpp timer_resolution.hpp
        reactos_phase.hpp reactos_phase.cpp)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${QL_MODULE}/${file}" "${overlay}/${file}"
        COMMAND_ERROR_IS_FATAL ANY)
    file(SHA256 "${overlay}/${file}" hash)
    string(APPEND provenance "${file} SHA256 ${hash}\n")
endforeach()
foreach(file lightning_abi.hpp lightning_image.hpp)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${QL_REACTOS_SOURCE_ROOT}/src/reactos/${file}" "${overlay}/${file}"
        COMMAND_ERROR_IS_FATAL ANY)
    file(SHA256 "${overlay}/${file}" hash)
    string(APPEND provenance "${file} SHA256 ${hash}\n")
endforeach()
foreach(file mesa_windows_renderer.c mesa_specialization_storage.c transport.h windows_platform.h
        windows_sync_fd.h windows_icd.def venus_loss_probe.h)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${QL_GUEST_ROOT}/${file}" "${overlay}/${file}" COMMAND_ERROR_IS_FATAL ANY)
    file(SHA256 "${overlay}/${file}" hash)
    string(APPEND provenance "${file} SHA256 ${hash}\n")
endforeach()
foreach(file windows_library.rs windows_transport.rs transport_contract.rs)
    file(SHA256 "${QL_GUEST_ROOT}/${file}" hash)
    string(APPEND provenance "Reused unchanged ${file} SHA256 ${hash}\n")
endforeach()
file(WRITE "${QL_STAGING}/lightning-nt5-adapter-provenance.txt" "${provenance}")
include("${QL_MODULE}/reactos-phase-adapter.cmake")
include("${QL_MODULE}/reactos-venus-oom-diagnostics.cmake")
include("${QL_MODULE}/reactos-venus-map-lifetime.cmake")
include("${QL_MODULE}/reactos-venus-device-lost.cmake")
file(SHA256 "${QL_MODULE}/reactos-phase-adapter.cmake" phase_hash)
file(APPEND "${QL_STAGING}/lightning-nt5-adapter-provenance.txt"
    "reactos-phase-adapter.cmake SHA256 ${phase_hash}\n")
file(SHA256 "${QL_MODULE}/reactos-venus-map-lifetime.cmake" map_lifetime_hash)
file(APPEND "${QL_STAGING}/lightning-nt5-adapter-provenance.txt"
    "reactos-venus-map-lifetime.cmake SHA256 ${map_lifetime_hash}\n")
file(SHA256 "${QL_MODULE}/reactos-venus-device-lost.cmake" device_lost_hash)
file(APPEND "${QL_STAGING}/lightning-nt5-adapter-provenance.txt"
    "reactos-venus-device-lost.cmake SHA256 ${device_lost_hash}\n")
foreach(file reactos-wsi-adapter.cmake reactos_wsi_dib.hpp reactos_wsi_gpu.hpp
        reactos_wsi_present_queue.hpp)
    file(SHA256 "${QL_MODULE}/${file}" hash)
    file(APPEND "${QL_STAGING}/lightning-nt5-adapter-provenance.txt" "${file} SHA256 ${hash}\n")
endforeach()
file(SHA256 "${QL_MODULE}/../../include/hw/misc/lightning-opengl-present.h" present_hash)
file(APPEND "${QL_STAGING}/lightning-nt5-adapter-provenance.txt"
    "lightning-opengl-present.h SHA256 ${present_hash}\n"
    "GPU Venus rendering; retained exportable BGRA8 images use producer wait and synchronous clipped GPU canvas copy when native capability exists. Unsupported paths retain completed CPU-image readback/GDI presentation. No zero-copy or physical-vblank claim.\n")
