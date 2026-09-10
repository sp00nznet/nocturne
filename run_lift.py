#!/usr/bin/env python3
"""
Nocturne Static Recompilation - Phase 3 lift driver.

Adapted from the Fury3/Hellbender driver, which is the proven pcrecomp linear-lift
pipeline: seed function entries AND exact end addresses from IDA's catalog
(analysis/ida_funcs.json), so each function is disassembled within its true
[start, end) bounds instead of a next-entry guess. Lifts every IDA function via
the shared lift32 Lifter and emits chunked C + a dispatch table for indirect calls.

Two things differ from the MSVC targets, both because Nocturne is a Watcom build:

  * The code section is named `AUTO`, not `.text` (Watcom's segment naming).
  * Every section header has VirtualSize = 0, with the real size in
    SizeOfRawData -- so the slice below goes through Section.effective_size
    rather than virtual_size, which would be an empty code image.

Usage: py -3 run_lift.py [analysis/nocturne.exe] [src/recomp/gen]
"""
import sys, os, json, time, re, shutil

_here = os.path.dirname(os.path.abspath(__file__))
_pcroot = os.path.join(_here, '..', 'tools')
_pc = os.path.join(_pcroot, 'tools')
sys.path.insert(0, os.path.join(_pc, 'pe'))
sys.path.insert(0, os.path.join(_pc, 'lift'))

from pe_analyze import analyze_pe, build_iat_map
from lift32 import Lifter, FUNCTION_LOCALS
from recover import recover_functions
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM

COND_JUMPS = {'je','jne','jz','jnz','ja','jae','jb','jbe','jg','jge','jl','jle',
              'js','jns','jo','jno','jp','jnp','jcxz','jecxz'}

# CRT functions we replace with hand-written host shims (src/runtime/shims_impl.c)
# instead of lifting. Their gnarly state-machine internals (e.g. _output's format
# parser) are well-understood library code -- shimming to the host CRT is more
# robust than debugging the lift. run_lift emits an extern decl; the body is
# hand-written and the dispatch table still points at it.
# HOST_SHIM: VAs whose bodies are NOT lifted -- the dispatch points at a hand-written
# body in shims_impl.c instead. EMPTY for the first Nocturne lift: we lift everything
# once to find out what actually breaks, rather than pre-shimming on a guess. IDA's
# FLIRT pass identifies 673 library functions in this binary (the Watcom CRT), and
# those are the candidates -- Watcom's CRT is also the one place a register calling
# convention can still appear, since the app body is cdecl. Populate as bring-up
# finds them; on the MSVC targets the set was the printf/stdio family, the heap
# (malloc/free/calloc/realloc), the FDIV test, and the __CI* math intrinsics.
HOST_SHIM = set()

# FORCE_RECOVER: extra entries that overlap an existing body and must be lifted
# anyway. The common case -- a direct `call` to a mid-body address, i.e. an
# alternate entry point IDA merged away -- is now detected automatically in
# pcrecomp's recover.py, so this stays EMPTY. Add a VA here only for one the
# scan cannot see, e.g. a target reached solely through a computed jump.
FORCE_RECOVER = set()
FPU_CMP = {'EQ':'==','NE':'!=','B':'<','BE':'<=','A':'>','AE':'>=',
           'L':'<','LE':'<=','G':'>','GE':'>='}


class LinearInstruction:
    __slots__ = ['address','size','mnemonic','op_str','bytes','operands',
                 'is_call','is_ret','is_cond_jump','is_uncond_jump','is_jump']
    def __init__(self, insn):
        self.address=insn.address; self.size=insn.size
        self.mnemonic=insn.mnemonic; self.op_str=insn.op_str
        self.bytes=bytes(insn.bytes)
        self.operands=list(insn.operands) if insn.operands else []
        self.is_call=insn.mnemonic=='call'
        self.is_ret=insn.mnemonic in ('ret','retn','retf')
        self.is_cond_jump=insn.mnemonic in COND_JUMPS
        self.is_uncond_jump=insn.mnemonic=='jmp'
        self.is_jump=self.is_cond_jump or self.is_uncond_jump
    @property
    def end_address(self): return self.address+self.size
    def get_branch_target(self):
        if self.operands and self.operands[0].type==X86_OP_IMM:
            return self.operands[0].imm & 0xFFFFFFFF
        return None


def linear_disassemble_function(md, code_data, code_start, func_start, func_end):
    offset=func_start-code_start; size=func_end-func_start
    if offset<0 or offset+size>len(code_data): return [], set()
    raw=code_data[offset:offset+size]; instructions=[]; leaders={func_start}
    for insn in md.disasm(raw, func_start):
        li=LinearInstruction(insn); instructions.append(li)
        if li.is_jump:
            t=li.get_branch_target()
            if t and func_start<=t<func_end: leaders.add(t)
            leaders.add(li.end_address)
        if li.mnemonic=='int3': break
    return instructions, leaders


