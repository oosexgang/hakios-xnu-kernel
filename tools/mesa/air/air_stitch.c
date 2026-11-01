/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * A function-stitching graph, typed as text, and the functions it names.
 *
 * The graph itself is Metal's: `air_stitch_graph` in nir_air.h is
 * MTLFunctionStitchingGraph field for field. What is here is the one thing
 * Metal has no equivalent of -- a way to write a graph on a command line --
 * and it is in the frontend's library rather than in one tool because every
 * tool that touches stitching needs it: air2nir folds a graph, air_run
 * dispatches one, and the Metal library's test will build the same graphs
 * through the objective API, where they have to be the same graphs or the
 * comparison is between two different things.
 *
 *   "_pf_invertRed(in0); _pf_redContour(n0); _pf_textureDiff(n0, n1)"
 *
 * One node per `;', in dependency order, numbered by position. An argument is
 * `inN' (the stitched function's own argument N), `nN' (node N's result),
 * `texN' / `sampN' / `bufN' (a texture, a sampler or a constant buffer bound at
 * index N), or `gridpos'. The output node is the last one unless the text ends
 * with `=> nN'.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir_air.h"
#include "util/macros.h"
#include "util/ralloc.h"

/* Every refusal goes into the caller's buffer rather than to stderr: this is
 * library code, and a tool that wants the sentence on one line with its own
 * prefix cannot get it back out of stderr.
 */
static void PRINTFLIKE(3, 4)
say(char *error, size_t size, const char *fmt, ...)
{
   if (!error || !size)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(error, size, fmt, ap);
   va_end(ap);
}

static char *
trim(char *s)
{
   while (*s == ' ' || *s == '\t')
      s++;
   char *e = s + strlen(s);
   while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
      *--e = '\0';
   return s;
}

static bool
parse_arg(const char *tok, struct air_stitch_arg *out)
{
   static const struct { const char *prefix; enum air_stitch_arg_kind kind; } kinds[] = {
      /* "in" before "n", or `in0' would read as node 0 of a prefix "n" */
      { "in",   AIR_STITCH_INPUT },
      { "tex",  AIR_STITCH_TEXTURE },
      { "samp", AIR_STITCH_SAMPLER },
      { "buf",  AIR_STITCH_BUFFER },
      { "n",    AIR_STITCH_NODE },
   };
   if (!strcmp(tok, "gridpos")) {
      *out = (struct air_stitch_arg){ .kind = AIR_STITCH_GRID_POSITION };
      return true;
   }
   for (unsigned k = 0; k < ARRAY_SIZE(kinds); k++) {
      size_t len = strlen(kinds[k].prefix);
      if (strncmp(tok, kinds[k].prefix, len))
         continue;
      char *end = NULL;
      unsigned long v = strtoul(tok + len, &end, 10);
      if (!end || *end || end == tok + len)
         continue;
      *out = (struct air_stitch_arg){ .kind = kinds[k].kind, .index = (unsigned)v };
      return true;
   }
   return false;
}

/* "f(a, b)" -> one node.
 *
 * strtok_r and not strtok, here and in the caller: strtok keeps one buffer for
 * the whole program, so splitting a node's arguments inside the loop that
 * splits the graph ENDED that loop -- a two-node graph parsed as one node and
 * the shader stored node 0's result. That is exactly what a stitched call that
 * had not been wired would look like, and it was the parser.
 */
static bool
parse_node(void *ctx, char *text, struct air_stitch_node *node,
           char *error, size_t size)
{
   text = trim(text);
   char *open = strchr(text, '(');
   char *close = strrchr(text, ')');
   if (!open || !close || close < open) {
      say(error, size, "`%s' is not `function(arguments)'", text);
      return false;
   }
   *open = *close = '\0';
   node->function = ralloc_strdup(ctx, trim(text));

   char *args = open + 1;
   unsigned n = 0;
   bool empty = true;
   for (char *p = args; *p; p++) {
      n += *p == ',';
      empty &= *p == ' ' || *p == '\t';
   }
   n = empty ? 0 : n + 1;

   struct air_stitch_arg *out = rzalloc_array(ctx, struct air_stitch_arg, MAX2(n, 1));
   unsigned idx = 0;
   char *save = NULL;
   for (char *tok = strtok_r(args, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
      tok = trim(tok);
      if (idx >= n || !parse_arg(tok, &out[idx++])) {
         say(error, size, "`%s' is not inN, nN, texN, sampN, bufN or gridpos", tok);
         return false;
      }
   }
   node->num_args = idx;
   node->args = out;
   return true;
}

const struct air_stitch_graph *
air_stitch_parse(void *mem_ctx, const char *text, char *error, size_t size)
{
   char *copy = ralloc_strdup(mem_ctx, text);
   struct air_stitch_graph *g = rzalloc(mem_ctx, struct air_stitch_graph);
   g->name = "stitched";
   g->output_node = ~0u;

   char *arrow = strstr(copy, "=>");
   if (arrow) {
      *arrow = '\0';
      char *tok = trim(arrow + 2);
      char *e = tok + strlen(tok);
      while (e > tok && e[-1] == ';')
         *--e = '\0';
      struct air_stitch_arg which;
      if (!parse_arg(trim(tok), &which) || which.kind != AIR_STITCH_NODE) {
         say(error, size, "`=> %s' is not `=> nN'", tok);
         return NULL;
      }
      g->output_node = which.index;
   }

   unsigned count = 1;
   for (const char *p = copy; *p; p++)
      count += *p == ';';
   struct air_stitch_node *nodes = rzalloc_array(mem_ctx, struct air_stitch_node, count);
   unsigned n = 0;
   char *save = NULL;
   for (char *tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
      if (!*trim(tok))
         continue;
      if (n >= count || !parse_node(mem_ctx, tok, &nodes[n++], error, size))
         return NULL;
   }
   if (!n) {
      say(error, size, "no nodes");
      return NULL;
   }
   g->num_nodes = n;
   g->nodes = nodes;
   if (g->output_node == ~0u)
      g->output_node = n - 1;
   if (g->output_node >= n) {
      say(error, size, "the output node is n%u of %u nodes", g->output_node, n);
      return NULL;
   }
   return g;
}

unsigned
air_stitch_functions_of(void *mem_ctx, const struct air_stitch_graph *g,
                        const void *data, size_t size,
                        struct air_stitch_function **out, char *error, size_t esize)
{
   struct air_metallib lib;
   const char *why = NULL;
   if (!air_metallib_parse(mem_ctx, data, size, &lib, &why)) {
      say(error, esize, "the container: %s", why ? why : "?");
      return 0;
   }

   struct air_stitch_function *fns =
      rzalloc_array(mem_ctx, struct air_stitch_function, MAX2(g->num_nodes, 1));
   unsigned num = 0;
   for (unsigned i = 0; i < g->num_nodes; i++) {
      const char *name = g->nodes[i].function;
      bool have = false;
      for (unsigned k = 0; k < num; k++)
         have |= !strcmp(fns[k].name, name);
      if (have)
         continue;      /* two nodes calling one function need it once */

      const struct air_metallib_function *found = NULL;
      for (unsigned k = 0; k < lib.num_functions; k++)
         if (!strcmp(lib.functions[k].name, name))
            found = &lib.functions[k];
      if (!found) {
         say(error, esize, "%s is not a function of this library", name);
         return 0;
      }
      fns[num++] = (struct air_stitch_function){
         found->name, found->bitcode, found->bitcode_size,
      };
   }
   *out = fns;
   return num;
}
