// Rules dialect action path (M5b Task 10): a rule's `then` clause may fire
// a plant or device action, e.g. `plant("Ficus").irrigation.open(8s)`,
// compiling to CALL_ACTION (0x52). `compile` is aliased to compileRule to
// match the brief's test vectors verbatim.
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { compile as compileRule, disassemble } from '../index.js'

test('a rule can fire a plant action', () => {
  const r = compileRule(`rule "water the ficus"
when plant("Ficus").soil.moisture < 25
then plant("Ficus").irrigation.open(8s)
cooldown 6h`)
  assert.equal(r.ok, true)
  assert.match(disassemble(r.bytecode), /CALL_ACTION plant "Ficus" irrigation\.open/)
})

test('a rule can fire a device action', () => {
  const r = compileRule(`rule "pump"
when plant("Ficus").soil.moisture < 20
then device("ble:AABBCCDDEEFF").switch.on()`)
  assert.equal(r.ok, true)
})

test('a rule may not exceed an action bound', () => {
  const r = compileRule(`rule "flood"
when plant("Ficus").soil.moisture < 20
then plant("Ficus").irrigation.open(600s)`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /600 exceeds the 300/)
})

test('a parameterless action takes no argument', () => {
  const r = compileRule(`rule "x"
when plant("F").soil.moisture < 20
then plant("F").switch.on(5s)`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /switch\.on takes no parameter/)
})

// ---- coverage beyond the brief's verbatim vectors ----

test('a parameterized action requires its duration argument', () => {
  const r = compileRule(`rule "x"
when plant("F").soil.moisture < 20
then plant("F").irrigation.open()`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /irrigation\.open requires a duration parameter/)
})

test('an unknown action name is a compile error', () => {
  const r = compileRule(`rule "x"
when plant("F").soil.moisture < 20
then plant("F").misting.spray(5s)`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /unknown action 'misting.spray'/)
})

test('a rule mixing a log action and a plant action compiles both', () => {
  const r = compileRule(`rule "x"
when plant("F").soil.moisture < 20
then log("watering"); plant("F").irrigation.open(8s)`)
  assert.equal(r.ok, true)
  const asm = disassemble(r.bytecode)
  assert.match(asm, /CALL_BUILTIN log/)
  assert.match(asm, /CALL_ACTION plant "F" irrigation\.open/)
})

test('pump.run compiles at its own (lower) bound', () => {
  const r = compileRule(`rule "x"
when plant("F").soil.moisture < 20
then plant("F").pump.run(120s)`)
  assert.equal(r.ok, true)
  assert.match(disassemble(r.bytecode), /CALL_ACTION plant "F" pump\.run/)
})

test('a zero-second duration is rejected (action.h: zero is not an open)', () => {
  const r = compileRule(`rule "x"
when plant("F").soil.moisture < 20
then plant("F").irrigation.open(0s)`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /irrigation\.open requires a nonzero duration/)
})

test('pump.run rejects a duration over its own 120s bound', () => {
  const r = compileRule(`rule "x"
when plant("F").soil.moisture < 20
then plant("F").pump.run(121s)`)
  assert.equal(r.ok, false)
  assert.match(r.errors[0].message, /121 exceeds the 120/)
})
