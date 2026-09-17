import { test } from 'node:test'
import assert from 'node:assert/strict'
import { WRAPPER_LIBRARY } from '../wrapperLibrary.js'
import { compileWrapper } from '../psc/index.js'

// Every catalog entry must be a real, compilable wrapper — a library entry
// that fails to compile would 500 on Install with no author to fix it.
test('every library wrapper compiles', () => {
  assert.ok(WRAPPER_LIBRARY.length >= 2)
  for (const w of WRAPPER_LIBRARY) {
    assert.ok(w.name && w.description && w.source, `entry missing fields: ${w.name}`)
    const r = compileWrapper(w.source)
    assert.ok(r.ok, `"${w.name}" failed to compile: ${JSON.stringify(r.errors || r)}`)
    assert.ok(r.bytecode && r.bytecode.length > 0, `"${w.name}" produced no bytecode`)
    // the compiled name must match the catalog name (the source's quoted name)
    assert.equal(r.name, w.name, `"${w.name}" compiled name mismatch: ${r.name}`)
  }
})

// Match keys are the whole point — assert each decodes off the format it claims.
test('Ruuvi matches manufacturer 0x0499 and emits temp/humidity/pressure', () => {
  const w = WRAPPER_LIBRARY.find((x) => x.name === 'Ruuvi RAWv2')
  const r = compileWrapper(w.source)
  assert.ok(r.ok)
  assert.equal(r.match.key, 0x0499)
  // capsUsed carries the numeric capability ids this wrapper emits (air.* per psc/caps.js)
  const caps = new Set(r.capsUsed)
  assert.ok(caps.has(1) && caps.has(5) && caps.has(6), `Ruuvi caps: ${[...caps]}`) // temp, humidity, pressure
})

test('Xiaomi ATC/pvvx matches service 0x181A and emits temp/humidity/battery', () => {
  const w = WRAPPER_LIBRARY.find((x) => x.name.startsWith('Xiaomi'))
  const r = compileWrapper(w.source)
  assert.ok(r.ok)
  assert.equal(r.match.key, 0x181a)
  const caps = new Set(r.capsUsed)
  assert.ok(caps.has(1) && caps.has(5) && caps.has(4), `ATC caps: ${[...caps]}`) // temp, humidity, battery
})
