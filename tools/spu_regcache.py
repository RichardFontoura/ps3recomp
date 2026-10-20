#!/usr/bin/env python3
import argparse
import os
import re
import sys

GPR = re.compile(r'ctx->gpr\[(\d+)\]')
FHEAD = re.compile(r'^(?:static\s+)?void\s+\w+\(spu_context\*\s*ctx\)\s*\{\s*$')
FNAME = re.compile(r'^(?:static\s+)?void\s+(\w+)\(spu_context\*\s*ctx\)\s*\{\s*$', re.M)
ASSIGN = re.compile(r'ctx->gpr\[(\d+)\]\s*=(?!=)')
NORET = re.compile(r'\breturn\b|\bSPU_RET\b|\bSPU_RET_REG\b|\bSPU_FAST_XFER\b|\bSPU_TAILCALL\b')
LABEL = re.compile(r'^\s*(loc_[0-9A-Fa-f]+):\s*$')
GOTO = re.compile(r'^\s*(?:if \((.*)\) )?goto (loc_[0-9A-Fa-f]+);\s*$')
HELPERS_INCLUDE = re.compile(r'^(#include "[./]*(?:runtime/spu/)?spu_helpers\.h"[^\n]*)$', re.M)
LS_DECL = '\tuint8_t* spu_rc_ls __attribute__((unused)) = ctx->ls;'
LS_ACCESS = re.compile(r'\bspu_ls_(?:read|write)128\(')
OUTPUT = 'spu_recomp_rc.c'
SKIP_LABEL_BASE = 0xC0DE0000
PIC_CTOR = re.compile(r'^__attribute__\(\(constructor\)\) static void \w+\(void\)\n\{\n[^{}]*spu_pic_register\([^{}]*\n\}\n', re.M)


def is_pure(line):
    rest = GPR.sub('', line).replace('spu_ls_read128(ctx,', '').replace('spu_ls_write128(ctx,', '')
    return not re.search(r'\bctx\b|\breturn\b|\bextern\b|g_spu_|\bSPU_|^\s*#', rest)


def split_if(line):
    stripped = line.lstrip()
    if not stripped.startswith('if ('):
        return None
    indent = line[:len(line) - len(stripped)]
    depth = 0
    for j in range(3, len(stripped)):
        if stripped[j] == '(':
            depth += 1
        elif stripped[j] == ')':
            depth -= 1
            if depth == 0:
                break
    else:
        return None
    cond = stripped[4:j]
    rest = stripped[j + 1:].rstrip()
    if not (rest.startswith(' {') and rest.endswith('}')):
        return None
    body = rest[2:-1]
    depth = 0
    for ch in body:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth < 0:
                return None
    if depth != 0:
        return None
    return indent, cond, body


def to_locals(line):
    return GPR.sub(lambda m: 'r' + m.group(1), line)


def leaves_function(line):
    return not re.search(r'\bif\b', line) and NORET.search(line)


def uses_defs(text):
    uses = {int(x) for x in GPR.findall(text)}
    defs = {int(x) for x in ASSIGN.findall(text)}
    if len(defs) == 1 and text.count(';') == 1:
        m = ASSIGN.search(text)
        uses = {int(x) for x in GPR.findall(text[:m.start()])} | {int(x) for x in GPR.findall(text[m.end():])}
        return uses, defs
    return uses, set()


def split_statements(body):
    statements, depth, start = [], 0, 0
    for i, ch in enumerate(body):
        if ch in '({':
            depth += 1
        elif ch in ')}':
            depth -= 1
        elif ch == ';' and depth == 0:
            statements.append(body[start:i].strip())
            start = i + 1
    if body[start:].strip():
        statements.append(body[start:].strip())
    return statements


def lower_branchy_ifs(body):
    out = []
    serial = 0
    for line in body:
        parts = split_if(line)
        if not (parts and is_pure('if (' + parts[1] + ')') and not is_pure(line) and re.search(r'\bgoto\b', parts[2])):
            out.append(line)
            continue
        indent, cond, branch = parts
        label = 'loc_%08X' % (SKIP_LABEL_BASE + serial)
        serial += 1
        out.append('%sif (!(%s)) goto %s;' % (indent, cond, label))
        pending = []
        for statement in split_statements(branch):
            if re.search(r'\bgoto\b', statement):
                if pending:
                    out.append(indent + '; '.join(pending) + ';')
                    pending = []
                out.append(indent + statement + ';')
            else:
                pending.append(statement)
        if pending:
            out.append(indent + '; '.join(pending) + ';')
        out.append(label + ':')
        out.append(indent + ';')
    return out


