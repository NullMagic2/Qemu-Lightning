/* Module: lightning.guest_vulkan.zink_cross_context_map_acceptance
 * Purpose: Prove a map cannot wait forever for another context's unflushed batch.
 * Created: 2026-10-16
 * Architecture: Links the pinned Zink and Gallium archives the Windows UMD links and
 * runs on any Vulkan device (Wine on host lavapipe in validation). Two contexts share
 * one screen, as two D3D devices in one process do. Context A records a clear and does
 * not flush; context B then maps the image for reading, as WddmPresent publishes a
 * frame. Upstream Zink waits for A to flush, which never happens, so B hangs and the
 * watchdog fails the run (Solitaire's frame window after a minimize). After A flushes,
 * B's map must wait for that submitted work and read the clear colour.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/box.h"
#include "zink_batch.h"
#include "zink_context.h"
#include "zink_public.h"
#include "zink_resource.h"
#include "zink_screen.h"

static const unsigned ROUNDS = 64;
static const unsigned IMAGE_SIZE = 64;
static const DWORD HANG_TIMEOUT_MS = 10000;

static volatile LONG progress;
static volatile LONG done;
static volatile LONG current_round;
static const char *volatile current_step = "setup";

static void
mark(const char *step)
{
   current_step = step;
   InterlockedIncrement(&progress);
}

static DWORD WINAPI
watchdog(void *unused)
{
   (void)unused;
   for (;;) {
      LONG seen = progress;
      Sleep(HANG_TIMEOUT_MS);
      if (seen == progress && !done) {
         printf("FAIL: round %ld hung in %s for %lu ms (map waited for another "
                "context's unflushed batch)\n", current_round, current_step,
                (unsigned long)HANG_TIMEOUT_MS);
         fflush(stdout);
         TerminateProcess(GetCurrentProcess(), 3);
      }
   }
   return 0;
}

/* Maps the image on reader and compares the four corners with texel. Returns 1 when
 * they match, 0 when they differ and -1 when the map fails. */
static int
corners_match(struct pipe_context *reader, struct pipe_resource *image, const uint8_t *texel)
{
   struct pipe_box box;
   struct pipe_transfer *transfer = NULL;
   int match = 1;

   u_box_2d(0, 0, IMAGE_SIZE, IMAGE_SIZE, &box);
   const uint8_t *pixels = reader->texture_map(reader, image, 0, PIPE_MAP_READ, &box, &transfer);
   if (!pixels)
      return -1;
   for (unsigned y = 0; y < IMAGE_SIZE; y += IMAGE_SIZE - 1)
      for (unsigned x = 0; x < IMAGE_SIZE; x += IMAGE_SIZE - 1)
         if (memcmp(pixels + y * transfer->stride + x * 4, texel, 4))
            match = 0;
   reader->texture_unmap(reader, transfer);
   return match;
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
   struct pipe_context *writer = screen->context_create(screen, NULL, 0);
   struct pipe_context *reader = screen->context_create(screen, NULL, 0);
   struct pipe_resource templ = {0};
   templ.target = PIPE_TEXTURE_2D;
   templ.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   templ.width0 = IMAGE_SIZE;
   templ.height0 = IMAGE_SIZE;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.usage = PIPE_USAGE_DEFAULT;
   templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *image = writer && reader ? screen->resource_create(screen, &templ) : NULL;
   if (!image) {
      printf("FAIL: context or image creation\n");
      return 2;
   }

   unsigned recording_maps = 0;
   for (unsigned index = 0; index < ROUNDS; ++index) {
      struct pipe_box box;
      uint8_t texel[4] = {(uint8_t)index, (uint8_t)(index >> 8), 0xa5, 0xff};

      InterlockedExchange(&current_round, (LONG)index);
      mark("clear");
      u_box_2d(0, 0, IMAGE_SIZE, IMAGE_SIZE, &box);
      writer->clear_texture(writer, image, 0, &box, texel);
      struct zink_resource *res = zink_resource(image);
      struct zink_batch_usage *usage = res->obj->bo->writes.u;
      if (usage != &zink_context(writer)->bs->usage || !usage->unflushed) {
         printf("FAIL: round %u setup: the clear is not in the writer's recording batch\n",
                index);
         return 2;
      }

      /* The writer has not flushed: the reader must not wait for it. */
      mark("map while the writer records");
      if (corners_match(reader, image, texel) < 0) {
         printf("FAIL: round %u map during recording returned NULL\n", index);
         return 2;
      }
      ++recording_maps;

      /* Once the writer flushes, the reader must see the submitted clear. */
      mark("writer flush");
      writer->flush(writer, NULL, index & 1 ? PIPE_FLUSH_ASYNC : 0);
      mark("map after the writer flushed");
      int match = corners_match(reader, image, texel);
      if (match != 1) {
         printf("FAIL: round %u map after the writer flushed: %s\n", index,
                match < 0 ? "NULL" : "not the clear colour");
         return 2;
      }
   }
   done = 1;
   pipe_resource_reference(&image, NULL);
   reader->destroy(reader);
   writer->destroy(writer);
   screen->destroy(screen);
   printf("PASS: %u rounds: map while another context records returned (%u), map after its "
          "sync/async flush read the clear colour\n", ROUNDS, recording_maps);
   return 0;
}
