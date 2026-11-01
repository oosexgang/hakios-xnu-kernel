#!/usr/bin/env python3
"""Checks every constant in include/hakios-metal/a6xx.hpp against Mesa's generated register headers.

    python3 -I tools/check-a6xx.py [MESA_GEN_DIR] [A6XX_HPP]

MESA_GEN_DIR is the directory with a6xx.xml.h, adreno_pm4.xml.h and adreno_common.xml.h, as Mesa's build writes
them (build/src/freedreno/registers/adreno/); when it is not given, the environment variable HAKIOS_MESA_GEN is.
A6XX_HPP defaults to the header of the project this script is in.

What is checked, by name:
  - every `constexpr <type> NAME = VALUE;` and every enumerator `NAME = VALUE,` in a6xx.hpp is looked up among the
    headers' #defines and enumerators, and the values must be equal;
  - a NAME__SCALE constant is compared with the right shift Mesa's encoder NAME(val) applies in its body
    (`val >> N`; 0 when there is none);
  - a NAME__MUL constant is compared with the multiplier Mesa's encoder NAME(val) applies in its body
    (`val * N.0`, the fixed-point fields: GRAS_SU_CNTL.LINEHALFWIDTH is quarter pixels, the point sizes sixteenths;
    0 when there is none);
  - a register array NAME (a `#define NAME(i0) (BASE + STRIDE*i0)` or an inline `NAME(uint32_t i0)` in the
    headers, with or without the `__attribute__((deprecated))` two of them carry) is checked as NAME = BASE and
    NAME__STRIDE = STRIDE;
  - a `// not-in-headers: why` comment marks a constant the headers cannot check; it is listed, not checked.
Anything else is an error: a constant the headers do not have, a value that differs, a constant without a
`file.xml.h:line` comment saying where it comes from. The exit status is 0 only when every constant was checked or
marked. A line comment that does not point at the constant's own line in the header is reported as a warning (the
line numbers are those of one Mesa version's output; the name check is what matters).
"""
import ast
import os
import re
import sys

HEADERS = ("a6xx.xml.h", "adreno_pm4.xml.h", "adreno_common.xml.h")


def read_headers(gen_dir):
    """#defines, enumerators and encoder shifts of the generated headers, each with where it is."""
    defines = {}  # name -> [(value, file, line)]
    enums = {}
    scales = {}  # encoder name -> (shift, file, line)
    muls = {}  # encoder name -> (multiplier, file, line)
    arrays = {}  # register array name -> (base, stride, file, line)
    text = {}  # file -> its lines
    array_re = r"\(\s*(0x[0-9a-fA-F]+|\d+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\s*\*\s*i0\s*\)"
    for fname in HEADERS:
        path = os.path.join(gen_dir, fname)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.read().split("\n")
        except OSError as e:
            sys.exit(f"cannot read {path}: {e}")
        text[fname] = lines
        in_enum = False
        fn = None
        body = []
        for i, line in enumerate(lines, 1):
            m = re.match(r"#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*$", line)
            if m:
                defines.setdefault(m.group(1), []).append((int(m.group(2), 0), fname, i))
                continue
            m = re.match(r"#define\s+(\w+)\(i0\)\s+" + array_re + r"\s*$", line)
            if m:
                arrays[m.group(1)] = (int(m.group(2), 0), int(m.group(3), 0), fname, i)
                continue
            m = re.match(r"static inline (?:__attribute__\(\(deprecated\)\)\s+)?uint32_t (\w+)\(uint32_t i0\)\s*\{\s*return\s+" +
                         r"(0x[0-9a-fA-F]+|\d+)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\s*\*\s*i0;\s*\}", line)
            if m:
                arrays[m.group(1)] = (int(m.group(2), 0), int(m.group(3), 0), fname, i)
                continue
            if re.match(r"enum\s+\w+\s*\{", line):
                in_enum = True
                continue
            if in_enum:
                if line.startswith("}"):
                    in_enum = False
                    continue
                m = re.match(r"\s*(\w+)\s*=\s*(-?0x[0-9a-fA-F]+|-?\d+)\s*,?\s*$", line)
                if m:
                    enums.setdefault(m.group(1), []).append((int(m.group(2), 0), fname, i))
                continue
            # a multi-line encoder: `static inline uint32_t NAME(type val)` then its body to the closing brace
            m = re.match(r"static inline uint32_t (\w+)\(.*\)\s*$", line)
            if m:
                fn = (m.group(1), i)
                body = []
                continue
            if fn is not None:
                if line.startswith("}"):
                    s = re.search(r"val\s*>>\s*(\d+)", " ".join(body))
                    scales[fn[0]] = (int(s.group(1)) if s else 0, fname, fn[1])
                    s = re.search(r"val\s*\*\s*(\d+)\.0", " ".join(body))
                    muls[fn[0]] = (int(s.group(1)) if s else 0, fname, fn[1])
                    fn = None
                else:
                    body.append(line)
    return defines, enums, scales, muls, arrays, text


