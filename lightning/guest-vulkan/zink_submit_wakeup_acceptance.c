/* Module: lightning.guest_vulkan.zink_submit_wakeup_acceptance
 * Purpose: Prove a map after an async flush cannot lose Zink's threaded-submit wakeup.
 * Created: 2026-10-16
 * Architecture: Links the pinned Zink and Gallium archives the Windows UMD links and
 * runs on any Vulkan device (Wine on host lavapipe in validation). One context with
 * threaded submit clears an image, flushes asynchronously, then maps the image for
 * reading, as WddmPresent publishes a frame. A gate job on Zink's flush queue delays
 * the submit, and a helper holds the batch's usage mutex until the map is blocked on
 * it, so the waiter's check-then-wait window becomes one deterministic step. With the
 * lost wakeup the map never returns and the watchdog fails the run.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/box.h"
#include "util/u_queue.h"
#include "zink_batch.h"
#include "zink_context.h"
#include "zink_public.h"
#include "zink_resource.h"
#include "zink_screen.h"

static const unsigned HELD_ROUNDS = 64;
static const unsigned FREE_ROUNDS = 256;
static const unsigned IMAGE_SIZE = 64;
static const unsigned FREE_SUBMIT_DELAY_US = 300;
static const int64_t STEP_TIMEOUT_NS = 2000000000;
static const DWORD HANG_TIMEOUT_MS = 10000;

enum step { STEP_SETUP, STEP_FLUSH, STEP_MAP, STEP_CHECK, STEP_DONE };
static const char *const STEP_NAMES[] = {"setup", "flush", "map", "check", "done"};

struct round_state {
   struct zink_batch_state *bs;
   unsigned delay_us;
   volatile LONG gate_open;
   volatile LONG mutex_held;
   volatile LONG map_started;
   /* Outcomes the helper saw while it held the usage mutex. */
   volatile LONG waiter_blocked;
   volatile LONG submitter_blocked;
   volatile LONG submitted_while_held;
};

static volatile LONG progress;
static volatile LONG current_step = STEP_SETUP;
static volatile LONG current_round;
static struct round_state *volatile active_round;

static void
mark(enum step step)
{
   InterlockedExchange(&current_step, step);
   InterlockedIncrement(&progress);
}

static DWORD WINAPI
watchdog(void *unused)
{
   (void)unused;
   for (;;) {
      LONG seen = progress;
      Sleep(HANG_TIMEOUT_MS);
      if (seen == progress && current_step != STEP_DONE) {
         struct round_state *round = active_round;
         printf("FAIL: round %ld hung in %s for %lu ms (lost threaded-submit wakeup): map "
                "blocked on the usage mutex %ld, submit finished while it was held %ld, "
                "batch unflushed %d\n", current_round, STEP_NAMES[current_step],
                (unsigned long)HANG_TIMEOUT_MS, round->waiter_blocked,
                round->submitted_while_held, round->bs ? round->bs->usage.unflushed : -1);
         fflush(stdout);
         TerminateProcess(GetCurrentProcess(), 3);
      }
   }
   return 0;
}

/* The artificial submit delay: Zink's flush queue has one thread and runs jobs in
 * order, so the batch flushed after this job stays unflushed until it returns. */
static void
gate_job(void *data, void *gdata, int thread_index)
{
   struct round_state *round = data;
   int64_t deadline = os_time_get_nano() + STEP_TIMEOUT_NS;
   (void)gdata;
   (void)thread_index;
   if (round->delay_us) {
      os_time_sleep(round->delay_us);
      return;
   }
   while (!round->gate_open && os_time_get_nano() < deadline)
      Sleep(0);
}

static volatile LONG *
lock_count(mtx_t *mutex)
{
   return &mutex->LockCount;
}

static bool
wait_until_changed(volatile LONG *value, LONG from, struct zink_batch_state *bs,
                   bool stop_on_submit)
{
   int64_t deadline = os_time_get_nano() + STEP_TIMEOUT_NS;
   while (*value == from && os_time_get_nano() < deadline) {
      if (stop_on_submit && p_atomic_read(&bs->fence.submitted))
         return false;
      Sleep(0);
   }
   return *value != from;
}

/* Models a waiter preempted between its unflushed check and its cnd_wait: the map
 * blocks on the usage mutex while the submit runs. Any waiter count change of the
 * critical section after the map started is the map; the next one is the submitter
 * taking the same lock, which only the fixed Zink does. */
static DWORD WINAPI
hold_usage_mutex(void *data)
{
   struct round_state *round = data;
   struct zink_batch_state *bs = round->bs;
   volatile LONG *count = lock_count(&bs->usage.mtx);

   mtx_lock(&bs->usage.mtx);
   LONG alone = *count;
   InterlockedExchange(&round->mutex_held, 1);
   while (!round->map_started)
      Sleep(0);
   if (wait_until_changed(count, alone, bs, false)) {
      InterlockedExchange(&round->waiter_blocked, 1);
      LONG one_waiter = *count;
      InterlockedExchange(&round->gate_open, 1);
      if (wait_until_changed(count, one_waiter, bs, true))
         InterlockedExchange(&round->submitter_blocked, 1);
      else if (p_atomic_read(&bs->fence.submitted))
         InterlockedExchange(&round->submitted_while_held, 1);
   } else {
      InterlockedExchange(&round->gate_open, 1);
   }
   mtx_unlock(&bs->usage.mtx);
   return 0;
}

