/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Query.cpp --
 *    Functions that manipulate query resources.
 */


#include "Query.h"
#include "State.h"

#include "Debug.h"


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateQuerySize --
 *
 *    The CalcPrivateQuerySize function determines the size of the
 *    user-mode display driver's private region of memory (that is,
 *    the size of internal driver structures, not the size of the
 *    resource video memory) for a query.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateQuerySize(D3D10DDI_HDEVICE hDevice,                          // IN
                     __in const D3D10DDIARG_CREATEQUERY *pCreateQuery)  // IN
{
   return sizeof(Query);
}


/* Qemu Lightning: the stream a D3D11 per-stream query counts. */
static unsigned
QueryStream(D3D10DDI_QUERY query)
{
   if (query >= D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0 &&
       query <= D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM3)
      return query - D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0;
   if (query >= D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0 &&
       query <= D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3)
      return query - D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0;
   return 0;
}

static uint
TranslateQueryType(D3D10DDI_QUERY query)
{
   switch (query) {
   case D3D10DDI_QUERY_EVENT:
      return PIPE_QUERY_GPU_FINISHED;
   case D3D10DDI_QUERY_OCCLUSION:
      return PIPE_QUERY_OCCLUSION_COUNTER;
   case D3D10DDI_QUERY_TIMESTAMP:
      return PIPE_QUERY_TIMESTAMP;
   case D3D10DDI_QUERY_TIMESTAMPDISJOINT:
      return PIPE_QUERY_TIMESTAMP_DISJOINT;
   case D3D10DDI_QUERY_PIPELINESTATS:
      return PIPE_QUERY_PIPELINE_STATISTICS;
   case D3D10DDI_QUERY_OCCLUSIONPREDICATE:
      return PIPE_QUERY_OCCLUSION_PREDICATE;
   case D3D10DDI_QUERY_STREAMOUTPUTSTATS:
      return PIPE_QUERY_SO_STATISTICS;
   case D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE:
      return PIPE_QUERY_SO_OVERFLOW_PREDICATE;
   /* Qemu Lightning: D3D11 statistics and per-stream stream-output queries. */
   case D3D11DDI_QUERY_PIPELINESTATS:
      return PIPE_QUERY_PIPELINE_STATISTICS;
   case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0:
   case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM1:
   case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM2:
   case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM3:
      return PIPE_QUERY_SO_STATISTICS;
   case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0:
   case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM1:
   case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM2:
   case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3:
      return PIPE_QUERY_SO_OVERFLOW_PREDICATE;
   default:
      LOG_UNSUPPORTED(true);
      return PIPE_QUERY_TYPES;
   }
}


/* A statistics fallback is usable only when every counter exists. Never
 * publish or poll a partial collection as a successful zero result. */
static bool
QueryHasStorage(const Query *query)
{
   if (query->failed || query->pipe_type >= PIPE_QUERY_TYPES)
      return false;
   if (query->Type == D3D10DDI_QUERY_EVENT || query->handle)
      return true;
   if (query->pipe_type != PIPE_QUERY_PIPELINE_STATISTICS)
      return false;
   for (unsigned i = 0; i < QUERY_STATISTICS_COUNTERS; i++) {
      if (!query->counters[i])
         return false;
   }
   return true;
}

static void
ReleaseQueryStorage(struct pipe_context *pipe, Query *query)
{
   if (query->handle) {
      if (query->active)
         pipe->end_query(pipe, query->handle);
      pipe->destroy_query(pipe, query->handle);
      query->handle = NULL;
   }
   for (unsigned i = 0; i < QUERY_STATISTICS_COUNTERS; i++) {
      if (query->counters[i]) {
         if (query->active)
            pipe->end_query(pipe, query->counters[i]);
         pipe->destroy_query(pipe, query->counters[i]);
         query->counters[i] = NULL;
      }
   }
   query->active = false;
   pipe->screen->fence_reference(pipe->screen, &query->fence, NULL);
}

