/* Module: lightning.direct3d_umd.ddi_serialization
 * Purpose: Preserve exact DDI calling conventions around the Rust entry gate.
 * Created: 2026-11-08
 * Architecture: Only concrete ABI forwarding lives here; Rust owns lock state.
 */
#ifndef QLG_DDI_SERIALIZATION_H
#define QLG_DDI_SERIALIZATION_H

#ifdef QEMU_LIGHTNING_NATIVE_COMMAND_LISTS
extern "C" void qlg_d3d11_enter(void);
extern "C" void qlg_d3d11_leave(void);
class DdiEntryGuard {
public:
    DdiEntryGuard() { qlg_d3d11_enter(); }
    ~DdiEntryGuard() { qlg_d3d11_leave(); }
    DdiEntryGuard(const DdiEntryGuard &) = delete;
    DdiEntryGuard &operator=(const DdiEntryGuard &) = delete;
};

template <typename Signature, Signature Function> struct DdiSerializedSignature;
template <typename Result, typename... Arguments,
          Result (APIENTRY *Function)(Arguments...)>
struct DdiSerializedSignature<Result (APIENTRY *)(Arguments...), Function> {
    static Result APIENTRY Call(Arguments... arguments)
    {
        DdiEntryGuard guard;
        return Function(arguments...);
    }
};
template <auto Function>
using DdiSerialized = DdiSerializedSignature<decltype(Function), Function>;
#define QLG_SERIALIZE(...) DdiSerialized<(__VA_ARGS__)>::Call
#else
#define QLG_SERIALIZE(...) __VA_ARGS__
#endif
#endif
