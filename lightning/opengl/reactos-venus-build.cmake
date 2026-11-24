# Module: qemu_lightning.opengl.reactos_venus_build
# Purpose: Reproduce the native i386 Venus dependency with the capability-gated NT5 exchange.
# Created: 2026-10-21
# Architecture: Reuses pinned Mesa/protocol and unchanged no_std policy; C++ owns new adapters.
if(NOT ql_reactos OR NOT QL_BUILD_MESA_OPENGL)
    message(FATAL_ERROR "The NT5 Venus build requires the ReactOS i386 or amd64 Mesa profile")
endif()
find_program(QL_CARGO cargo REQUIRED)
get_filename_component(QL_GUEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../guest-vulkan" ABSOLUTE)
set(ql_policy_root "${CMAKE_BINARY_DIR}/reactos-policy")
file(MAKE_DIRECTORY "${ql_policy_root}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/reactos-policy-Cargo.toml.in" "${ql_policy_root}/Cargo.toml" @ONLY)
set(QL_POLICY_LIBRARY "${ql_policy_root}/target/${ql_ros_rust_target}/release/liblightning_reactos_reused_policy.a")
# The unchanged no_std policy is rebuilt for the selected ReactOS architecture.
string(TOUPPER "${ql_ros_rust_target}" ql_ros_cargo_target)
string(REPLACE "-" "_" ql_ros_cargo_target "${ql_ros_cargo_target}")
add_custom_command(OUTPUT "${QL_POLICY_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E env RUSTC_BOOTSTRAP=1
        CARGO_TARGET_${ql_ros_cargo_target}_LINKER=${ql_ros_triplet}-gcc
        "${QL_CARGO}" build --manifest-path "${ql_policy_root}/Cargo.toml"
        -Zbuild-std=core,compiler_builtins --target ${ql_ros_rust_target} --offline --release
    DEPENDS "${ql_policy_root}/Cargo.toml" "${QL_GUEST_ROOT}/windows_library.rs"
        "${QL_GUEST_ROOT}/windows_transport.rs" "${QL_GUEST_ROOT}/transport_contract.rs" VERBATIM)
add_custom_target(ql_reactos_venus_policy DEPENDS "${QL_POLICY_LIBRARY}")
ExternalProject_Add(ql_reactos_venus
    DEPENDS ql_reactos_venus_policy ql_reactos_import_library ql_reactos_sync
    SOURCE_DIR "${CMAKE_BINARY_DIR}/venus-source"
    BINARY_DIR "${CMAKE_BINARY_DIR}/venus-build"
    DOWNLOAD_COMMAND "${CMAKE_COMMAND}" "-DQL_MODULE=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DQL_GIT=${GIT_EXECUTABLE}" "-DQL_REFERENCE=${QL_MESA_SOURCE_ROOT}"
        "-DQL_STAGING=<SOURCE_DIR>" "-DQL_COMMIT=${QL_MESA_COMMIT}"
        "-DQL_PATCH=${CMAKE_CURRENT_SOURCE_DIR}/zink-submit-wakeup.patch"
        -DQL_REACTOS_I386=ON "-DQL_REACTOS_ARCH=${QL_REACTOS_ARCH}" "-DQL_REACTOS_PATCH=${CMAKE_CURRENT_SOURCE_DIR}/reactos-mesa.patch"
        "-DQL_GUEST_ROOT=${QL_GUEST_ROOT}" "-DQL_POLICY_LIBRARY=${QL_POLICY_LIBRARY}"
        "-DQL_REACTOS_SOURCE_ROOT=${QL_REACTOS_SOURCE_ROOT}"
        "-DQL_NATIVE_ADAPTER_HEADER=${QL_NATIVE_ADAPTER_HEADER}"
        "-DQL_VENUS_SOURCE=${QL_VENUS_PROTOCOL_SOURCE_ROOT}"
        "-DQL_DIRECTX_SOURCE=${QL_DIRECTX_HEADERS_SOURCE_ROOT}"
        -DQL_VENUS_COMMIT=ca19b6358d7cc491bc3e4de76f04c6700876a8fa
        -DQL_DIRECTX_COMMIT=9e393d6d8a3b30dcc6f2806ef604ec16a27b0d7e
        -DQL_VENUS_DIRECTORY=venus-protocol-1.1.3 -DQL_DIRECTX_DIRECTORY=DirectX-Headers-1.0
        -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare-venus.cmake"
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env ${QL_MESA_ENV}
        "NINJA=${QL_NINJA_EXECUTABLE}" "${QL_MESON_EXECUTABLE}" setup --reconfigure <BINARY_DIR> <SOURCE_DIR>
        --cross-file "${QL_MESA_CROSS_FILE}" --wrap-mode=nodownload --buildtype=release
        -Dplatforms=windows -Dvulkan-drivers=virtio -Dgallium-drivers=[] -Dllvm=disabled
        -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false
        -Dglvnd=disabled -Dbuild-tests=false -Dlibunwind=disabled -Dzstd=disabled
        -Dzlib=disabled -Dvalgrind=disabled -Dvideo-codecs=[] -Dexpat=disabled
        -Dspirv-tools=disabled -Dmin-windows-version=7
        "-Dc_link_args=${ql_mesa_c_links}" "-Dcpp_link_args=${ql_mesa_cpp_links}"
    BUILD_COMMAND "${CMAKE_COMMAND}" -E env ${QL_MESA_ENV}
        "${QL_NINJA_EXECUTABLE}" -C <BINARY_DIR> -j "${QL_MESA_JOBS}"
        src/virtio/vulkan/libvulkan_virtio.dll
    INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "${QL_OPENGL_PACKAGE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        <BINARY_DIR>/src/virtio/vulkan/libvulkan_virtio.dll "${QL_OPENGL_PACKAGE}/QemuLightningVulkan.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        <SOURCE_DIR>/lightning-opengl-provenance.txt "${QL_OPENGL_PACKAGE}/lightning-venus-provenance.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        <SOURCE_DIR>/lightning-nt5-adapter-provenance.txt "${QL_OPENGL_PACKAGE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        <SOURCE_DIR>/lightning-condition-adapter-provenance.txt "${QL_OPENGL_PACKAGE}/lightning-venus-condition-provenance.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        <SOURCE_DIR>/subprojects/DirectX-Headers-1.0/LICENSE "${QL_OPENGL_PACKAGE}/DirectX-Headers-LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        <SOURCE_DIR>/subprojects/venus-protocol-1.1.3/vn_protocol.py "${QL_OPENGL_PACKAGE}/Venus-original-notices.py"
    BUILD_BYPRODUCTS "${QL_OPENGL_PACKAGE}/QemuLightningVulkan.dll")
ExternalProject_Add_Step(ql_reactos_venus sync_cpp_adapters
    COMMAND "${CMAKE_COMMAND}" "-DQL_MODULE=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DQL_STAGING=<SOURCE_DIR>" "-DQL_REACTOS_SOURCE_ROOT=${QL_REACTOS_SOURCE_ROOT}"
        "-DQL_GUEST_ROOT=${QL_GUEST_ROOT}" -P "${CMAKE_CURRENT_SOURCE_DIR}/sync-venus-adapters.cmake"
    DEPENDEES configure DEPENDERS build ALWAYS TRUE)
ExternalProject_Add_Step(ql_reactos_venus reactos_import_audit
    COMMAND "${CMAKE_COMMAND}" "-DQL_OBJDUMP=${QL_REACTOS_OBJDUMP}"
        "-DQL_PACKAGE=${QL_OPENGL_PACKAGE}" "-DQL_SYSTEM32=${QL_REACTOS_SYSTEM32}" "-DQL_REACTOS_ARCH=${ql_ros_arch}"
        "-DQL_MODULES=QemuLightningVulkan.dll"
        "-DQL_REPORT=${QL_OPENGL_PACKAGE}/reactos-venus-import-audit.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/audit-reactos-imports.cmake"
    DEPENDEES install ALWAYS TRUE)
install(FILES "${QL_OPENGL_PACKAGE}/QemuLightningVulkan.dll"
    "${QL_OPENGL_PACKAGE}/lightning-venus-provenance.txt"
    "${QL_OPENGL_PACKAGE}/lightning-nt5-adapter-provenance.txt"
    "${QL_OPENGL_PACKAGE}/lightning-venus-condition-provenance.txt"
    "${QL_OPENGL_PACKAGE}/DirectX-Headers-LICENSE"
    "${QL_OPENGL_PACKAGE}/Venus-original-notices.py"
    "${QL_OPENGL_PACKAGE}/reactos-venus-import-audit.txt" DESTINATION .)
