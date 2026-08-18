import { test } from 'node:test'
import assert from 'node:assert/strict'
import { compileWrapper, disassemble } from '../index.js'

function assertFails(src, msgPattern) {
  const r = compileWrapper(src)
  assert.equal(r.ok, false, `expected compile failure for: ${src}`)
  assert.ok(Array.isArray(r.errors) && r.errors.length > 0)
  const e = r.errors[0]
  assert.equal(typeof e.line, 'number')
  assert.equal(typeof e.col, 'number')
  assert.equal(typeof e.message, 'string')
  if (msgPattern) assert.match(e.message, msgPattern)
}

test('spec section 3 Ruuvi example compiles', () => {
  // Spec §3's example is two lines as of the amendment that made `>>`
  // bit-exact (the battery.level line was removed: it was wrong on three
  // counts at once -- the non-bit-exact shift, a missing +1600 mV Ruuvi
  // offset, and pct() being identity so the result could never pass
  // capability_encode()'s 0-100% range check). `>>`'s own bit-exactness is
  // covered separately below, not via this golden.
  const src = `wrapper "ruuvi" match manufacturer 0x0499
decode
  emit air.temperature   i16_be(payload, 1) * 0.005
  emit air.humidity      u16_be(payload, 3) * 0.0025`
  const r = compileWrapper(src)
  assert.equal(r.ok, true)
  assert.equal(r.name, 'ruuvi')
  assert.deepEqual(r.match, { kind: 1, key: 0x0499 })
  assert.equal(r.capsUsed.length, 2)
  assert.deepEqual(new Set(r.capsUsed), new Set([1, 5])) // air.temperature, air.humidity

  const asm = disassemble(r.bytecode)
  assert.match(asm, /LOAD_I16BE 1/)
  // f32 round-trip (0.005 -> 0.004999999888241291 etc.), so match loosely.
  assert.match(asm, /PUSH_CONST .*0\.00499/)
  assert.match(asm, /EMIT air\.temperature/)
  assert.match(asm, /LOAD_U16BE 3/)
  assert.match(asm, /PUSH_CONST .*0\.00249/)
  assert.match(asm, /EMIT air\.humidity/)
  assert.match(asm, /HALT$/m)
})

test('`>>` compiles to a bit-exact DIV-then-FLOOR sequence (spec §3 as amended)', () => {
  // No JS-side VM exists to execute this bytecode and check a runtime
  // number, so the browser-compiler-side proof of bit-exactness is the
  // codegen SHAPE itself: DIV by exactly 2^5=32 immediately followed by
  // FLOOR (psvm.c's 0x6C). The actual numeric claim -- 0xAC36 (44086) >> 5
  // is exactly 1377, not 1377.6875 -- is asserted on the C/VM side
  // (tests/host/test_psvm.c), which is the only place bytecode actually
  // runs.
  const r = compileWrapper(
    `wrapper "shift" match manufacturer 0x0001\ndecode\n  emit air.temperature u16_be(payload, 0) >> 5`
  )
  assert.equal(r.ok, true)
  // Strip the "NNNN: " address prefix each disasm line carries so the
  // mnemonic sequence is checked for exact, immediate adjacency (DIV
  // directly followed by FLOOR, nothing else in between).
  const mnemonics = disassemble(r.bytecode).split('\n').map((l) => l.replace(/^\d+: /, ''))
  assert.deepEqual(mnemonics, [
    'LOAD_U16BE 0',
    'PUSH_CONST 0 ; 32',
    'DIV',
    'FLOOR',
    'EMIT air.temperature',
    'HALT',
  ])
})

test('right-shift amount must be a literal, but the shifted value may be any expression', () => {
  assertFails(
    `wrapper "x" match manufacturer 0x0001\ndecode\n  emit air.temperature u8(payload, 0) >> u8(payload, 1)`,
    /right-shift/i
  )
})