def classify(body):
    kinds = []
    for line in lower_branchy_ifs(body):
        if not line.strip() or is_pure(line):
            m = GOTO.match(line)
            kinds.append(('g', (line, m.group(2), m.group(1) is not None)) if m else ('p', line))
            continue
        if re.search(r'\bgoto\b', line):
            raise ValueError('impure goto: ' + line)
        parts = split_if(line)
        if parts and is_pure('if (' + parts[1] + ')'):
            kinds.append(('if', parts))
            continue
        kinds.append(('x', line))
    return kinds


def successors(kinds):
    labels = {}
    for i, (kind, value) in enumerate(kinds):
        if kind == 'p':
            m = LABEL.match(value)
            if m:
                labels[m.group(1)] = i
    succ = []
    n = len(kinds)
    for i, (kind, value) in enumerate(kinds):
        if kind == 'g':
            if value[1] not in labels:
                raise ValueError('goto to unknown label ' + value[1])
            succ.append(([i + 1] if value[2] else []) + [labels[value[1]]])
        elif kind == 'x' and leaves_function(value):
            succ.append([])
        else:
            succ.append([i + 1] if i + 1 <= n else [])
    return succ


def may_dirty(kinds, succ):
    n = len(kinds)
    dirty_in = [None] * (n + 1)
    dirty_in[0] = frozenset()
    work = [0]
    while work:
        i = work.pop()
        if i >= n:
            continue
        kind, value = kinds[i]
        if kind == 'p':
            out = dirty_in[i] | frozenset(int(x) for x in ASSIGN.findall(value))
        elif kind == 'x':
            out = frozenset()
        else:
            out = dirty_in[i]
        for j in succ[i]:
            if dirty_in[j] is None:
                dirty_in[j] = out
                work.append(j)
            elif not out <= dirty_in[j]:
                dirty_in[j] = dirty_in[j] | out
                work.append(j)
    return dirty_in


def live_locals(kinds, succ, dirty_in):
    n = len(kinds)

    def spilled(i):
        return set(dirty_in[i]) if dirty_in[i] is not None else set()

    live_in = [set() for _ in range(n + 1)]
    live_in[n] = spilled(n)
    ud = [uses_defs(v if k == 'p' else v[0]) if k in ('p', 'g') else None for k, v in kinds]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            kind, value = kinds[i]
            live_out = set()
            for j in succ[i]:
                live_out |= live_in[j]
            if kind == 'p':
                uses, defs = ud[i]
                new = uses | (live_out - defs)
            elif kind == 'g':
                new = ud[i][0] | live_out
            elif kind == 'if':
                new = {int(x) for x in GPR.findall(value[1])} | spilled(i) | live_out
            else:
                new = spilled(i)
            if new != live_in[i]:
                live_in[i] = new
                changed = True
    return live_in


def transform_function(body):
    kinds = classify(body)
    used, written = set(), set()
    for kind, value in kinds:
        if kind in ('p', 'g'):
            text = value if kind == 'p' else value[0]
            used.update(int(n) for n in GPR.findall(text))
            written.update(int(n) for n in ASSIGN.findall(text))
        elif kind == 'if':
            used.update(int(n) for n in GPR.findall(value[1]))
    has_ls = any(LS_ACCESS.search(line) for line in body)
    if not used:
        if has_ls:
            return [LS_DECL] + list(body), 0
        return list(body), 0
    succ = successors(kinds)
    dirty_in = may_dirty(kinds, succ)
    live_in = live_locals(kinds, succ, dirty_in)
    all_reload = ' '.join('r%d = ctx->gpr[%d];' % (r, r) for r in sorted(used))

    def reload_after(i):
        live_out = set()
        for j in succ[i]:
            live_out |= live_in[j]
        return ' '.join('r%d = ctx->gpr[%d];' % (r, r) for r in sorted(live_out & used))

    def spill_before(i):
        return ' '.join('ctx->gpr[%d] = r%d;' % (r, r) for r in sorted(dirty_in[i] or ()))

    out = ([LS_DECL] if has_ls else []) + ['\tu128 ' + ', '.join('r%d = ctx->gpr[%d]' % (r, r) for r in sorted(used)) + ';']
    for i, (kind, value) in enumerate(kinds):
        if kind == 'p':
            out.append(to_locals(value))
        elif kind == 'g':
            out.append(to_locals(value[0]))
        elif kind == 'if':
            indent, cond, branch = value
            spill = spill_before(i)
            tail = '' if NORET.search(branch) else ' ' + all_reload + (' spu_rc_ls = ctx->ls;' if has_ls else '')
            out.append('%sif (%s) { %s%s%s }' % (indent, to_locals(cond), spill + ' ' if spill else '', branch.strip(), tail))
        else:
            indent = value[:len(value) - len(value.lstrip())]
            spill = spill_before(i)
            if spill:
                out.append(indent + spill)
            out.append(value)
            if not leaves_function(value):
                reload = reload_after(i)
                ls_reload = 'spu_rc_ls = ctx->ls;' if has_ls else ''
                if reload or ls_reload:
                    out.append(indent + ' '.join(x for x in (reload, ls_reload) if x))
    if dirty_in[len(kinds)] is not None:
        spill = ' '.join('ctx->gpr[%d] = r%d;' % (r, r) for r in sorted(dirty_in[len(kinds)]))
        if spill:
            out.append('\t' + spill)
    return out, 1