def lift_function_linear(lifter, name, instructions, leaders):
    va = name[4:] if name.startswith('sub_') else '0'
    # Per-function scratch comes from lift32.FUNCTION_LOCALS -- the lifter is the
    # only thing that knows which locals its emitted statements reference, so it
    # owns the list. (_st/_fp_top/_fpu_cw are GLOBAL: the x87 stack is shared
    # across calls -- see recomp_types.h.)
    lines=[f'void {name}(void) {{']
    lines += [f'    {decl}' for decl in FUNCTION_LOCALS]
    lines += [f'    RECOMP_ENTER(0x{va}u);','']
    lifter._flag_state=None
    # If the function has an indirect jump (jump table / switch), a computed
    # target can land on ANY instruction in it -- so label every instruction so
    # the local dispatch below can reach it. Otherwise just label block leaders.
    has_indirect = any(i.is_uncond_jump and i.get_branch_target() is None
                       for i in instructions)
    for insn in instructions:
        if has_indirect or insn.address in leaders:
            lines.append(f'L_{insn.address:08X}:')
        for line in lifter.lift_instruction(insn): lines.append(f'    {line}')
    if instructions and not instructions[-1].is_ret:
        lines.append('    return; /* end of function */')

    # Intra-function indirect jumps (jump tables / switch statements) lift to
    # RECOMP_ITAIL(<runtime expr>), but the dispatch table only knows function
    # entries -- it can't resolve a label *inside* this function, so the switch
    # breaks. Route indirect ITAILs through a local label dispatch first: a
    # standard-C switch over every label in this function (goto), falling back to
    # the global RECOMP_ITAIL for genuine cross-function tail calls.
    indirect = [i for i,l in enumerate(lines)
                if 'RECOMP_ITAIL(' in l and 'RECOMP_ITAIL(0x' not in l]
    body = '\n'.join(lines)
    defined = sorted(set(re.findall(r'(?m)^\s*(L_[0-9A-Fa-f]{8})\s*:', body)))
    if indirect and defined:
        for i in indirect:
            m = re.search(r'RECOMP_ITAIL\((.+?)\);\s*return;', lines[i])
            if m:
                lines[i] = (f'    {{ _itail_tgt = (uint32_t)({m.group(1)}); '
                            f'goto _ljump; }}')
        lines.append('  _ljump:')
        lines.append('    switch (_itail_tgt) {')
        for lbl in defined:
            lines.append(f'      case 0x{int(lbl[2:],16):08X}u: goto {lbl};')
        lines.append('      default: RECOMP_ITAIL(_itail_tgt); return;')
        lines.append('    }')

    refed = set(re.findall(r'goto\s+(L_[0-9A-Fa-f]{8})', '\n'.join(lines)))
    for lbl in sorted(refed - set(defined)):
        lines.append(f'    {lbl}: RECOMP_ITAIL(0x{int(lbl[2:],16):08X}u); return;')
    lines.append('}')
    out = '\n'.join(lines)
    out = re.sub(r'CMP_(\w+)\(_fpu_cmp\)',
                 lambda m: f'((_fpu_cmp) {FPU_CMP.get(m.group(1), "==")} 0)', out)
    return out


def write_chunk(out, idx, funcs):
    with open(os.path.join(out, f'recomp_{idx:04d}.c'),'w') as f:
        f.write('/* Nocturne Recompilation - Auto-generated - DO NOT EDIT */\n')
        f.write(f'/* File {idx}: {len(funcs)} functions */\n\n')
        f.write('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n')
        f.write('#include "recomp_funcs.h"\n#include <math.h>\n#include <string.h>\n\n')
        for code,_,_ in funcs: f.write(code+'\n\n')