static void
QueryExecutionFailed(D3D10DDI_HDEVICE device, Query *query)
{
   /* Begin, End, GetData and SetPredication allow device removal, not the
    * allocation error permitted by CreateQuery. Keep failure sticky. */
   query->failed = true;
   SetError(device, D3DDDIERR_DEVICEREMOVED);
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateQuery --
 *
 *    The CreateQuery function creates driver-side resources for a
 *    query that the Microsoft Direct3D runtime subsequently issues
 *    for processing.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateQuery(D3D10DDI_HDEVICE hDevice,                          // IN
            __in const D3D10DDIARG_CREATEQUERY *pCreateQuery,  // IN
            D3D10DDI_HQUERY hQuery,                            // IN
            D3D10DDI_HRTQUERY hRTQuery)                        // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   Query *pQuery = CastQuery(hQuery);
   memset(pQuery, 0, sizeof *pQuery);

   pQuery->Type = pCreateQuery->Query;
   pQuery->Flags = pCreateQuery->MiscFlags;

   pQuery->pipe_type = TranslateQueryType(pCreateQuery->Query);

   if (pQuery->pipe_type >= PIPE_QUERY_TYPES) {
      QueryExecutionFailed(hDevice, pQuery);
      return;
   }
   /* Events use a deferred fence captured at End rather than a query. */
   if (pQuery->Type == D3D10DDI_QUERY_EVENT)
      return;

   pQuery->handle = pipe->create_query(pipe, pQuery->pipe_type, QueryStream(pQuery->Type));
   if (!pQuery->handle && pQuery->pipe_type == PIPE_QUERY_PIPELINE_STATISTICS &&
       pipe->screen->caps.query_pipeline_statistics_single) {
      for (unsigned i = 0; i < QUERY_STATISTICS_COUNTERS; i++) {
         pQuery->counters[i] = pipe->create_query(pipe, PIPE_QUERY_PIPELINE_STATISTICS_SINGLE, i);
         if (!pQuery->counters[i])
            break;
      }
   }
   if (!QueryHasStorage(pQuery)) {
      /* The runtime never destroys a failed CreateQuery handle. Unwind
       * here, including counters allocated before a later failure. */
      ReleaseQueryStorage(pipe, pQuery);
      pQuery->failed = true;
      SetError(hDevice, E_OUTOFMEMORY);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyQuery --
 *
 *    The DestroyQuery function releases resources for a query.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
DestroyQuery(D3D10DDI_HDEVICE hDevice, // IN
             D3D10DDI_HQUERY hQuery)   // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;
   Query *pQuery = CastQuery(hQuery);

   if (pDevice->pPredicate == pQuery) {
      pipe->render_condition(pipe, NULL, false, PIPE_RENDER_COND_WAIT);
      pDevice->pPredicate = NULL;
   }
   ReleaseQueryStorage(pipe, pQuery);
}


/*
 * ----------------------------------------------------------------------
 *
 * QueryBegin --
 *
 *    The QueryBegin function marks the beginning of a sequence of
 *    graphics commands for a query and transitions the query to the
 *    "building" state.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
QueryBegin(D3D10DDI_HDEVICE hDevice,   // IN
           D3D10DDI_HQUERY hQuery)     // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;

   Query *pQuery = CastQuery(hQuery);
   if (!QueryHasStorage(pQuery)) {
      QueryExecutionFailed(hDevice, pQuery);
      return;
   }
   if (pQuery->Type == D3D10DDI_QUERY_EVENT || pQuery->Type == D3D10DDI_QUERY_TIMESTAMP)
      return;

   if (pQuery->handle) {
      if (!pipe->begin_query(pipe, pQuery->handle)) {
         QueryExecutionFailed(hDevice, pQuery);
         return;
      }
   } else {
      for (unsigned i = 0; i < QUERY_STATISTICS_COUNTERS; i++) {
         if (!pipe->begin_query(pipe, pQuery->counters[i])) {
            /* Balance only counters whose Begin actually succeeded. */
            for (unsigned j = 0; j < i; j++)
               pipe->end_query(pipe, pQuery->counters[j]);
            QueryExecutionFailed(hDevice, pQuery);
            return;
         }
      }
   }
   pQuery->active = true;
}