test('match kinds map to wmatch_kind_t (service=0, manufacturer=1, mac_prefix=2)', () => {
  const service = compileWrapper(`wrapper "s" match service 0xFCD2\ndecode\n  emit air.temperature u8(payload, 0)`)
  assert.equal(service.ok, true)
  assert.deepEqual(service.match, { kind: 0, key: 0xFCD2 })

  const mac = compileWrapper(`wrapper "m" match mac_prefix 0xAABBCC\ndecode\n  emit air.temperature u8(payload, 0)`)
  assert.equal(mac.ok, true)
  assert.deepEqual(mac.match, { kind: 2, key: 0xAABBCC })
})

test('require clause appears before the emits in disassembly', () => {
  const src = `wrapper "test" match service 0x1234
decode
  require u8(payload, 0) == 5
  emit air.temperature u8(payload, 1)`
  const r = compileWrapper(src)
  assert.equal(r.ok, true)
  const asm = disassemble(r.bytecode)
  assert.match(asm, /REQUIRE[\s\S]*EMIT air\.temperature/)
})

test('multiple accessors and len()/bits() disassemble correctly', () => {
  const src = `wrapper "multi" match manufacturer 0x0001
decode
  require len(payload) > 10
  emit battery.level bits(payload, 0, 0, 4)
  emit signal.rssi u32_le(payload, 2)`
  const r = compileWrapper(src)
  assert.equal(r.ok, true)
  const asm = disassemble(r.bytecode)
  assert.match(asm, /PAYLOAD_LEN/)
  assert.match(asm, /LOAD_BITS 0 0 4/)
  assert.match(asm, /LOAD_U32LE 2/)
})

test('unknown match kind is a compile error', () => {
  assertFails(
    `wrapper "x" match bogus 0x01\ndecode\n  emit air.temperature u8(payload, 0)`,
    /match kind/i
  )
})

test('unknown capability name is a compile error', () => {
  assertFails(
    `wrapper "x" match service 0x01\ndecode\n  emit foo.bar u8(payload, 0)`,
    /capability/i
  )
})

test('offset literal beyond 30 is a compile error', () => {
  assertFails(
    `wrapper "x" match service 0x01\ndecode\n  emit air.temperature u8(payload, 31)`,
    /offset|range/i
  )
  // 30 is the last valid u8 offset (0-indexed, payload is at most 31 bytes).
  const ok = compileWrapper(`wrapper "x" match service 0x01\ndecode\n  emit air.temperature u8(payload, 30)`)
  assert.equal(ok.ok, true)
})

test('multi-byte accessor offset beyond range is a compile error', () => {
  assertFails(
    `wrapper "x" match service 0x01\ndecode\n  emit air.temperature u16_be(payload, 30)`,
    /offset|range/i
  )
})

test('missing decode block is a compile error', () => {
  assertFails(`wrapper "x" match service 0x01`, /decode/i)
})

test('missing wrapper name is a compile error', () => {
  assertFails(`wrapper\nmatch service 0x01\ndecode\n  emit air.temperature u8(payload, 0)`, /name|string/i)
})

test('match key out of range for its kind is a compile error', () => {
  assertFails(
    `wrapper "x" match service 0x10000\ndecode\n  emit air.temperature u8(payload, 0)`,
    /range/i
  )
})

test('require with a non-literal-shape aes_ccm_decrypt statement disassembles AES_CCM', () => {
  const src = `wrapper "enc" match manufacturer 0x0499
decode
  aes_ccm_decrypt(payload, 4, 6)
  emit air.temperature u8(payload, 4)`
  const r = compileWrapper(src)
  assert.equal(r.ok, true)
  const asm = disassemble(r.bytecode)
  assert.match(asm, /AES_CCM/)
  assert.match(asm, /AES_CCM[\s\S]*LOAD_U8 4/)
})

