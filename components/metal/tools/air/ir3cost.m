/*
 * ir3cost: what a shader costs the compiler, measured on this Mac -- the function translated, finalised and
 * compiled by ir3 for the Adreno 830 (rwa_ir3_cost, tools/mesa/air/air_metal.h), with the function constants given
 * as glassref takes them. For deciding before a phone run whether a configuration's shaders would stall the window
 * server: CoreAnimation's specialised uber shaders with boot-arg metal_fc=1 did (runs glass6, glass7).
 *
 *   ir3cost [--fc INDEX=VALUE]... LIB.metallib FUNCTION
 *   ir3cost --sets FILE LIB.metallib FUNCTION      one constant set a line, as "--fc I=V --fc I=V ..."
 *
 * Build: tools/air/ir3cost.sh. The times are this Mac's; the phone's CPU at its usual clocks is slower.
 */
#import <Metal/Metal.h>
#import <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_metal.h"

static uint16_t
half_bits(float f)
{
	__fp16 h = (__fp16)f;
	uint16_t b;
	memcpy(&b, &h, 2);
	return b;
}

static int
one(id<MTLLibrary> lib, const char *name, NSString *set, const char *label)
{
	id<MTLFunction> fn = [lib newFunctionWithName:@(name)];
	if (fn == nil) {
		fprintf(stderr, "ir3cost: no function %s\n", name);
		return 1;
	}
	NSData *air = ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel_getUid("bitcodeData"));
	void *copy = malloc(air.length);
	memcpy(copy, air.bytes, air.length);

	/* the constants, typed by the declaration */
	NSMutableDictionary<NSNumber *, NSString *> *given = [NSMutableDictionary dictionary];
	NSArray *words = [set componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
	for (NSUInteger i = 0; i + 1 < words.count; i++) {
		if ([words[i] isEqualToString:@"--fc"]) {
			NSArray *kv = [words[i + 1] componentsSeparatedByString:@"="];
			if (kv.count == 2) {
				given[@([kv[0] intValue])] = kv[1];
			}
		}
	}
	struct rwa_constant list[64];
	unsigned count = 0;
	for (NSString *cname in fn.functionConstantsDictionary) {
		MTLFunctionConstant *c = fn.functionConstantsDictionary[cname];
		NSString *v = given[@(c.index)];
		if (v == nil || count == 64) {
			continue;
		}
		memset(&list[count], 0, sizeof(list[count]));
		list[count].index = (unsigned)c.index;
		switch (c.type) {
		case MTLDataTypeBool: case MTLDataTypeChar: case MTLDataTypeUChar:
			list[count].bytes[0] = (uint8_t)v.intValue, list[count].size = 1; break;
		case MTLDataTypeUChar2: case MTLDataTypeChar2: list[count].size = 2; list[count].bytes[0] = (uint8_t)v.intValue; break;
		case MTLDataTypeUChar4: case MTLDataTypeChar4: list[count].size = 4; list[count].bytes[0] = (uint8_t)v.intValue; break;
		case MTLDataTypeHalf: { uint16_t h = half_bits(v.floatValue); memcpy(list[count].bytes, &h, 2); list[count].size = 2; break; }
		case MTLDataTypeFloat: { float f = v.floatValue; memcpy(list[count].bytes, &f, 4); list[count].size = 4; break; }
		case MTLDataTypeShort: case MTLDataTypeUShort: { int16_t x = (int16_t)v.intValue; memcpy(list[count].bytes, &x, 2); list[count].size = 2; break; }
		default: { int32_t x = v.intValue; memcpy(list[count].bytes, &x, 4); list[count].size = 4; break; }
		}
		count++;
	}
	if (count) {
		rwa_constants_register(copy, air.length, list, count);
	}
	struct rwa_ir3_cost cost;
	char why[512] = "";
	if (rwa_ir3_cost(copy, air.length, name, 0x44050001, &cost, why, sizeof(why))) {
		printf("%-24s %s: %s\n", label, name, why);
		return 1;
	}
	printf("%-24s %s (%u constants): translate %.0f ms, finalize %.0f ms, ir3 %.0f ms; NIR %u, ir3 %u instr, %u dwords, "
	       "regs %u+%uh, constlen %u; peak %llu MB, NIR %llu MB, driver keeps %llu MB\n", label, name, count, cost.translate_ms,
	       cost.finalize_ms, cost.compile_ms, cost.nir_instructions, cost.instructions, cost.dwords, cost.max_full_reg,
	       cost.max_half_reg, cost.constlen, cost.peak_rss >> 20, cost.retained >> 20, cost.driver_kept >> 20);
	fflush(stdout);
	return 0;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		NSMutableString *set = [NSMutableString string];
		const char *sets = NULL;
		int a = 1;
		for (; a < argc && argv[a][0] == '-'; a++) {
			if (!strcmp(argv[a], "--fc") && a + 1 < argc) {
				[set appendFormat:@" --fc %s", argv[++a]];
			} else if (!strcmp(argv[a], "--sets") && a + 1 < argc) {
				sets = argv[++a];
			} else {
				fprintf(stderr, "usage: ir3cost [--fc I=V]... [--sets FILE] LIB.metallib FUNCTION\n");
				return 2;
			}
		}
		if (argc - a != 2) {
			fprintf(stderr, "usage: ir3cost [--fc I=V]... [--sets FILE] LIB.metallib FUNCTION\n");
			return 2;
		}
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		NSData *b = [NSData dataWithContentsOfFile:@(argv[a])];
		NSError *e = nil;
		id<MTLLibrary> lib = [dev newLibraryWithData:dispatch_data_create(b.bytes, b.length, NULL,
							     DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&e];
		if (lib == nil) {
			fprintf(stderr, "ir3cost: %s\n", e.description.UTF8String);
			return 2;
		}
		if (sets == NULL) {
			return one(lib, argv[a + 1], set, "given");
		}
		NSString *text = [NSString stringWithContentsOfFile:@(sets) encoding:NSUTF8StringEncoding error:nil];
		int bad = 0, n = 0;
		for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
			if (line.length == 0) {
				continue;
			}
			char label[32];
			snprintf(label, sizeof(label), "set %d", ++n);
			bad |= one(lib, argv[a + 1], line, label);
		}
		return bad;
	}
}