/*
 * ----------------------------------------------------------------------
 *
 * QueryEnd --
 *
 *    The QueryEnd function marks the end of a sequence of graphics
 *    commands for a query and transitions the query to the
 *    "issued" state.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
QueryEnd(D3D10DDI_HDEVICE hDevice,  // IN
         D3D10DDI_HQUERY hQuery)    // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;
   Query *pQuery = CastQuery(hQuery);
   if (!QueryHasStorage(pQuery)) {
      QueryExecutionFailed(hDevice, pQuery);
      return;
   }

   if (pQuery->Type == D3D10DDI_QUERY_EVENT) {
      pipe->screen->fence_reference(pipe->screen, &pQuery->fence, NULL);
      /* Capture this point without submitting the partial command buffer.
       * A no-flush poll must not ask the backend to flush its deferred fence. */
      pipe->flush(pipe, &pQuery->fence, PIPE_FLUSH_DEFERRED);
      if (!pQuery->fence) {
         QueryExecutionFailed(hDevice, pQuery);
         return;
      }
   } else {
      /* The DDI defines End without Begin as an empty begin/end pair for
       * query types that support Begin. Timestamps have implicit Begin. */
      if (!pQuery->active && pQuery->Type != D3D10DDI_QUERY_TIMESTAMP) {
         QueryBegin(hDevice, hQuery);
         if (pQuery->failed)
            return;
      }
      bool ended = true;
      if (pQuery->handle) {
         ended = pipe->end_query(pipe, pQuery->handle);
      } else {
         for (unsigned i = 0; i < QUERY_STATISTICS_COUNTERS; i++) {
            if (!pipe->end_query(pipe, pQuery->counters[i]))
               ended = false;
         }
      }
      pQuery->active = false;
      if (!ended) {
         QueryExecutionFailed(hDevice, pQuery);
         return;
      }
   }
   pQuery->SeqNo = ++pDevice->LastEmittedQuerySeqNo;
}


