#!/usr/bin/env python3
"""Count AVR cycles of the E pulse in a compiled RPU_DataWrite / RPU_DataRead (fix 0013).

The host tests use a cycle MODEL.  This script measures the real compiled code: it disassembles
the two functions with avr-objdump and adds up the cycles between the instruction that raises E
(sbi PORTG,2) and the one that drops it (cbi PORTG,2), or the PINA sample for a read.

  check_pulse_cycles.py [FILE] [--src DIR]   FILE = ELF or object from avr-gcc.  Without FILE an object is built
                                     from RPU.cpp with avr-g++ and the Arduino core (if installed).

Only the 6802-mode path is measured (the first `sbi PORTG,2` ... `cbi PORTG,2` in the function).

Limits: avr-gcc 7.3 for atmega2560 (3-byte return addresses), no interrupts counted, instructions
not in the table below make the script stop and say so rather than guess.  This is a count of
the code as compiled, not a scope measurement: the MCU clock is assumed to be exactly 16 MHz.
"""
import argparse, glob, os, re, subprocess, sys, tempfile

NS_PER_CYCLE = 62.5
E_HIGH_MIN_NS = 450      # MC6821 minimum E pulse width high
TDDR_MAX_NS = 290        # MC6821 data delay: read data valid up to this long after E rises
PORTG_IO, PORTG_E_BIT = 0x14, 2
PINA_IO = 0x00

CYCLES = {
    'nop': 1, 'sbi': 2, 'cbi': 2, 'in': 1, 'out': 1, 'lds': 2, 'sts': 2, 'ldi': 1, 'mov': 1, 'movw': 1,
    'and': 1, 'andi': 1, 'or': 1, 'ori': 1, 'eor': 1, 'com': 1, 'swap': 1, 'lsr': 1, 'asr': 1, 'lsl': 1,
    'add': 1, 'adc': 1, 'sub': 1, 'subi': 1, 'sbc': 1, 'sbci': 1, 'inc': 1, 'dec': 1, 'clr': 1, 'ser': 1,
    'rjmp': 2, 'push': 2, 'pop': 2, 'cp': 1, 'cpi': 1, 'cpc': 1, 'tst': 1, 'ld': 2, 'st': 2,
}


def find_tool(avr_tools, name):
    p = os.path.join(avr_tools, name)
    return p if os.path.exists(p) else name