// PSVM_MAX_EMITS (components/psvm/include/psvm.h) is 16: the VM's emit
// buffer is a fixed array, not growable. A wrapper whose worst-case run
// pushes more EMITs than that would install cleanly and then fail *every*
// run with PSVM_ERR_LIMITS -- caught here at compile time instead. The two
// tests below pin the exact boundary (16 ok, 17 not), not just "some smaller
// number now fails".
function nEmitsWrapper(n) {
  const emits = Array.from({ length: n }, () => '  emit air.temperature u8(payload, 0)').join('\n')
  return `wrapper "x" match manufacturer 0x0001\ndecode\n${emits}`
}

test('a 16-emit wrapper compiles (at the PSVM_MAX_EMITS boundary)', () => {
  const r = compileWrapper(nEmitsWrapper(16))
  assert.equal(r.ok, true)
  const asm = disassemble(r.bytecode)
  assert.equal((asm.match(/EMIT/g) || []).length, 16)
})

test('a 17-emit wrapper is a compile error naming PSVM_MAX_EMITS and the count', () => {
  assertFails(nEmitsWrapper(17), /17.*16|16.*PSVM_MAX_EMITS/i)
  const r = compileWrapper(nEmitsWrapper(17))
  assert.match(r.errors[0].message, /17/)
  assert.match(r.errors[0].message, /16/)
  assert.match(r.errors[0].message, /PSVM_MAX_EMITS/)
})

// ---- connect block (M5a spec section 2, task 3) ----

test('connect block compiles and assigns slot offsets', () => {
  const r = compileWrapper(`wrapper "acme env" match service 0x181A
connect every 10min
  write 2A00 = 01
  read 2A6E as temp
  read 2A6F as hum
decode
  emit air.temperature  i16_le(temp, 0) * 0.01
  emit air.humidity     u16_le(hum, 0) * 0.01`)
  assert.equal(r.ok, true)
  assert.equal(r.plan.intervalS, 600)
  assert.deepEqual(r.plan.reads.map(x => [x.name, x.uuid16, x.offset]),
                   [['temp', 0x2A6E, 0], ['hum', 0x2A6F, 16]])
  assert.deepEqual(r.plan.writes, [{ uuid16: 0x2A00, data: [0x01] }])
  // the named buffer became a compile-time offset: hum reads at 16
  assert.match(disassemble(r.bytecode), /LOAD_U16LE 16/)
  // each read carries the fewest bytes its accessors need (M5a spec §4):
  // both are read by a 2-byte accessor at offset 0.
  assert.deepEqual(r.plan.reads.map(x => x.minLen), [2, 2])
  assert.match(disassemble(r.bytecode), /READ 0x2A6E -> slot 0 \(offset 0, min 2 B\)/)
})

test('min_len follows the widest accessor, not the first', () => {
  const r = compileWrapper(`wrapper "wide" match service 0x181A
connect every 10min
  read 2A6E as t
decode
  emit air.temperature  i16_le(t, 0) * 0.01
  emit air.humidity     u32_le(t, 4) * 0.01`)
  assert.equal(r.ok, true)
  // u32_le at offset 4 needs 8 bytes; i16_le at 0 needs 2. The larger wins.
  assert.deepEqual(r.plan.reads.map(x => x.minLen), [8])
})

test('a read no accessor touches still requires one byte', () => {
  const r = compileWrapper(`wrapper "unused" match service 0x181A
connect every 10min
  read 2A6E as t
  read 2A6F as unused
decode
  emit air.temperature  i16_le(t, 0) * 0.01`)
  assert.equal(r.ok, true)
  // An empty response must never count as a successful read.
  assert.deepEqual(r.plan.reads.map(x => x.minLen), [2, 1])
})

test('write after the first read is a compile error, not a silent reorder', () => {
  const r = compileWrapper(`wrapper "order" match service 0x181A
connect every 10min
  read 2A6E as t
  write 2A00 = 01
decode
  emit air.temperature  i16_le(t, 0) * 0.01`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /every write before every read/)
})

