/* SPDX-License-Identifier: MIT */
#include "nvk_device.h"
#include "nvk_physical_device.h"
#include "nvk_shader.h"
#include "vk_alloc.h"

#include "clb197.h"

#include <stdio.h>
#include <stdlib.h>

#define NO_VALUE UINT32_MAX

/* The value the push's last write of `method` carries, NO_VALUE where it has
 * none: Fermi's method headers (nv_push.h), every opcode NVK emits
 */
static uint32_t
last_write(const uint32_t *dw, uint32_t count, uint32_t method)
{
   uint32_t value = NO_VALUE;
   for (uint32_t i = 0; i < count;) {
      const uint32_t hdr = dw[i++];
      const uint32_t opcode = hdr >> 29, at = (hdr & 0x1fff) << 2;
      const uint32_t n = (hdr >> 16) & 0x1fff;
      if (opcode == 4) {
         if (at == method)
            value = n;
         continue;
      }
      for (uint32_t k = 0; k < n && i < count; k++, i++) {
         uint32_t m = at;
         if (opcode == 1)
            m = at + 4 * k;
         else if (opcode == 5)
            m = k ? at + 4 : at;
         if (m == method)
            value = dw[i];
      }
   }
   return value;
}

static int failures;

static void
check(bool ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      failures++;
   }
}

static uint32_t
fragment_push(struct nvk_device *dev, uint8_t bound_textures, uint32_t method)
{
   struct nvk_shader *shader = calloc(1, sizeof(*shader));
   shader->info.stage = MESA_SHADER_FRAGMENT;
   shader->info.bound_textures = bound_textures;
   shader->hdr_addr = 0x1000;
   if (nvk_shader_fill_push(dev, shader, vk_default_allocator()) != VK_SUCCESS) {
      check(false, "nvk_shader_fill_push");
      return NO_VALUE;
   }
   check(shader->push_dw_count <=
            nvk_max_shader_push_dw(nvk_device_physical(dev),
                                   MESA_SHADER_FRAGMENT, false),
         "the fragment push fits nvk_max_shader_push_dw");
   const uint32_t value = last_write(shader->push_dw, shader->push_dw_count,
                                     method);
   vk_free(vk_default_allocator(), shader->push_dw);
   free(shader);
   return value;
}

int
main(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   struct nvk_physical_device *pdev = calloc(1, sizeof(*pdev));
   struct nvk_device *dev = calloc(1, sizeof(*dev));
   pdev->info.cls_eng3d = MAXWELL_B;
   dev->vk.physical = &pdev->vk;
   dev->vk.enabled_features.fragmentShaderPixelInterlock = true;

   check(fragment_push(dev, 3, NVB197_SET_BINDLESS_TEXTURE) == 2,
         "a fragment shader whose bound textures read constant buffer 2 "
         "selects it with SET_BINDLESS_TEXTURE");
   check(fragment_push(dev, 1, NVB197_SET_BINDLESS_TEXTURE) == 0,
         "constant buffer 0 is selected too");
   check(fragment_push(dev, 0, NVB197_SET_BINDLESS_TEXTURE) == NO_VALUE,
         "a fragment shader without bound textures leaves it");

   free(dev);
   free(pdev);
   if (failures == 0)
      printf("bound textures: ok\n");
   return failures ? 1 : 0;
}