def build_object(src, avr_tools, name):
    cores = glob.glob(os.path.expanduser('~/Library/Arduino15/packages/arduino/hardware/avr/*'))
    if not cores:
        sys.exit('no Arduino AVR core found; pass an ELF/object built with avr-gcc as FILE')
    core = sorted(cores)[-1]
    outdir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build')
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, name)
    cmd = [find_tool(avr_tools, 'avr-g++'), '-c', '-Os', '-mmcu=atmega2560', '-DF_CPU=16000000L', '-DARDUINO=10819',
           '-DARDUINO_AVR_MEGA2560', '-DARDUINO_ARCH_AVR', '-w',
           '-I', os.path.join(core, 'cores/arduino'), '-I', os.path.join(core, 'variants/mega'),
           '-I', os.path.join(core, 'libraries/EEPROM/src'), '-I', src, os.path.join(src, 'RPU.cpp'), '-o', out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('avr-g++ failed:\n' + r.stderr[-2000:])
    return out


def disassemble(avr_tools, path, func):
    """{address: (op, [operands], target)} for one function; target is the jump destination of an
    rjmp (from the relocation in an object file, or from the address comment in a linked ELF)."""
    r = subprocess.run([find_tool(avr_tools, 'avr-objdump'), '-dr', '-z', '--no-show-raw-insn', path],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('avr-objdump failed: ' + r.stderr)
    code, on, last = {}, False, None
    for line in r.stdout.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            on = m.group(1) == func
            continue
        if not on:
            continue
        m = re.match(r'^\s*([0-9a-f]+):\s+(\S+)\s*([^;]*?)\s*(?:;\s*(.*))?$', line)
        if m and not line.strip().split(':')[1].strip().startswith('R_AVR'):
            addr, op, args, comment = int(m.group(1), 16), m.group(2), m.group(3), m.group(4) or ''
            ops = []     # numeric operands only (register names such as r24 are skipped)
            for x in args.split(','):
                try:
                    ops.append(int(x.strip(), 0))
                except ValueError:
                    pass
            target = None
            cm = re.match(r'0x([0-9a-f]+)', comment)
            if op == 'rjmp' and cm:
                target = int(cm.group(1), 16)
            code[addr] = [op, ops, target]
            last = addr
            continue
        m = re.match(r'^\s*([0-9a-f]+):\s+R_AVR_13_PCREL\s+\S+\+0x([0-9a-f]+)', line)
        if m and int(m.group(1), 16) in code:
            code[int(m.group(1), 16)][2] = int(m.group(2), 16)     # object file: the relocation names the target
    return code


def nxt_of(code, pc):
    keys = sorted(code)
    i = keys.index(pc) + 1
    return keys[i] if i < len(keys) else None


def walk(avr_tools, path, func, label, read):
    code = disassemble(avr_tools, path, func)
    if not code:
        sys.exit('%s: function %s not found in %s' % (label, func, path))
    start = None
    for addr in sorted(code):
        op, ops, _ = code[addr]
        if op == 'sbi' and ops[:2] == [PORTG_IO, PORTG_E_BIT]:
            start = addr
            break
    if start is None:
        sys.exit('%s: no sbi PORTG,2 (E high) found' % label)
    cyc, pc, steps = 0, sorted(a for a in code if a > start)[0], 0
    while steps < 64:
        steps += 1
        if pc not in code:
            sys.exit('%s: lost the instruction stream at 0x%x' % (label, pc))
        op, ops, target = code[pc]
        if read and op == 'sbi' and ops[:2] == [PORTG_IO, PORTG_E_BIT]:
            cyc = 0                                      # a later E rise restarts the count (upstream raises E twice)
            pc = nxt_of(code, pc)
            continue
        elif read and op == 'in' and ops[:1] == [PINA_IO]:
            return cyc                                   # PINA is read in the first cycle of this `in`
        if op not in CYCLES:
            sys.exit('%s: instruction %r between E rise and the %s is not in the cycle table' % (label, op, 'sample' if read else 'fall'))
        cyc += CYCLES[op]
        if not read and op == 'cbi' and ops[:2] == [PORTG_IO, PORTG_E_BIT]:
            return cyc
        keys = sorted(code)
        nxt = keys[keys.index(pc) + 1] if keys.index(pc) + 1 < len(keys) else None
        pc = target if (op == 'rjmp' and target is not None) else nxt
        if pc is None:
            break
    sys.exit('%s: did not reach the %s after the E rise' % (label, 'PINA sample' if read else 'E fall'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', nargs='?')
    ap.add_argument('--repo', required=True)
    ap.add_argument('--src', help='directory holding RPU.cpp to build (default: the repository)')
    ap.add_argument('--avr-tools', required=True)
    a = ap.parse_args()
    path = a.file or build_object(a.src or a.repo, a.avr_tools, 'RPU_avr_upstream.o' if a.src else 'RPU_avr.o')
    ok = True
    hi = walk(a.avr_tools, path, '_Z13RPU_DataWriteih', 'RPU_DataWrite', False)
    ns = hi * NS_PER_CYCLE
    print('RPU_DataWrite: E high for %d cycles = %.1f ns (needs >= %d ns): %s' % (hi, ns, E_HIGH_MIN_NS, 'ok' if ns >= E_HIGH_MIN_NS else 'TOO SHORT'))
    ok &= ns >= E_HIGH_MIN_NS
    sample = walk(a.avr_tools, path, '_Z12RPU_DataReadi', 'RPU_DataRead', True)
    sns = sample * NS_PER_CYCLE
    print('RPU_DataRead:  PINA sampled %d cycles = %.1f ns after E rises (needs >= %d ns; E falls after the sample): %s'
          % (sample, sns, TDDR_MAX_NS, 'ok' if sns >= TDDR_MAX_NS else 'TOO EARLY'))
    ok &= sns >= TDDR_MAX_NS
    sys.exit(0 if ok else 1)


main()