def evaluate(expr, env):
    """A C++ constant expression made of literals, names defined earlier in the header, and | & << >> + - * ~."""
    expr = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uU]?[lL]{0,2}\b", r"\1", expr)
    expr = re.sub(r"\btrue\b", "True", expr)
    expr = re.sub(r"\bfalse\b", "False", expr)
    ops = {
        ast.BitOr: lambda a, b: a | b,
        ast.BitAnd: lambda a, b: a & b,
        ast.LShift: lambda a, b: a << b,
        ast.RShift: lambda a, b: a >> b,
        ast.Add: lambda a, b: a + b,
        ast.Sub: lambda a, b: a - b,
        ast.Mult: lambda a, b: a * b,
    }

    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, bool)):
            return int(n.value)
        if isinstance(n, ast.Name):
            if n.id in env:
                return env[n.id]
            raise ValueError(f"unknown name {n.id}")
        if isinstance(n, ast.BinOp) and type(n.op) in ops:
            return ops[type(n.op)](ev(n.left), ev(n.right))
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.USub):
            return -ev(n.operand)
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, ast.Invert):
            return ~ev(n.operand)
        raise ValueError(f"cannot evaluate {ast.dump(n)}")

    return ev(ast.parse(expr, mode="eval"))


def read_hpp(path):
    """Every constant of a6xx.hpp: name, value, its line, the header line its comment names, its marker."""
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().split("\n")
    except OSError as e:
        sys.exit(f"cannot read {path}: {e}")
    items = []
    env = {}
    in_enum = False
    for i, line in enumerate(lines, 1):
        code, _, comment = line.partition("//")
        marker = re.search(r"not-in-headers:\s*(.*)", comment)
        src = re.search(r"(\w+\.xml\.h):(\d+)", comment)
        m = re.match(r"\s*(?:inline\s+)?constexpr\s+\w+\s+(\w+)\s*=\s*(.+?)\s*;\s*$", code)
        if not m:
            if re.match(r"\s*enum\s+\w+(\s*:\s*\w+)?\s*\{", code):
                in_enum = True
                continue
            if in_enum:
                if code.strip().startswith("}"):
                    in_enum = False
                    continue
                m = re.match(r"\s*(\w+)\s*=\s*(.+?)\s*,?\s*$", code)
            if not m:
                continue
        name, expr = m.group(1), m.group(2)
        try:
            value = evaluate(expr, env)
        except (ValueError, SyntaxError) as e:
            sys.exit(f"{path}:{i}: cannot evaluate `{expr}` for {name}: {e}")
        env[name] = value
        items.append({
            "name": name,
            "value": value,
            "line": i,
            "src": (src.group(1), int(src.group(2))) if src else None,
            "marker": marker.group(1).strip() if marker else None,
        })
    return items


