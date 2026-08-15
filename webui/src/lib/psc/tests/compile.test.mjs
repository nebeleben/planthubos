import { test } from 'node:test'
import assert from 'node:assert/strict'
import { compile, disassemble } from '../index.js'

test('spec example compiles to expected ops', () => {
  const src = `rule "monstera dry"
when plant("Monstera").soil.moisture < 22%
then notify("Monstera is dry: {plant(\\"Monstera\\").soil.moisture}%")
mode edge
cooldown 2h
every 30min`
  const r = compile(src)
  assert.equal(r.ok, true)
  assert.equal(r.name, 'monstera dry')
  assert.equal(r.mode, 'edge')
  assert.equal(r.cooldown_s, 7200)
  assert.equal(r.every_s, 1800)
  assert.deepEqual(r.refs, [{ kind: 0, name: 'Monstera', capability: 0, field: 0 }])
  const asm = disassemble(r.bytecode)
  assert.match(asm, /LOAD_REF 0/)
  assert.match(asm, /PUSH_CONST .*22/)
  assert.match(asm, /LT\n.*HALT_BOOL/)
  assert.match(asm, /BUILD_STR 3\n.*CALL_BUILTIN notify\n.*HALT/)
})

test('same ref deduplicates in ref table', () => {
  const r = compile(`rule "x"\nwhen plant("A").soil.moisture < 20 and plant("A").soil.moisture > 5\nthen log("y")`)
  assert.equal(r.ok, true)
  assert.equal(r.refs.length, 1)
})

test('.age ref and device() ref', () => {
  const r = compile(`rule "stale"\nwhen device("AA:BB:CC:DD:EE:FF").air.temperature.age > 3600\nthen log("stale")`)
  assert.equal(r.ok, true)
  assert.deepEqual(r.refs, [{ kind: 1, name: 'AA:BB:CC:DD:EE:FF', capability: 1, field: 1 }])
})

test('operator precedence: and binds tighter than or, not tightest', () => {
  const r = compile(`rule "p"\nwhen not plant("A").soil.moisture < 10 or plant("A").battery.level < 20 and plant("A").soil.moisture > 50\nthen log("z")`)
  assert.equal(r.ok, true)
  const asm = disassemble(r.bytecode)
  assert.match(asm, /NOT[\s\S]*AND[\s\S]*OR/)  /* NOT before AND before OR in emit order */
})
