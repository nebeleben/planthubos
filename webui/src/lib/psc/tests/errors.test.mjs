import { test } from 'node:test'
import assert from 'node:assert/strict'
import { compile } from '../index.js'

function assertFails(src, msgPattern) {
  const r = compile(src)
  assert.equal(r.ok, false, `expected compile failure for: ${src}`)
  assert.ok(Array.isArray(r.errors) && r.errors.length > 0)
  const e = r.errors[0]
  assert.equal(typeof e.line, 'number')
  assert.equal(typeof e.col, 'number')
  assert.equal(typeof e.message, 'string')
  if (msgPattern) assert.match(e.message, msgPattern)
}

test('unknown capability', () => {
  assertFails(
    `rule "x"\nwhen plant("A").soil.mositure < 20\nthen log("y")`,
    /capability/i
  )
})

test('unit mismatch', () => {
  assertFails(
    `rule "x"\nwhen plant("A").air.temperature < 22%\nthen log("y")`,
    /unit/i
  )
})

test('missing rule name', () => {
  assertFails(
    `rule\nwhen plant("A").soil.moisture < 20\nthen log("y")`,
    /name|string/i
  )
})

test('missing when', () => {
  assertFails(
    `rule "x"\nthen log("y")`,
    /when/i
  )
})

test('missing then', () => {
  assertFails(
    `rule "x"\nwhen plant("A").soil.moisture < 20`,
    /then/i
  )
})

test('unterminated string', () => {
  assertFails(
    `rule "x\nwhen plant("A").soil.moisture < 20\nthen log("y")`,
    /string/i
  )
})

test('unknown action', () => {
  assertFails(
    `rule "x"\nwhen plant("A").soil.moisture < 20\nthen open("y")`,
    /action/i
  )
})

test('every below 30s minimum', () => {
  assertFails(
    `rule "x"\nwhen plant("A").soil.moisture < 20\nthen log("y")\nevery 5s`,
    /every|30/i
  )
})

test('duration typo', () => {
  assertFails(
    `rule "x"\nwhen plant("A").soil.moisture < 20\nthen log("y")\ncooldown 2hours`,
    /duration/i
  )
})

test('rule name over 48 chars', () => {
  const longName = 'x'.repeat(49)
  assertFails(
    `rule "${longName}"\nwhen plant("A").soil.moisture < 20\nthen log("y")`,
    /name|48/i
  )
})

test('unknown mode', () => {
  assertFails(
    `rule "x"\nwhen plant("A").soil.moisture < 20\nthen log("y")\nmode sometimes`,
    /mode/i
  )
})
