#!/usr/bin/env python3
"""Mutation check of the tests: break one fix at a time in a scratch copy of the library and
confirm that the tests notice.  The branch itself is never modified.

  run.sh mutants            (or: python3 mutants.py)

Each line names the deliberate defect and the tests that failed.  "NOT CAUGHT" means no test
depends on that behaviour (one such mutant is expected, see the comment on it).
"""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
work = tempfile.mkdtemp(prefix='s11c_mut_')
shutil.copytree(os.path.join(REPO, 'system11c', 'tests'), os.path.join(work, 'system11c', 'tests'),
                ignore=shutil.ignore_patterns('build'))
for f in ('RPU.cpp', 'RPU.h', 'RPU_Config.h', 'RPU_AlphaScan.h'):
    shutil.copy(os.path.join(REPO, f), os.path.join(work, f))
orig = open(os.path.join(work, 'RPU.cpp')).read()
uncaught = 0


def run(name, old, new):
    global uncaught
    assert orig.count(old) >= 1, name + ': pattern not found (library changed?)'
    open(os.path.join(work, 'RPU.cpp'), 'w').write(orig.replace(old, new, 1))
    r = subprocess.run([os.path.join(work, 'system11c', 'tests', 'run.sh'), 'branch'], capture_output=True, text=True,
                       env=dict(os.environ, S11C_TEST_BUILD=os.path.join(work, 'build')))
    fails = [l.split()[1] for l in r.stdout.splitlines() if re.match(r'^\S+\s+\S+\s+(FAIL|CRASH)', l)]
    if not fails:
        uncaught += 1
    print('%-42s %s' % (name, ', '.join(fails) if fails else 'NOT CAUGHT'))
    sys.stdout.flush()


run('no special handover',"if (SpecialActive != 0xFF && SpecialActive != (byte)(solenoidOn - 16)) {","if (false) {")
run('no min-off',"if (SolenoidCoolLeft != 0 && (SolenoidStack[SolenoidStackFirst] & RPU_SOLENOID_ENTRY_COIL) == SolenoidLastCoil) {","if (false) {")
run('lamp order',"""    RPU_DataWrite(PIA_LAMPS_PORT_B, 0x00);
    RPU_DataWrite(PIA_LAMPS_PORT_A, curLampByte);
    RPU_DataWrite(PIA_LAMPS_PORT_B, 0x01 << (LampStrobe));""","""    RPU_DataWrite(PIA_LAMPS_PORT_B, 0x01 << (LampStrobe));
    RPU_DataWrite(PIA_LAMPS_PORT_A, curLampByte);""")
run('no TIMSK1 mask in write',"""  byte savedTIMSK1 = TIMSK1;
  TIMSK1 = savedTIMSK1 & (byte)~_BV(OCIE1A);

  // Set data pins to output""","""  byte savedTIMSK1 = TIMSK1;

  // Set data pins to output""")
run('E wait 2 cycles',"    RPU_E_WAIT_6;\n","    RPU_E_WAIT_2;\n")
run('display PIA first',"""  RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_SWITCH_PORT_A, 0x00);
  RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x3C);

  RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0xFF);
  RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0x00);

  RPU_DataWrite(PIA_LAMPS_CONTROL_A, 0x38);""","""  RPU_InitializeDisplayPIA();
  RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_SWITCH_PORT_A, 0x00);
  RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x3C);

  RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0xFF);
  RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0x00);

  RPU_DataWrite(PIA_LAMPS_CONTROL_A, 0x38);""")
run('no sound gap',"  delayMicroseconds(RPU_S11C_SOUND_COMMAND_GAP_US);","")
run('no continuous deadline',"if (ContinuousDeadline[d] == 0) ContinuousSolenoidBits &= ~((unsigned short)1 << d);","")
run('no hold-pwm limit',"      if (HoldPwmRemaining) {\n        HoldPwmRemaining -= 1;","      if (true) {")
run('no refire gap',"AppendToSolenoidStack(RPU_SOLENOID_ENTRY_IDLE, RPU_SWITCH_SOLENOID_REFIRE_GAP_SLOTS);","")
run('GameOn polarity',"disableFlippers ? S11C_GAMEON_DISABLED_CRB : S11C_GAMEON_ENABLED_CRB","disableFlippers ? S11C_GAMEON_ENABLED_CRB : S11C_GAMEON_DISABLED_CRB")
run('EnableSolenoidStack ignores game over',"GameOverLine ? S11C_GAMEON_DISABLED_CRB : S11C_GAMEON_ENABLED_CRB","S11C_GAMEON_ENABLED_CRB")
run('special table wrong',"0x3001, /* sol 17  PIA4 CA2 */\n  0x2C01, /* sol 18  PIA3 CA2 */","0x2C01, /* sol 17  PIA4 CA2 */\n  0x3001, /* sol 18  PIA3 CA2 */")
run('sol-1 coin lockout shift',"  if (solNum >= 16) return;\n","")
run('read sampled too early (wait 2)', "    RPU_E_WAIT_8;\n", "    RPU_E_WAIT_2;\n")
run('E left high after a read', "  if (!UsesM6800Processor) PORTG &= ~0x04;", "")
run('no TIMSK1 mask in read', "  // Atomic bus transaction (see RPU_DataWrite)\n  byte savedTIMSK1 = TIMSK1;\n  TIMSK1 = savedTIMSK1 & (byte)~_BV(OCIE1A);", "  byte savedTIMSK1 = TIMSK1;")
run('busy flag ignores the active pulse', "if (SolenoidActiveLeft != 0 && (SolenoidActive & RPU_SOLENOID_ENTRY_COIL) < 8) busy = true;", "")
run('boot writes the old $2203', "RPU_DataWrite(PIA_SOLENOID_11_CONTROL_B, S11C_GAMEON_DISABLED_DDR_CRB);", "RPU_DataWrite(0x2203, 0x30);")
# SRAM sizing only: 150 entries behave the same as 32 for everything the tests exercise (fix 0006/0007 sizes).
run('solenoid stack 150 entries instead of 32', "#define SOLENOID_STACK_SIZE 32\n", "#define SOLENOID_STACK_SIZE 150\n")

shutil.rmtree(work, ignore_errors=True)
print('%d mutant(s) not caught' % uncaught)
