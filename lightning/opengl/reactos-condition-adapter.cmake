# Module: qemu_lightning.opengl.reactos_condition_adapter
# Purpose: Reuse the C++ NT5 condition wait owner in both private Mesa runtimes.
# Created: 2026-10-22
# Architecture: Canonical imports use one app-local C++ DLL for wait lifetimes.
foreach(required QL_MODULE QL_STAGING)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing condition adapter input: ${required}")
    endif()
endforeach()
set(condition_directory "${QL_STAGING}/src/c11/impl")
set(condition_provenance "Private NT5 condition-variable adapter inputs\n")
foreach(name reactos_condition_variable.hpp reactos_condition_variable.cpp)
    file(SHA256 "${QL_MODULE}/${name}" digest)
    string(APPEND condition_provenance "${name} SHA256 ${digest}\n")
endforeach()
foreach(relative src/c11/impl/threads_win32.c src/util/cnd_monotonic.c)
    set(path "${QL_STAGING}/${relative}")
    file(READ "${path}" source)
    set(previous "${source}")
    if(relative STREQUAL "src/c11/impl/threads_win32.c")
        set(header "reactos_condition_variable.hpp")
    else()
        set(header "../c11/impl/reactos_condition_variable.hpp")
    endif()
    string(REPLACE "#include \"${header}\"\n" "" source "${source}")
    # Migrate the earlier embedded overlay back to canonical API imports.
    # C11, monotonic waits and the static C++ runtime must all resolve through
    # QemuLightningSync, including initialization and wake operations.
    foreach(pair "InitializeConditionVariable;ql_nt5_initialize_condition_variable"
                 "SleepConditionVariableCS;ql_nt5_sleep_condition_variable_cs"
                 "SleepConditionVariableSRW;ql_nt5_sleep_condition_variable_srw"
                 "WakeConditionVariable;ql_nt5_wake_condition_variable"
                 "WakeAllConditionVariable;ql_nt5_wake_all_condition_variable")
        list(GET pair 0 original)
        list(GET pair 1 replacement)
        string(REPLACE "${replacement}(" "${original}(" source "${source}")
    endforeach()
    if(NOT source STREQUAL previous)
        file(WRITE "${path}" "${source}")
    endif()
    file(SHA256 "${path}" digest)
    string(APPEND condition_provenance "${relative} SHA256 ${digest}\n")
endforeach()
set(path "${condition_directory}/meson.build")
file(READ "${path}" source)
set(previous "${source}")
string(REPLACE "  files_mesa_util_c11 += 'reactos_condition_variable.cpp'\n" "" source "${source}")
if(NOT source STREQUAL previous)
    file(WRITE "${path}" "${source}")
endif()
file(REMOVE "${condition_directory}/reactos_condition_variable.cpp" "${condition_directory}/reactos_condition_variable.hpp")
file(WRITE "${QL_STAGING}/lightning-condition-adapter-provenance.txt" "${condition_provenance}")