test('the same characteristic may not be read twice in one connect block', () => {
  const r = compileWrapper(`wrapper "dup" match service 0x181A
connect every 10min
  read 2A6E as a
  read 2A6E as b
decode
  emit air.temperature  i16_le(a, 0) * 0.01`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /duplicate read of UUID/)
})

test('the same characteristic may not be written twice in one connect block', () => {
  const r = compileWrapper(`wrapper "dupw" match service 0x181A
connect every 10min
  write 2A00 = 01
  write 2A00 = 02
  read 2A6E as t
decode
  emit air.temperature  i16_le(t, 0) * 0.01`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /duplicate write to UUID/)
})

test('a wrapper with no connect block is byte-identical to before', () => {
  const src = `wrapper "ruuvi" match manufacturer 0x0499
decode
  emit air.temperature   i16_be(payload, 1) * 0.005`
  const r = compileWrapper(src)
  assert.equal(r.ok, true)
  assert.equal(r.plan, null)
  assert.equal(r.bytecode[6] | (r.bytecode[7] << 8), 0)   // flags still 0
})

test('decode may not reference an undeclared buffer', () => {
  assertFails(`wrapper "x" match service 0x181A
connect every 10min
  read 2A6E as temp
decode
  emit air.humidity u16_le(hum, 0)`, /unknown buffer 'hum'/)
})

test('payload is not addressable in a connect wrapper', () => {
  assertFails(`wrapper "x" match service 0x181A
connect every 10min
  read 2A6E as temp
decode
  emit air.temperature u8(payload, 0)`, /payload/)
})

test('duplicate buffer name is rejected', () => {
  assertFails(`wrapper "x" match service 0x181A
connect every 10min
  read 2A6E as temp
  read 2A6F as temp
decode
  emit air.temperature u8(temp, 0)`, /duplicate/)
})

test('caps are enforced at compile time', () => {
  const reads = ['2A01','2A02','2A03','2A04','2A05'].map(u => `  read ${u} as b${u}`).join('\n')
  assertFails(`wrapper "x" match service 0x181A\nconnect every 10min\n${reads}\ndecode\n  emit air.temperature u8(b2A01, 0)`, /at most 4 reads/)
})

test('an offset past a slot is a compile error, not a runtime surprise', () => {
  assertFails(`wrapper "x" match service 0x181A
connect every 10min
  read 2A6E as temp
decode
  emit air.temperature u8(temp, 16)`, /outside/)
})

// Fix round 1: len() and aes_ccm_decrypt() have no honest meaning scoped to
// a named GATT buffer (PAYLOAD_LEN/AES_CCM have no buffer operand -- both
// always act on the VM's one physical working buffer), and len() in
// particular would silently compare against the whole 64-byte concatenated
// buffer rather than any one slot's size, so a guard like
// `require len(temp) == 6` would fail on every run with no visible error.
// Reject both at compile time in a connect wrapper; both remain legal in an
// advert wrapper exactly as M3 defined them.

test('len() is a compile error in a connect wrapper (a named buffer has no runtime length)', () => {
  assertFails(`wrapper "x" match service 0x181A
connect every 10min
  read 2A6E as temp
decode
  emit air.temperature len(temp)`, /len\(temp\)/)
})

test('aes_ccm_decrypt() is a compile error in a connect wrapper (not scoped to a named buffer)', () => {
  assertFails(`wrapper "x" match service 0x181A
connect every 10min
  read 2A6E as temp
decode
  aes_ccm_decrypt(temp, 0, 6)
  emit air.temperature u8(temp, 0)`, /aes_ccm_decrypt/)
})