def main(argv):
    gen_dir = argv[1] if len(argv) > 1 else os.environ.get("HAKIOS_MESA_GEN")
    if not gen_dir:
        sys.exit(__doc__.strip())
    here = os.path.dirname(os.path.abspath(__file__))
    hpp = argv[2] if len(argv) > 2 else os.path.join(here, "..", "include", "hakios-metal", "a6xx.hpp")

    defines, enums, scales, muls, arrays, text = read_headers(gen_dir)
    items = read_hpp(hpp)
    if not items:
        sys.exit(f"{hpp}: no constants found; the parser or the file is wrong")

    checked_values = 0
    checked_scales = 0
    checked_muls = 0
    checked_arrays = 0
    unchecked = []
    mismatches = []
    missing = []
    uncommented = []
    drift = []

    for it in items:
        name, value = it["name"], it["value"]
        if it["marker"] is not None:
            unchecked.append(it)
            continue
        if it["src"] is None:
            uncommented.append(it)
        if name.endswith("__SCALE"):
            base = name[: -len("__SCALE")]
            if base not in scales:
                missing.append((it, f"no encoder {base}(val) in the headers"))
                continue
            expected, f, l = scales[base]
            if expected != value:
                mismatches.append((it, expected, f, l))
                continue
            checked_scales += 1
            lookup = base
        elif name.endswith("__MUL"):
            base = name[: -len("__MUL")]
            if base not in muls:
                missing.append((it, f"no encoder {base}(val) in the headers"))
                continue
            expected, f, l = muls[base]
            if expected != value:
                mismatches.append((it, expected, f, l))
                continue
            checked_muls += 1
            lookup = base
        elif name.endswith("__STRIDE"):
            base = name[: -len("__STRIDE")]
            if base not in arrays:
                missing.append((it, f"no register array {base}(i0) in the headers"))
                continue
            _, stride, f, l = arrays[base]
            if stride != value:
                mismatches.append((it, stride, f, l))
                continue
            checked_arrays += 1
            lookup = base
        else:
            found = defines.get(name, []) + enums.get(name, [])
            if name in arrays:
                b, _, f, l = arrays[name]
                found = found + [(b, f, l)]
            if not found:
                missing.append((it, "not a #define, an enumerator or a register array in the headers"))
                continue
            if all(v != value for v, _, _ in found):
                v, f, l = found[0]
                mismatches.append((it, v, f, l))
                continue
            checked_values += 1
            lookup = name
        if it["src"] is not None:
            f, l = it["src"]
            lines = text.get(f)
            if lines is None or not (1 <= l <= len(lines)) or not re.search(r"\b%s\b" % re.escape(lookup), lines[l - 1]):
                drift.append((it, lookup))

    print(f"checked {checked_values + checked_scales + checked_muls + checked_arrays} constants of {os.path.relpath(hpp)} "
          f"against {gen_dir}: {checked_values} values, {checked_scales} encoder shifts, {checked_muls} encoder "
          f"multipliers, {checked_arrays} array strides")
    if unchecked:
        print(f"not checkable, and why ({len(unchecked)}):")
        for it in unchecked:
            print(f"  {it['name']} = {it['value']:#x} -- {it['marker']}")
    if drift:
        print(f"warning: {len(drift)} line comments do not point at the constant's line in the header:")
        for it, lookup in drift:
            print(f"  a6xx.hpp:{it['line']} {it['name']} says {it['src'][0]}:{it['src'][1]}")

    failed = False
    if mismatches:
        failed = True
        it, v, f, l = mismatches[0]
        print(f"MISMATCH: {it['name']} is {it['value']:#x} in a6xx.hpp:{it['line']} but {v:#x} in {f}:{l}"
              + (f" ({len(mismatches)} mismatches in all)" if len(mismatches) > 1 else ""))
    for it, why in missing:
        failed = True
        print(f"NOT FOUND: {it['name']} (a6xx.hpp:{it['line']}): {why}")
    for it in uncommented:
        failed = True
        print(f"NO SOURCE: {it['name']} (a6xx.hpp:{it['line']}) has no `file.xml.h:line` comment")
    print("FAIL" if failed else "ok")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