def main():
    exe = sys.argv[1] if len(sys.argv)>1 else os.path.join(_here,'analysis','nocturne.exe')
    out = sys.argv[2] if len(sys.argv)>2 else os.path.join(_here,'src','recomp','gen')
    split = int(sys.argv[3]) if len(sys.argv)>3 else 400
    os.makedirs(out, exist_ok=True)

    # The lifted code and recomp_types.h are two halves of one contract -- the
    # lifter emits _mm[n]/_flag_k and the header has to declare them. Copying the
    # header from the same pcrecomp checkout the Lifter was imported from, on
    # every run, keeps the halves in step. A hand-copied header is how a stale
    # one goes unnoticed until 200 MMX bodies fail to compile.
    shutil.copy(os.path.join(_pcroot, 'runtime', 'recomp32', 'recomp_types.h'),
                os.path.join(out, 'recomp_types.h'))

    print('=== Nocturne Static Recompilation (Phase 3 lift) ===', flush=True)
    info=analyze_pe(exe); iat=build_iat_map(info)
    print(f'[*] base=0x{info.image_base:08X} code=0x{info.code_start:08X}-0x{info.code_end:08X} IAT={len(iat)}')
    pe_data=open(exe,'rb').read()
    # Watcom names the code section AUTO and leaves VirtualSize 0; effective_size
    # falls back to SizeOfRawData so this slice is the whole code image, not empty.
    text=[s for s in info.sections if s.is_code][0]
    code_data=pe_data[text.raw_offset: text.raw_offset+min(text.effective_size,text.raw_size)]
    cs, ce = info.code_start, info.code_end

    # Seed entries + exact end bounds from IDA's catalog.
    cat=json.load(open(os.path.join(_here,'analysis','ida_funcs.json'),encoding='utf-8'))
    fns=[(f['ea'], f['end']) for f in cat['functions'] if cs <= f['ea'] < ce]
    fns.sort()
    print(f'[*] seeded {len(fns)} functions from IDA catalog (with exact bounds)')

    # IDA's catalog misses two kinds of entry point: functions reached only via
    # jmp-thunk chains / tail calls / stored function pointers, and alternate
    # entry points inside a body it merged. The first stall at runtime with
    # `ITAIL: unresolved VA ...`; the second break the *build*, since the lifter
    # emits a call to a function nobody defines. pcrecomp's recover module finds
    # both -- see tools/lift/recover.py.
    missing = recover_functions(code_data, cs, ce, fns, forced=FORCE_RECOVER)
    if missing:
        fns.extend(missing); fns.sort()
        print(f'[*] recovered {len(missing)} functions IDA missed (thunks, tail calls, alt entries): '
              + ', '.join(f'0x{a:08X}' for a,_ in missing[:8]) + (' ...' if len(missing)>8 else ''))

    md=Cs(CS_ARCH_X86, CS_MODE_32); md.detail=True
    lifter=Lifter(iat_map=iat)
    all_entries=[]; errors=0; idx=0; chunk=[]; t0=time.time()
    for addr, end in fns:
        end = min(end, ce)
        if end-addr < 1: continue
        name=f'sub_{addr:08X}'
        if addr in HOST_SHIM:
            # body provided by hand in shims_impl.c; just declare + register it
            chunk.append((f'/* {name}: host-shimmed in shims_impl.c */\n'
                          f'extern void {name}(void);\n', addr, name))
            all_entries.append((addr,name)); continue
        try:
            insns,leaders=linear_disassemble_function(md, code_data, cs, addr, end)
            if not insns:
                chunk.append((f'void {name}(void) {{ }} /* empty */\n',addr,name))
                all_entries.append((addr,name)); continue
            code=lift_function_linear(lifter, name, insns, leaders)
            chunk.append((code,addr,name)); all_entries.append((addr,name))
        except Exception as e:
            chunk.append((f'/* ERROR {name}: {e} */\nvoid {name}(void) {{}}\n',addr,name))
            all_entries.append((addr,name)); errors+=1
        if len(chunk)>=split:
            write_chunk(out, idx, chunk); idx+=1; chunk=[]
            print(f'[*]   {len(all_entries)}/{len(fns)} ({errors} err)', flush=True)
    if chunk: write_chunk(out, idx, chunk); idx+=1

    with open(os.path.join(out,'recomp_funcs.h'),'w') as f:
        f.write('/* Nocturne Recompilation - Auto-generated */\n#pragma once\n#include <stdint.h>\n\n')
        for a,n in all_entries: f.write(f'void {n}(void);  /* 0x{a:08X} */\n')
    with open(os.path.join(out,'recomp_dispatch.c'),'w') as f:
        f.write('/* Nocturne Recompilation - Auto-generated */\n#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n')
        f.write('const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a,n in sorted(all_entries): f.write(f'    {{ 0x{a:08X}u, {n} }},\n')
        f.write('};\n\n'+f'const uint32_t recomp_dispatch_count = {len(all_entries)};\n')

    total_lines=total_bytes=0
    for fn in os.listdir(out):
        fp=os.path.join(out,fn)
        if os.path.isfile(fp):
            total_bytes+=os.path.getsize(fp)
            total_lines+=sum(1 for _ in open(fp, encoding='utf-8', errors='replace'))
    json.dump({'functions':len(all_entries),'errors':errors,'files':idx,
               'lines':total_lines,'bytes':total_bytes},
              open(os.path.join(_here,'analysis','phase3_codegen.json'),'w'), indent=1)
    print('='*56)
    print(f'  functions: {len(all_entries):,}  errors: {errors}  files: {idx}')
    print(f'  lines of C: {total_lines:,}  size: {total_bytes/1048576:.1f} MB  time: {time.time()-t0:.1f}s')
    print('='*56)


if __name__=='__main__':
    main()