/*
 * ----------------------------------------------------------------------
 *
 * QueryGetData --
 *
 *    The QueryGetData function polls for the state of a query operation.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
QueryGetData(D3D10DDI_HDEVICE hDevice,                      // IN
             D3D10DDI_HQUERY hQuery,                        // IN
             __out_bcount_full_opt (DataSize) void *pData,  // OUT
             UINT DataSize,                                 // IN
             UINT Flags)                                    // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;
   Query *pQuery = CastQuery(hQuery);
   struct pipe_query *state = pQuery->handle;

   if (!QueryHasStorage(pQuery)) {
      QueryExecutionFailed(hDevice, pQuery);
      return;
   }

   const bool no_flush = (Flags & D3D10_DDI_GET_DATA_DO_NOT_FLUSH) != 0;

   /* QueryGetData polls; it never waits indefinitely for GPU completion.
    * Submit explicitly only when the Windows runtime permits it. */
   if (!no_flush)
      pipe->flush(pipe, NULL, 0);

   union pipe_query_result result;

   memset(&result, 0, sizeof result);

   bool ret;

   if (pQuery->Type == D3D10DDI_QUERY_EVENT) {
      /* A null context and zero timeout observe only already submitted work.
       * Passing the producing context would submit a deferred Zink fence. */
      result.b = pQuery->fence && pipe->screen->fence_finish(pipe->screen, NULL,
                                                           pQuery->fence, 0);
      ret = result.b;
   } else if (state) {
      ret = pipe->get_query_result(pipe, state, false, &result);
   } else if (pQuery->counters[0]) {
      /* Qemu Lightning: gather the single-counter statistics. */
      ret = true;
      for (unsigned i = 0; i < QUERY_STATISTICS_COUNTERS && ret; i++) {
         union pipe_query_result counter = {};
         ret = pipe->get_query_result(pipe, pQuery->counters[i], false, &counter);
         if (ret)
            result.pipeline_statistics.counters[i] = counter.u64;
      }
   } else {
      QueryExecutionFailed(hDevice, pQuery);
      return;
   }

   if (!ret) {
      SetError(hDevice, DXGI_DDI_ERR_WASSTILLDRAWING);
      return;
   }

   if (pData) {
      switch (pQuery->Type) {
      case D3D10DDI_QUERY_EVENT:
      case D3D10DDI_QUERY_OCCLUSIONPREDICATE:
      case D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE:
      case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0:
      case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM1:
      case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM2:
      case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3:
         *(BOOL *)pData = result.b;
         break;
      case D3D10DDI_QUERY_OCCLUSION:
      case D3D10DDI_QUERY_TIMESTAMP:
         *(UINT64 *)pData = result.u64;
         break;
      case D3D10DDI_QUERY_TIMESTAMPDISJOINT:
         {
            D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT *pResult =
              (D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT *)pData;
            pResult->Frequency = result.timestamp_disjoint.frequency;
            pResult->Disjoint = result.timestamp_disjoint.disjoint;
         }
         break;
      case D3D11DDI_QUERY_PIPELINESTATS:
         {
            /* Qemu Lightning: the D3D11 layout adds HS, DS and CS counts. */
            D3D11_DDI_QUERY_DATA_PIPELINE_STATISTICS *pResult =
              (D3D11_DDI_QUERY_DATA_PIPELINE_STATISTICS *)pData;
            pResult->IAVertices = result.pipeline_statistics.ia_vertices;
            pResult->IAPrimitives = result.pipeline_statistics.ia_primitives;
            pResult->VSInvocations = result.pipeline_statistics.vs_invocations;
            pResult->GSInvocations = result.pipeline_statistics.gs_invocations;
            pResult->GSPrimitives = result.pipeline_statistics.gs_primitives;
            pResult->CInvocations = result.pipeline_statistics.c_invocations;
            pResult->CPrimitives = result.pipeline_statistics.c_primitives;
            pResult->PSInvocations = result.pipeline_statistics.ps_invocations;
            pResult->HSInvocations = result.pipeline_statistics.hs_invocations;
            pResult->DSInvocations = result.pipeline_statistics.ds_invocations;
            pResult->CSInvocations = result.pipeline_statistics.cs_invocations;
         }
         break;
      case D3D10DDI_QUERY_PIPELINESTATS:
         {
            D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS *pResult =
              (D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS *)pData;
            pResult->IAVertices = result.pipeline_statistics.ia_vertices;
            pResult->IAPrimitives = result.pipeline_statistics.ia_primitives;
            pResult->VSInvocations = result.pipeline_statistics.vs_invocations;
            pResult->GSInvocations = result.pipeline_statistics.gs_invocations;
            pResult->GSPrimitives = result.pipeline_statistics.gs_primitives;
            pResult->CInvocations = result.pipeline_statistics.c_invocations;
            pResult->CPrimitives = result.pipeline_statistics.c_primitives;
            pResult->PSInvocations = result.pipeline_statistics.ps_invocations;
            //pResult->HSInvocations = result.pipeline_statistics.hs_invocations;
            //pResult->DSInvocations = result.pipeline_statistics.ds_invocations;
            //pResult->CSInvocations = result.pipeline_statistics.cs_invocations;
         }
         break;
      case D3D10DDI_QUERY_STREAMOUTPUTSTATS:
      case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0:
      case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM1:
      case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM2:
      case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM3:
         {
            D3D10_DDI_QUERY_DATA_SO_STATISTICS *pResult =
              (D3D10_DDI_QUERY_DATA_SO_STATISTICS *)pData;
            pResult->NumPrimitivesWritten = result.so_statistics.num_primitives_written;
            pResult->PrimitivesStorageNeeded = result.so_statistics.primitives_storage_needed;
         }
         break;
      default:
         assert(0);
         break;
      }
   }

   /*
    * Keep track of the last finished query, as wgfasync checks that queries
    * are completed in order.
    */
   if ((pQuery->SeqNo - pDevice->LastFinishedQuerySeqNo) > 0) {
      pDevice->LastFinishedQuerySeqNo = pQuery->SeqNo;
   }
}