static int
run_round(struct pipe_context *pipe, struct pipe_resource *image, unsigned index,
          bool hold, struct round_state *round)
{
   struct zink_context *ctx = zink_context(pipe);
   struct zink_screen *screen = zink_screen(pipe->screen);
   struct zink_resource *res = zink_resource(image);
   struct util_queue_fence gate_done;
   uint8_t texel[4] = {(uint8_t)index, (uint8_t)(index >> 8), 0x5a, 0xff};
   struct pipe_box box;
   HANDLE helper = NULL;
   int result = 0;

   InterlockedExchange(&current_round, (LONG)index);
   memset(round, 0, sizeof(*round));
   active_round = round;
   round->delay_us = hold ? 0 : FREE_SUBMIT_DELAY_US;
   util_queue_fence_init(&gate_done);
   util_queue_add_job(&screen->flush_queue, round, &gate_done, gate_job, NULL, 0);

   mark(STEP_FLUSH);
   u_box_2d(0, 0, IMAGE_SIZE, IMAGE_SIZE, &box);
   pipe->clear_texture(pipe, image, 0, &box, texel);
   /* Not waiting for the submit, as a D3D event-query poll flushes its deferred
    * fence (zink_fence.c) and a DO_NOT_WAIT map flushes busy work (Resource.cpp). */
   pipe->flush(pipe, NULL, PIPE_FLUSH_ASYNC);

   struct zink_batch_usage *usage = res->obj->bo->writes.u;
   if (!zink_resource_usage_is_unflushed_write(res) || usage == &ctx->bs->usage) {
      printf("FAIL: round %u setup: the clear is not in a queued, unflushed batch "
             "(write usage %p, current batch %p, unflushed %d)\n", index, (void *)usage,
             (void *)&ctx->bs->usage, usage ? usage->unflushed : -1);
      InterlockedExchange(&round->gate_open, 1);
      util_queue_fence_wait(&gate_done);
      return 2;
   }
   round->bs = (struct zink_batch_state *)((char *)usage -
                                           offsetof(struct zink_batch_state, usage));
   if (hold) {
      helper = CreateThread(NULL, 0, hold_usage_mutex, round, 0, NULL);
      while (!round->mutex_held)
         Sleep(0);
   }

   mark(STEP_MAP);
   struct pipe_transfer *transfer = NULL;
   InterlockedExchange(&round->map_started, 1);
   const uint8_t *pixels = pipe->texture_map(pipe, image, 0, PIPE_MAP_READ, &box, &transfer);

   mark(STEP_CHECK);
   if (!pixels) {
      printf("FAIL: round %u map returned NULL\n", index);
      result = 2;
   } else {
      for (unsigned y = 0; y < IMAGE_SIZE && !result; y += IMAGE_SIZE - 1)
         for (unsigned x = 0; x < IMAGE_SIZE && !result; x += IMAGE_SIZE - 1)
            if (memcmp(pixels + y * transfer->stride + x * 4, texel, 4)) {
               printf("FAIL: round %u texel (%u,%u) is not the clear colour\n", index, x, y);
               result = 2;
            }
      pipe->texture_unmap(pipe, transfer);
   }
   if (helper) {
      WaitForSingleObject(helper, INFINITE);
      CloseHandle(helper);
   }
   util_queue_fence_wait(&gate_done);
   util_queue_fence_destroy(&gate_done);
   return result;
}

int
main(void)
{
   CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
   struct pipe_screen *screen = zink_create_screen(NULL, NULL);
   if (!screen) {
      printf("FAIL: zink_create_screen\n");
      return 2;
   }
   if (!zink_screen(screen)->threaded_submit) {
      printf("FAIL: threaded submit is off (needs >1 CPU, no GALLIUM_THREAD=0, no flushsync)\n");
      return 2;
   }
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   struct pipe_resource templ = {0};
   templ.target = PIPE_TEXTURE_2D;
   templ.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   templ.width0 = IMAGE_SIZE;
   templ.height0 = IMAGE_SIZE;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *image = pipe ? screen->resource_create(screen, &templ) : NULL;
   if (!image) {
      printf("FAIL: context or image creation\n");
      return 2;
   }

   struct round_state round;
   unsigned blocked = 0, submitter_blocked = 0, submitted_while_held = 0;
   int64_t start = os_time_get_nano();
   for (unsigned index = 0; index < HELD_ROUNDS; ++index) {
      if (run_round(pipe, image, index, true, &round))
         return 2;
      blocked += round.waiter_blocked;
      submitter_blocked += round.submitter_blocked;
      submitted_while_held += round.submitted_while_held;
   }
   int64_t held_ns = os_time_get_nano() - start;
   start = os_time_get_nano();
   for (unsigned index = 0; index < FREE_ROUNDS; ++index)
      if (run_round(pipe, image, HELD_ROUNDS + index, false, &round))
         return 2;
   int64_t free_ns = os_time_get_nano() - start;
   mark(STEP_DONE);

   printf("held rounds %u: map blocked on the usage mutex %u, submitter waited for it %u, "
          "submit finished while it was held %u (%.2f ms/round)\n",
          HELD_ROUNDS, blocked, submitter_blocked, submitted_while_held,
          held_ns / 1e6 / HELD_ROUNDS);
   printf("free rounds %u with a %u us submit delay (%.2f ms/round)\n", FREE_ROUNDS,
          FREE_SUBMIT_DELAY_US, free_ns / 1e6 / FREE_ROUNDS);
   if (blocked != HELD_ROUNDS) {
      printf("FAIL: the map reached the usage mutex in only %u of %u rounds\n", blocked,
             HELD_ROUNDS);
      return 2;
   }
   pipe_resource_reference(&image, NULL);
   pipe->destroy(pipe);
   screen->destroy(screen);
   printf("PASS: %u flush-then-map rounds survived a submit racing the waiter, "
          "%u free-running rounds\n", HELD_ROUNDS, FREE_ROUNDS);
   return 0;
}