test('len() and aes_ccm_decrypt() remain legal in an advert wrapper (no connect block)', () => {
  const src = `wrapper "x" match manufacturer 0x0001
decode
  require len(payload) > 10
  aes_ccm_decrypt(payload, 4, 6)
  emit air.temperature u8(payload, 4)`
  const r = compileWrapper(src)
  assert.equal(r.ok, true)
  const asm = disassemble(r.bytecode)
  assert.match(asm, /PAYLOAD_LEN/)
  assert.match(asm, /AES_CCM/)
})

// ---- action block (M5b spec section 2, task 4) ----

test('an action block compiles with its write and confirm', () => {
  const r = compileWrapper(`wrapper "acme valve" match service 0x181A

action irrigation.open(duration_s max 300)
  write 2AF0 = 01 u16le(duration_s)
  confirm read 2AF1 as st
    require u8(st, 0) == 1

action switch.off()
  write 2AF0 = 00

connect every 5min
  read 2AF1 as st
decode
  emit switch.state  u8(st, 0)`)
  assert.equal(r.ok, true)
  // switch.off is present because irrigation.open here has no
  // closes_itself, so the hub owes the close and the compiler now requires
  // something to close it with (whole-branch review, finding 3). The
  // assertions below are all about actions[0], the open, and are unchanged.
  assert.equal(r.actions.length, 2)
  assert.equal(r.actions[1].name, 'switch.off')
  assert.equal(r.actions[0].actionId, 2)
  assert.equal(r.actions[0].paramMax, 300)
  assert.deepEqual(r.actions[0].write.bytes, [0x01])
  assert.equal(r.actions[0].write.paramOffset, 1)
  assert.equal(r.actions[0].confirm.value, 1)
  assert.match(disassemble(r.bytecode), /ACTION irrigation\.open/)
})

test('a wrapper may not raise an action bound above the firmware maximum', () => {
  const r = compileWrapper(`wrapper "greedy" match service 0x181A
action irrigation.open(duration_s max 600)
  write 2AF0 = 01 u16le(duration_s)
decode
  emit switch.state  0`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /max 600 exceeds the 300/)
})

test('an unknown action name is a compile error', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action laser.fire(duration_s max 5)
  write 2AF0 = 01
decode
  emit switch.state  0`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /unknown action 'laser\.fire'/)
})

test('a parameterless action rejects a parameter splice', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action switch.on()
  write 2AF0 = 01 u16le(duration_s)
decode
  emit switch.state  0`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /switch\.on takes no parameter/)
})

test('an action with no confirm compiles and is marked unconfirmable', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action switch.off()
  write 2AF0 = 00
decode
  emit switch.state  0`)
  assert.equal(r.ok, true)
  assert.equal(r.actions[0].confirm, null)
})

// ---- closes_itself (M5b spec section 4.3, fix round 1) ----
// Device-local timed-off is the PREFERRED path (the DIY profile's mandatory
// behaviour); the hub-scheduled close is the weaker fallback. Without a way
// to set psvm.h flags bit 0, every timed action was stuck on the weak path.

test('closes_itself sets deviceLocal (flags bit 0)', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action irrigation.open(duration_s max 300)
  write 2AF0 = 01 u16le(duration_s)
  closes_itself
decode
  emit switch.state  0`)
  assert.equal(r.ok, true)
  assert.equal(r.actions[0].deviceLocal, true)
})

test('an action with no closes_itself is not device-local (flags bit 0 clear)', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action irrigation.open(duration_s max 300)
  write 2AF0 = 01 u16le(duration_s)
action switch.off()
  write 2AF0 = 00
decode
  emit switch.state  0`)
  assert.equal(r.ok, true)
  assert.equal(r.actions[0].deviceLocal, false)
})

// ---- the hub-owed close must be declarable (whole-branch review, finding 3;
// mirrors psvm_validate()'s own rejection, which is the enforcement point) ----

test('a duration action with no closes_itself and no switch.off is a compile error naming the reason', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action irrigation.open(duration_s max 300)
  write 2AF0 = 01 u16le(duration_s)
decode
  emit switch.state  0`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /the hub owes its close/)
  assert.match(r.errors[0].message, /switch\.off/)
})

