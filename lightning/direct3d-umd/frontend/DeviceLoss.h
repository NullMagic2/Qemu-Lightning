/* Module: qemu_lightning.direct3d_umd.device_loss
 * Purpose: Report native backend loss around command and status DDI entries.
 * Created: 2026-11-07
 * Architecture: Typed C++ adapter preserves Windows calling conventions and
 * signatures of the retained C++ frontend. No command or resource is decoded.
 */
#ifndef QLG_DEVICE_LOSS_H
#define QLG_DEVICE_LOSS_H

/* Include after State.h, whose retained native declarations have no guard. */
#include <type_traits>
#include "DdiSerialization.h"

enum class DeviceCallMode { Work, Release, Destroy };

inline Device *DeviceCallOwner(D3D10DDI_HDEVICE handle)
{
   return CastDevice(handle);
}

template <typename Arguments>
inline Device *DeviceCallOwner(Arguments *arguments)
{
   return CastDevice(arguments->hDevice);
}

template <typename Result>
inline Result DeviceRemovedResult()
{
   if constexpr (std::is_same<Result, HRESULT>::value)
      return D3DDDIERR_DEVICEREMOVED;
   else
      return Result();
}

/* Work is refused after loss. Release/unmap still own native references and
 * must finish. DestroyDevice destroys the pipe itself, so has no post-poll.
 * All other returns, including early fallback returns, pass the same checks.
 * The runtime supplies valid handles and serializes this immediate context. */
template <typename Signature, Signature Function, DeviceCallMode Mode>
struct DeviceCall;

template <typename Result, typename First, typename... Arguments,
          Result (APIENTRY *Function)(First, Arguments...), DeviceCallMode Mode>
struct DeviceCall<Result (APIENTRY *)(First, Arguments...), Function, Mode>
{
   static Result APIENTRY Call(First first, Arguments... arguments)
   {
      Device *device = DeviceCallOwner(first);
      D3D10DDI_HDEVICE handle = {device};

      if constexpr (Mode == DeviceCallMode::Destroy) {
         return Function(first, arguments...);
      } else {
         if (DeviceRemoved(device) && Mode == DeviceCallMode::Work) {
            SetError(handle, D3DDDIERR_DEVICEREMOVED);

            if constexpr (std::is_void<Result>::value)
               return;
            else
               return DeviceRemovedResult<Result>();
         }

         if constexpr (std::is_void<Result>::value) {
            Function(first, arguments...);
            SetError(handle, S_OK);
         } else {
            Result result = Function(first, arguments...);

            if constexpr (std::is_same<Result, HRESULT>::value) {
               if (result == D3DDDIERR_DEVICEREMOVED)
                  device->device_removed = true;
            }

            if (DeviceRemoved(device)) {
               SetError(handle, D3DDDIERR_DEVICEREMOVED);
               return DeviceRemovedResult<Result>();
            }

            return result;
         }
      }
   }
};

/* A successful constructor owns an object even if backend loss becomes visible
 * just before it returns. Reporting removal invalidates that handle: the runtime
 * will not call its destroy callback. Release it before reporting. Error serials
 * distinguish a successful return from a constructor that already failed and
 * unwound; nested DDI11_1/base constructors cannot release the same object twice.
 */
template <typename Signature, Signature Function,
          typename ReleaseSignature, ReleaseSignature Release>
struct DeviceCreateCall;

template <typename Description, typename Handle, typename... Arguments,
          void (APIENTRY *Function)(D3D10DDI_HDEVICE, Description, Handle, Arguments...),
          void (APIENTRY *Release)(D3D10DDI_HDEVICE, Handle)>
struct DeviceCreateCall<
   void (APIENTRY *)(D3D10DDI_HDEVICE, Description, Handle, Arguments...), Function,
   void (APIENTRY *)(D3D10DDI_HDEVICE, Handle), Release>
{
   static void APIENTRY Call(D3D10DDI_HDEVICE device_handle, Description description,
                             Handle object, Arguments... arguments)
   {
      Device *device = CastDevice(device_handle);

      if (DeviceRemoved(device)) {
         SetError(device_handle, D3DDDIERR_DEVICEREMOVED);
         return;
      }

      const uint64_t before = device->error_serial;

      Function(device_handle, description, object, arguments...);

      if (DeviceRemoved(device)) {
         if (device->error_serial == before)
            Release(device_handle, object);

         SetError(device_handle, D3DDDIERR_DEVICEREMOVED);
      }
   }
};

/* A rejected Map has no future Unmap from the runtime. Undo only a successful
 * newly created transfer before reporting loss, leaving older maps untouched. */
template <void (APIENTRY *Function)(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT,
                                    D3D10_DDI_MAP, UINT, D3D10DDI_MAPPED_SUBRESOURCE *),
          void (APIENTRY *Unmap)(D3D10DDI_HDEVICE, D3D10DDI_HRESOURCE, UINT)>
struct DeviceMapCall
{
   static void APIENTRY Call(D3D10DDI_HDEVICE handle, D3D10DDI_HRESOURCE resource,
                             UINT subresource, D3D10_DDI_MAP access, UINT flags,
                             D3D10DDI_MAPPED_SUBRESOURCE *mapped)
   {
      Device *device = CastDevice(handle);

      if (DeviceRemoved(device)) {
         SetError(handle, D3DDDIERR_DEVICEREMOVED);
         return;
      }

      const uint64_t before = device->error_serial;

      Function(handle, resource, subresource, access, flags, mapped);

      if (DeviceRemoved(device)) {
         if (device->error_serial == before)
            Unmap(handle, resource, subresource);

         memset(mapped, 0, sizeof *mapped);
         SetError(handle, D3DDDIERR_DEVICEREMOVED);
      }
   }
};

#define QLG_DEVICE_MAP(function, unmap) \
   DeviceMapCall<&function, &unmap>::Call
#define QLG_DEVICE_CREATE(function, release) \
   DeviceCreateCall<decltype(&function), &function, decltype(&release), &release>::Call
#define QLG_DEVICE_CALL(function) \
   DeviceCall<decltype(&function), &function, DeviceCallMode::Work>::Call
#define QLG_DEVICE_RELEASE(function) \
   DeviceCall<decltype(&function), &function, DeviceCallMode::Release>::Call
#define QLG_DEVICE_DESTROY(function) \
   DeviceCall<decltype(&function), &function, DeviceCallMode::Destroy>::Call

#endif
