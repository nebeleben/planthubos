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
  const src = `wrapper "ruuvi" match manufacturer 0x0499
decode
  emit air.temperature   i16_be(payload, 3) * 0.005
  emit air.humidity      u16_be(payload, 5) * 0.0025
  emit battery.level     pct(u16_be(payload, 13) >> 5)`
  const r = compileWrapper(src)
  assert.equal(r.ok, true)
  assert.equal(r.name, 'ruuvi')
  assert.deepEqual(r.match, { kind: 1, key: 0x0499 })
  assert.equal(r.capsUsed.length, 3)
  assert.deepEqual(new Set(r.capsUsed), new Set([1, 5, 4])) // air.temperature, air.humidity, battery.level

  const asm = disassemble(r.bytecode)
  assert.match(asm, /LOAD_I16BE 3/)
  // f32 round-trip (0.005 -> 0.004999999888241291 etc.), so match loosely.
  assert.match(asm, /PUSH_CONST .*0\.00499/)
  assert.match(asm, /EMIT air\.temperature/)
  assert.match(asm, /LOAD_U16BE 5/)
  assert.match(asm, /PUSH_CONST .*0\.00249/)
  assert.match(asm, /EMIT air\.humidity/)
  assert.match(asm, /LOAD_U16BE 13/)
  // `>> 5` compiles to DIV by 32 (no dedicated shift opcode in M1's table).
  assert.match(asm, /PUSH_CONST .*32/)
  assert.match(asm, /DIV/)
  assert.match(asm, /EMIT battery\.level/)
  assert.match(asm, /HALT$/m)
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