def transform_file(src):
    lines = src.split('\n')
    out = []
    i = 0
    functions = 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        i += 1
        if FHEAD.match(line):
            j = i
            while lines[j] != '}':
                j += 1
            body, n = transform_function(lines[i:j])
            functions += n
            out.extend(body)
            i = j
    return '\n'.join(out), functions


def twin(dst, src):
    names = FNAME.findall(src)
    if not names:
        return dst, 0
    m = re.match(r'(\w+)_spu_func_[0-9A-Fa-f_]+$', names[0])
    if not m:
        raise ValueError('unexpected function name ' + names[0])
    prefix = m.group(1)
    if any(not n.startswith(prefix + '_spu_') for n in names):
        raise ValueError('mixed prefixes in ' + prefix)
    statics = set(re.findall(r'^static\s+void\s+(\w+)\(spu_context\*', src, re.M))
    rename = re.compile(r'\b' + re.escape(prefix) + r'_(?!rc_)(\w+)\b')
    out = rename.sub(lambda mm: prefix + '_rc_' + mm.group(1), dst)
    out = PIC_CTOR.sub('', out)
    lines = out.split('\n')
    first = next(i for i, line in enumerate(lines) if FHEAD.match(line))
    decls = ['%svoid %s_rc_spu_%s(spu_context* ctx);' % ('static ' if n in statics else '', prefix, n[len(prefix) + 5:])
             for n in names]
    lines[first:first] = decls + ['']
    public = [n for n in names if n not in statics]
    tail = ['', 'extern void spu_rc_add_pairs(const void* const* pairs, unsigned n);',
            'static const void* const s_spu_rc_pairs[] = {']
    tail += ['    (const void*)%s, (const void*)%s_rc_spu_%s,' % (n, prefix, n[len(prefix) + 5:]) for n in public]
    tail += ['};', '__attribute__((constructor)) static void %s_rc_pairs_ctor(void)' % prefix,
             '{', '    spu_rc_add_pairs(s_spu_rc_pairs, %du);' % len(public), '}', '']
    return '\n'.join(lines + tail), len(public)


def build_image(directory):
    path = os.path.join(directory, 'spu_recomp.c')
    src = open(path, encoding='utf-8', errors='surrogateescape').read()
    dst, functions = transform_file(src)
    dst, pairs = twin(dst, src)
    dst, n = HELPERS_INCLUDE.subn(r'\1\n#include "spu_helpers_vec.h"', dst, count=1)
    if not n:
        raise ValueError('no spu_helpers.h include')
    target = os.path.join(directory, OUTPUT)
    if os.path.exists(target):
        with open(target, encoding='utf-8', errors='surrogateescape', newline='') as f:
            if f.read() == dst:
                return functions, pairs
    with open(target, 'w', encoding='utf-8', errors='surrogateescape', newline='\n') as f:
        f.write(dst)
    return functions, pairs


def main():
    parser = argparse.ArgumentParser(description='Write a register-cached twin (spu_recomp_rc.c) next to every '
                                     'lifted SPU image (spu_recomp.c) under a directory.')
    parser.add_argument('spu_dir')
    parser.add_argument('--only', action='append', default=[], help='image directory name to process (repeatable)')
    parser.add_argument('--clean', action='store_true', help='remove the twins instead of writing them')
    args = parser.parse_args()
    total = 0
    skipped = 0
    for name in sorted(os.listdir(args.spu_dir)):
        directory = os.path.join(args.spu_dir, name)
        if args.only and name not in args.only:
            continue
        if not os.path.isfile(os.path.join(directory, 'spu_recomp.c')):
            continue
        if args.clean:
            target = os.path.join(directory, OUTPUT)
            if os.path.exists(target):
                os.remove(target)
            continue
        try:
            functions, pairs = build_image(directory)
        except ValueError as exc:
            skipped += 1
            target = os.path.join(directory, OUTPUT)
            if os.path.exists(target):
                os.remove(target)
            print('%-40s skipped: %s' % (name, exc), file=sys.stderr)
            continue
        total += functions
        print('%-40s %5d functions cached, %5d pairs' % (name, functions, pairs))
    if not args.clean:
        print('total %d functions cached; %d images skipped' % (total, skipped))


if __name__ == '__main__':
    main()
