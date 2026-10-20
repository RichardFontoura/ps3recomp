import re

MARK = '#include "spu_ibf.h"'
SITE = re.compile(r'ctx->pc = ctx->gpr\[(\d+)\]\._u32\[0\]; g_spu_trampoline_fn = spu_indirect_branch; return;')
RET = re.compile(r'\bSPU_RET\(ctx\);')
DIRECT_BRACKET = re.compile(r'ctx->host_depth\+\+; (\w+\(ctx\);) spu_drain_call\(ctx, ([^;]*?)\); ctx->host_depth--;')
INDIRECT_BRACKET = re.compile(r'ctx->host_depth\+\+; ctx->drain_ret_pc = ([^;]*?); spu_indirect_branch\(ctx\); '
                              r'spu_drain_call\(ctx, ([^;]*?)\); ctx->host_depth--;')
HELPERS_VEC = re.compile(r'^#include "spu_helpers_vec\.h".*$', re.M)


def function_head(prefix):
    return re.compile(r'^(?:static\s+)?void\s+(' + re.escape(prefix) + r'_rc_spu_func_([0-9A-Fa-f]{8}))'
                      r'\(spu_context\*\s*ctx\)\s*\{\s*$')


def declarations_end(body):
    d = 0
    while d < len(body) and (body[d].startswith('\tuint8_t* spu_rc_ls') or body[d].startswith('\tu128 ')):
        d += 1
    return d


def rewrite_brackets(text):
    text = DIRECT_BRACKET.sub(lambda m: 'ctx->host_depth++; spu_ibf_push(ctx, %s); %s spu_drain_call(ctx, %s); '
                              'spu_ibf_pop(ctx); ctx->host_depth--;' % (m.group(2), m.group(1), m.group(2)), text)
    return INDIRECT_BRACKET.sub(lambda m: 'ctx->host_depth++; spu_ibf_push(ctx, %s); ctx->drain_ret_pc = %s; '
                                'SPU_IBF_CALL(ctx); spu_drain_call(ctx, %s); spu_ibf_pop(ctx); ctx->host_depth--;'
                                % (m.group(2), m.group(1), m.group(2)), text)


def transform(src):
    if MARK in src:
        return src, (0, 0, 0)
    m = re.search(r'^void (\w+?)_rc_spu_func_', src, re.M)
    if not m:
        return src, (0, 0, 0)
    prefix = m.group(1)
    loop_heads = {int(x, 16) for x in re.findall(r'spu_ila\(0x([0-9A-Fa-f]+)\)', src)}
    head = function_head(prefix)
    lines = src.split('\n')
    out = []
    sites = retself = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        hm = head.match(line)
        if not hm:
            out.append(line)
            i += 1
            continue
        name = hm.group(1)
        entry = int(hm.group(2), 16)
        j = i + 1
        while lines[j] != '}':
            j += 1
        body = lines[i + 1:j]
        use_retself = entry in loop_heads and any(RET.search(b) for b in body)
        if not any(SITE.search(b) for b in body) and not use_retself:
            out.append(line)
            out.extend(body)
            i = j
            continue
        d = declarations_end(body)
        rewritten = body[:d] + ['spu_ibf_top: ;']
        for b in body[d:]:
            b, n = SITE.subn(lambda mm: 'SPU_IBF_SITE(ctx, %s, %s) %s' % (mm.group(1), name, mm.group(0)), b)
            sites += n
            if use_retself:
                b, n = RET.subn('SPU_IBF_RETSELF(ctx, 0x%Xu, %s) SPU_RET(ctx);' % (entry, name), b)
                retself += n
            rewritten.append(b)
        out.append(line)
        out.extend(rewritten)
        i = j
    text = '\n'.join(out)
    brackets = len(DIRECT_BRACKET.findall(text)) + len(INDIRECT_BRACKET.findall(text))
    text = rewrite_brackets(text)
    inc = HELPERS_VEC.search(text)
    if not inc:
        raise ValueError('no spu_helpers_vec.h include')
    text = text[:inc.end()] + '\n' + MARK + text[inc.end():]
    return text, (sites, retself, brackets)