static bool
IsPredicate(D3D10DDI_QUERY type)
{
   return type == D3D10DDI_QUERY_OCCLUSIONPREDICATE ||
          type == D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE ||
          (type >= D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0 &&
           type <= D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3);
}


/*
 * ----------------------------------------------------------------------
 *
 * SetPredication --
 *
 *    The SetPredication function specifies whether rendering and
 *    resource-manipulation commands that follow are actually performed.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
SetPredication(D3D10DDI_HDEVICE hDevice,  // IN
               D3D10DDI_HQUERY hQuery,    // IN
               BOOL PredicateValue)       // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   struct pipe_context *pipe = pDevice->pipe;
   Query *pQuery = CastQuery(hQuery);
   struct pipe_query *state = CastPipeQuery(hQuery);
   if (pQuery && (!QueryHasStorage(pQuery) || !pQuery->handle || !IsPredicate(pQuery->Type))) {
      QueryExecutionFailed(hDevice, pQuery);
      return;
   }
   enum pipe_render_cond_flag wait;

   wait = (pQuery && pQuery->Flags & D3D10DDI_QUERY_MISCFLAG_PREDICATEHINT) ?
             PIPE_RENDER_COND_NO_WAIT : PIPE_RENDER_COND_WAIT;

   pipe->render_condition(pipe, state, PredicateValue, wait);

   pDevice->pPredicate = pQuery;
   pDevice->PredicateValue = PredicateValue;
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckPredicate --
 *
 *    Check predicate value and whether to draw or not.
 *
 * ----------------------------------------------------------------------
 */

BOOL
CheckPredicate(Device *pDevice)
{
   Query *pQuery = pDevice->pPredicate;
   if (!pQuery) {
      return true;
   }

   const D3D10DDI_HDEVICE hDevice = { pDevice };
   if (!QueryHasStorage(pQuery) || !pQuery->handle || !IsPredicate(pQuery->Type)) {
      QueryExecutionFailed(hDevice, pQuery);
      return false;
   }

   struct pipe_context *pipe = pDevice->pipe;
   union pipe_query_result result = {};
   if (!pipe->get_query_result(pipe, pQuery->handle, true, &result)) {
      /* A failed blocking read is not an unknown predicate that permits
       * execution. Report failure and suppress the guarded operation. */
      QueryExecutionFailed(hDevice, pQuery);
      return false;
   }

   if (!!result.b == !!pDevice->PredicateValue) {
      return false;
   }

   return true;
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckCounterInfo --
 *
 *    The CheckCounterInfo function determines global information that
 *    is related to manipulating counters.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckCounterInfo(D3D10DDI_HDEVICE hDevice,                  // IN
                 __out D3D10DDI_COUNTER_INFO *pCounterInfo) // OUT
{
   //LOG_ENTRYPOINT();

   pCounterInfo->LastDeviceDependentCounter = (D3D10DDI_QUERY)0;
   pCounterInfo->NumSimultaneousCounters = 0;
   pCounterInfo->NumDetectableParallelUnits = 0;
}


/*
 * ----------------------------------------------------------------------
 *
 * CheckCounter --
 *
 *    The CheckCounter function retrieves information that
 *    describes a counter.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CheckCounter(
   D3D10DDI_HDEVICE hDevice,                                                                // IN
   D3D10DDI_QUERY Query,                                                                    // IN
   __out D3D10DDI_COUNTER_TYPE *pCounterType,                                               // OUT
   __out UINT *pActiveCounters,                                                             // OUT
   __out_ecount_part_z_opt (*pNameLength, *pNameLength) LPSTR pName,                        // OUT
   __inout_opt UINT *pNameLength,                                                           // OUT
   __out_ecount_part_z_opt (*pUnitsLength, *pUnitsLength) LPSTR pUnits,                     // OUT
   __inout_opt UINT *pUnitsLength,                                                          // OUT
   __out_ecount_part_z_opt (*pDescriptionLength, *pDescriptionLength) LPSTR pDescription,   // OUT
   __inout_opt UINT* pDescriptionLength)                                                    // OUT
{
   LOG_ENTRYPOINT();

   SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
}