test('closes_itself alone satisfies the hub-owed-close rule (no switch.off needed)', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action irrigation.open(duration_s max 300)
  write 2AF0 = 01 u16le(duration_s)
  closes_itself
decode
  emit switch.state  0`)
  assert.equal(r.ok, true)
})

test('a parameterless action alone needs no switch.off', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action switch.on()
  write 2AF0 = 01
decode
  emit switch.state  0`)
  assert.equal(r.ok, true)
})

test('closes_itself on a parameterless action is a compile error naming the reason', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action switch.on()
  write 2AF0 = 01
  closes_itself
decode
  emit switch.state  0`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /closes_itself requires an action with a duration parameter/)
  assert.match(r.errors[0].message, /never written to it/)
})

test('a second closes_itself on the same action is a compile error', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action irrigation.open(duration_s max 300)
  write 2AF0 = 01 u16le(duration_s)
  closes_itself
  closes_itself
decode
  emit switch.state  0`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /closes_itself may only be declared once/)
})

test('the emitted per-action flags byte is 0x03 when closes_itself and confirm are both present', () => {
  // No connect block, so the action table starts right after the code
  // section with no consts/refs in between (constCount/refCount both 0 for
  // this source -- asserted below so a future change to this source that
  // silently added a const would fail loudly here instead of misreading a
  // stale byte offset).
  const src = `wrapper "x" match service 0x181A
action irrigation.open(duration_s max 300)
  write 2AF0 = 01 u16le(duration_s)
  closes_itself
  confirm read 2AF1 as st
    require u8(st, 0) == 1
decode
  emit switch.state  u8(payload, 0)`
  const r = compileWrapper(src)
  assert.equal(r.ok, true)
  const view = new DataView(r.bytecode.buffer, r.bytecode.byteOffset, r.bytecode.byteLength)
  const constCount = view.getUint16(12, true)
  const refCount = view.getUint16(14, true)
  const codeLen = view.getUint16(16, true)
  assert.equal(constCount, 0)
  assert.equal(refCount, 0)
  const HEADER_LEN = 18
  const actionTableStart = HEADER_LEN + codeLen // no connect-plan section in this source
  const flagsOffset = actionTableStart + 1 /* action_count */ + 1 /* action_id */ + 2 /* param_max */
  assert.equal(r.bytecode[flagsOffset], 0x03)
})

// ---- duplicate action_id / PSVM_ACTION_MAX (fix round 1: these rejections
// were implemented and correct per the brief's Step 4, but untested) ----

test('a duplicate action_id in one wrapper is a compile error', () => {
  const r = compileWrapper(`wrapper "x" match service 0x181A
action switch.on()
  write 2AF0 = 01
action switch.on()
  write 2AF0 = 01
decode
  emit switch.state  0`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /duplicate action 'switch\.on'/)
})

test('more than PSVM_ACTION_MAX actions in one wrapper is a compile error', () => {
  // ACTIONS (plan-limits.js) defines exactly PSVM_ACTION_MAX (4) distinct
  // actions, so a 5th action block is necessarily a repeat of one of the
  // first four -- this exercises the count-exceeded path specifically (not
  // the duplicate-id path), which is why the count check runs first in
  // parser.js's action-block loop.
  const r = compileWrapper(`wrapper "x" match service 0x181A
action switch.on()
  write 2AF0 = 01
action switch.off()
  write 2AF0 = 00
action irrigation.open(duration_s max 300)
  write 2AF1 = 01 u16le(duration_s)
action pump.run(duration_s max 120)
  write 2AF2 = 01 u16le(duration_s)
action switch.on()
  write 2AF0 = 01
decode
  emit switch.state  0`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /more than 4 actions/)
  assert.match(r.errors[0].message, /PSVM_ACTION_MAX/)
})
