// Rules dialect action path (M5b Task 10): a rule's `then` clause may fire
// a plant or device action, e.g. `plant("Ficus").irrigation.open(8s)`,
// compiling to CALL_ACTION (0x52). `compile` is aliased to compileRule to
// match the brief's test vectors verbatim.
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { compile as compileRule, disassemble } from '../index.js'
import { tokenize } from '../lexer.js'
import { parse } from '../parser.js'

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

test('a button.action rule compiles (the "action" keyword is a valid cap-name segment)', () => {
  // Regression: `action` is a reserved keyword (M5b wrapper action block),
  // so the parser used to reject `button.action` with "expected identifier,
  // got 'action'" -- making the momentary button.action capability (cap 9)
  // unusable in a rule. Cap-name segments now accept a keyword token.
  const r = compileRule(`rule "btn single"
when device("zb:13F92704008D1500").button.action == 1
then log("single press")
mode edge
cooldown 1s`)
  assert.equal(r.ok, true)
  assert.match(disassemble(r.bytecode), /button\.action/)
})

// ---- Task 10: `@N` endpoint qualifier ----

test('@N parses on a capability ref (condition)', () => {
  const ast = parse(tokenize('rule "r" when device("zb:AA").switch.state@2 == 1 then log("x")'))
  assert.equal(ast.when.left.type, 'ref')
  assert.equal(ast.when.left.endpoint, 2)
})

test('bare ref has no explicit endpoint (defaults to lowest)', () => {
  const ast = parse(tokenize('rule "r" when device("zb:AA").switch.state == 1 then log("x")'))
  assert.equal(ast.when.left.type, 'ref')
  assert.equal(ast.when.left.endpoint, undefined)
})

test('@0 and @999 are rejected', () => {
  assert.throws(
    () => parse(tokenize('rule "r" when device("zb:AA").switch.state@0 == 1 then log("x")')),
    /endpoint/
  )
  assert.throws(
    () => parse(tokenize('rule "r" when device("zb:AA").switch.state@999 == 1 then log("x")')),
    /endpoint/
  )
})

test('an @N ref compiles: the ref table carries the endpoint and LOAD_REF renders it', () => {
  const r = compileRule('rule "r" when device("zb:AA").switch.state@2 == 1 then log("x")')
  assert.equal(r.ok, true)
  assert.deepEqual(r.refs, [{ kind: 1, name: 'zb:AA', capability: 8, field: 0, endpoint: 2 }])
  assert.match(disassemble(r.bytecode), /LOAD_REF 0 ; device "zb:AA" switch\.state@2/)
})

test('a bare ref (no @N) compiles with endpoint 0 (unspecified -> lowest) and renders with no @ suffix', () => {
  const r = compileRule('rule "r" when device("zb:AA").switch.state == 1 then log("x")')
  assert.equal(r.ok, true)
  assert.equal(r.refs[0].endpoint, 0)
  assert.doesNotMatch(disassemble(r.bytecode), /switch\.state@/)
})

test('an @N action ref parses: the action node carries endpoint (then clause)', () => {
  const ast = parse(tokenize('rule "r" when device("zb:AA").switch.state == 1 then device("zb:AA").switch.on()@2'))
  assert.equal(ast.actions[0].type, 'action_call')
  assert.equal(ast.actions[0].endpoint, 2)
})

test('a bare action ref (no @N) has no explicit endpoint', () => {
  const ast = parse(tokenize('rule "r" when device("zb:AA").switch.state == 1 then device("zb:AA").switch.on()'))
  assert.equal(ast.actions[0].endpoint, undefined)
})

test('an @N action compiles: CALL_ACTION carries the endpoint byte', () => {
  const r = compileRule(`rule "pump"
when plant("Ficus").soil.moisture < 20
then device("zb:AA").switch.on()@2`)
  assert.equal(r.ok, true)
  assert.match(disassemble(r.bytecode), /CALL_ACTION device "zb:AA" switch\.on@2/)
})

test('@0 and @999 are rejected on an action ref too', () => {
  const bad0 = compileRule('rule "r" when plant("F").soil.moisture < 20 then device("zb:AA").switch.on()@0')
  assert.equal(bad0.ok, false)
  assert.match(bad0.errors[0].message, /endpoint/)

  const bad999 = compileRule('rule "r" when plant("F").soil.moisture < 20 then device("zb:AA").switch.on()@999')
  assert.equal(bad999.ok, false)
  assert.match(bad999.errors[0].message, /endpoint/)
})
