/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * The MTLB container of a .metallib: a header, a function list of tagged
 * fields, and one LLVM module per function. Worked out against the files of
 * iOS 16.5 (docs/metal.md, section 9.1; tools/metallib.py is the same in
 * Python).
 */

#include "nir_air.h"

#include <string.h>

#include "util/ralloc.h"

static uint64_t
rd64(const uint8_t *p)
{
   uint64_t v = 0;
   for (int i = 7; i >= 0; i--)
      v = v << 8 | p[i];
   return v;
}

static uint32_t
rd32(const uint8_t *p)
{
   return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t
rd16(const uint8_t *p)
{
   return p[0] | p[1] << 8;
}

bool
air_metallib_parse(void *mem_ctx, const void *data, size_t size, struct air_metallib *lib,
                   const char **error)
{
   const uint8_t *b = data;
   memset(lib, 0, sizeof(*lib));
   *error = NULL;

   if (size < 0x58 || memcmp(b, "MTLB", 4) != 0) {
      *error = "not an MTLB container";
      return false;
   }
   lib->version_major = rd16(b + 4);
   lib->version_minor = rd16(b + 6);
   uint64_t total = rd64(b + 0x10);
   uint64_t funcs = rd64(b + 0x18);
   uint64_t bc_off = rd64(b + 0x48);
   uint64_t bc_len = rd64(b + 0x50);
   if (total > size || funcs + 4 > size || bc_off + bc_len > size) {
      *error = "MTLB header points past the file";
      return false;
   }

   const uint8_t *p = b + funcs;
   uint32_t count = rd32(p);
   p += 4;
   lib->functions = rzalloc_array(mem_ctx, struct air_metallib_function, count ? count : 1);
   for (uint32_t i = 0; i < count; i++) {
      if (p + 4 > b + size) {
         *error = "function list runs past the file";
         return false;
      }
      uint32_t len = rd32(p);
      const uint8_t *q = p + 4, *end = p + len;
      if (len < 4 || end > b + size) {
         *error = "function entry runs past the file";
         return false;
      }
      struct air_metallib_function *f = &lib->functions[lib->num_functions++];
      f->type = -1;
      uint64_t module_off = 0, module_size = 0;
      bool have_off = false;
      while (q + 6 <= end && memcmp(q, "ENDT", 4) != 0) {
         uint16_t vlen = rd16(q + 4);
         const uint8_t *v = q + 6;
         if (v + vlen > end) {
            *error = "function field runs past its entry";
            return false;
         }
         if (!memcmp(q, "NAME", 4)) {
            f->name = ralloc_strndup(mem_ctx, (const char *)v, vlen);
         } else if (!memcmp(q, "TYPE", 4) && vlen >= 1) {
            f->type = v[0];
         } else if (!memcmp(q, "OFFT", 4) && vlen >= 24) {
            module_off = rd64(v + 16);
            have_off = true;
         } else if (!memcmp(q, "MDSZ", 4) && vlen >= 8) {
            module_size = rd64(v);
         }
         q = v + vlen;
      }
      if (!f->name || !have_off || !module_size) {
         *error = "function entry without NAME, OFFT or MDSZ";
         return false;
      }
      if (module_off + module_size > bc_len) {
         *error = "function module outside the bitcode section";
         return false;
      }
      f->bitcode = b + bc_off + module_off;
      f->bitcode_size = module_size;
      p = end;
   }
   return true;
}

const char *
air_metallib_type_name(int type)
{
   /* the container's TYPE tag; 4 is what the 135 stitchable functions of
    * PortraitFilters carry, whose metadata says air.visible
    */
   switch (type) {
   case 0: return "vertex";
   case 1: return "fragment";
   case 2: return "kernel";
   case 3: return "unqualified";
   case 4: return "visible";
   case 5: return "extern";
   case 6: return "intersection";
   default: return "unknown";
   }
}
