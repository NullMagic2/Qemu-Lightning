/*
 * Module: d3d10umd.Compute (Qemu Lightning)
 * Purpose: D3D11 compute dispatch, direct and indirect.
 * Created: 2026-10-16
 * Architecture: See Compute.h.
 */

#include "Compute.h"

#include "Debug.h"
#include "Draw.h"
#include "State.h"
#include "UnorderedAccess.h"

/* D3D11 limit on each thread group count of a direct dispatch. */
static const UINT MAX_THREAD_GROUP_COUNT = 65535;
static const unsigned GRID_DIMENSIONS = 3;


/* The bound compute shader, ready to launch, or NULL when there is none or
 * its translation was rejected (the dispatch is skipped). */
static const Shader *
PrepareCompute(Device *pDevice)
{
    const Shader *shader = pDevice->bound_shaders[MESA_SHADER_COMPUTE];

    if (!shader || shader->invalid || !shader->handle)
        return NULL;
    pDevice->resource_binding_failed = false;
    BindStageResources(pDevice, MESA_SHADER_COMPUTE);
    if (pDevice->resource_binding_failed) {
        D3D10DDI_HDEVICE device = {pDevice};

        SetError(device, E_OUTOFMEMORY);
        return NULL;
    }
    return shader;
}


static void
Launch(Device *pDevice, const Shader *shader, const UINT groups[3],
       struct pipe_resource *indirect, UINT offset)
{
    D3D10DDI_HDEVICE device = {pDevice};
    HRESULT dependency = OrderUnorderedAccess(pDevice, MESA_SHADER_COMPUTE);

    if (FAILED(dependency)) {
        SetError(device, dependency);
        return;
    }

    struct pipe_grid_info info;

    memset(&info, 0, sizeof info);
    info.work_dim = GRID_DIMENSIONS;
    for (unsigned i = 0; i < GRID_DIMENSIONS; i++) {
        info.block[i] = shader->block[i];
        info.last_block[i] = 0;
        info.grid[i] = groups ? groups[i] : 0;
    }
    info.indirect = indirect;
    info.indirect_offset = offset;
    pDevice->pipe->launch_grid(pDevice->pipe, &info);
    SetError(device, OrderUnorderedAccess(pDevice, MESA_SHADER_COMPUTE));
}


void APIENTRY
Dispatch(D3D10DDI_HDEVICE hDevice, UINT ThreadGroupCountX, UINT ThreadGroupCountY,
         UINT ThreadGroupCountZ)
{
    LOG_ENTRYPOINT();

    Device *pDevice = CastDevice(hDevice);
    const UINT groups[3] = {ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ};

    /* An empty grid does nothing; an oversized one is invalid in D3D11. */
    for (unsigned i = 0; i < GRID_DIMENSIONS; i++) {
        if (!groups[i] || groups[i] > MAX_THREAD_GROUP_COUNT)
            return;
    }
    const Shader *shader = PrepareCompute(pDevice);
    if (shader)
        Launch(pDevice, shader, groups, NULL, 0);
}


void APIENTRY
DispatchIndirect(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hBuffer, UINT AlignedByteOffset)
{
    LOG_ENTRYPOINT();

    Device *pDevice = CastDevice(hDevice);
    struct pipe_resource *buffer = CastPipeResource(hBuffer);

    if (!buffer)
        return;
    const Shader *shader = PrepareCompute(pDevice);
    if (shader)
        Launch(pDevice, shader, NULL, buffer, AlignedByteOffset);
}
