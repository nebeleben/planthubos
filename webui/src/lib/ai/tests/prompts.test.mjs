import { test } from 'node:test'
import assert from 'node:assert/strict'
import { WRAPPER_TEMPLATE } from '../prompts/wrapper.js'
import { RULE_TEMPLATE } from '../prompts/rule.js'
import { extractSource, provenance } from '../extract.js'
import { compile, disassemble } from '../../psc/index.js'

const DEVICE = {
  id: 'ble:D0CF13E5BCCA',
  rssi: -46,
  samples: [{ hex: '051388578', len: 4, ts: 40 }, { hex: '051390578', len: 4, ts: 45 }],
}

test('wrapper prompt carries the samples, the OUI and the capability names', () => {
  const { system, user } = WRAPPER_TEMPLATE.build({ device: DEVICE })
  assert.match(system, /wrapper/i)
  assert.match(user, /051388578/)
  assert.match(user, /D0CF13/)
  assert.match(system, /air\.temperature/)
  assert.match(system, /payload/)
})

// The redaction promise, asserted against the string that actually goes out.
test('wrapper prompt leaks nothing spec section 4 forbids', () => {
  const { system, user } = WRAPPER_TEMPLATE.build({ device: DEVICE })
  const all = system + user
  assert.ok(!all.includes('D0CF13E5BCCA'), 'full MAC leaked into the prompt')
  assert.ok(!all.includes('ble:'), 'device id leaked into the prompt')
  assert.ok(!all.includes('-46'), 'rssi leaked into the prompt')
})

test('rule prompt carries plant names and capabilities but no device identifiers', () => {
  const plants = [{ id: 1, name: 'Fern',
                    bindings: [{ cap: 0, name: 'soil.moisture', device: 'ble:80EACA892A0A', value: 12 }] }]
  const { system, user } = RULE_TEMPLATE.build({ plants, request: 'water when dry' })
  const all = system + user
  assert.match(all, /Fern/)
  assert.match(all, /soil\.moisture/)
  assert.match(user, /water when dry/)
  assert.ok(!all.includes('80EACA892A0A'), 'device MAC leaked into the prompt')
  assert.ok(!all.includes('ble:'), 'device id leaked into the prompt')
})

test('templates declare an integer version', () => {
  assert.equal(Number.isInteger(WRAPPER_TEMPLATE.version), true)
  assert.equal(Number.isInteger(RULE_TEMPLATE.version), true)
})

test('extracts a fenced block', () => {
  const out = extractSource('Here you go:\n\n```\nwrapper "x" match service 0x181A\n```\n\nEnjoy.')
  assert.equal(out, 'wrapper "x" match service 0x181A')
})

test('extracts a language-tagged fence and prefers the first block', () => {
  const out = extractSource('```plantscript\nfirst\n```\ntext\n```\nsecond\n```')
  assert.equal(out, 'first')
})

test('returns null when there is no code block at all', () => {
  assert.equal(extractSource('I am unable to help with that.'), null)
})

// Fix round 1: a fence whose code starts on the opening fence's own line
// (no language tag, no separating newline before the header) must not
// have that line mistaken for an info string and dropped -- for a
// wrapper, the match header is the single most load-bearing line.
test('extracts a same-line fence, keeping the header line as code', () => {
  const out = extractSource('```wrapper "x" match service 0x1234\ndecode\n  emit air.temperature u8(payload, 0)\n```')
  assert.equal(out, 'wrapper "x" match service 0x1234\ndecode\n  emit air.temperature u8(payload, 0)')
})

// Fix round 1: RULE_DIALECT says mode/cooldown/every "must appear in that
// order" but does not claim each may appear only once -- parser.js only
// rejects a clause going backward (idx < orderIdx), not a repeat. Pin
// that a repeat compiles and the later value wins, so the prompt text
// never overstates a constraint the compiler doesn't enforce.
test('repeating a rule clause compiles and the later value wins, matching RULE_DIALECT', () => {
  const r = compile('rule "x"\nwhen plant("A").soil.moisture < 20%\nthen log("y")\nmode edge\nmode level')
  assert.equal(r.ok, true)
  assert.equal(r.mode, 'level')
})

// Fix round 1: RULE_DIALECT says quotes inside a string interpolation
// {...} may be written bare or escaped, both compiling the same way.
// readInterpolationSpan (lexer.js) only tracks \, {, } -- it has no
// notion of quote termination -- so this should produce byte-identical
// bytecode either way.
test('bare and escaped inner quotes in an interpolation compile identically, matching RULE_DIALECT', () => {
  const escaped = compile('rule "x"\nwhen plant("A").soil.moisture < 20%\nthen notify("v: {plant(\\"A\\").soil.moisture}%")')
  const bare = compile('rule "x"\nwhen plant("A").soil.moisture < 20%\nthen notify("v: {plant("A").soil.moisture}%")')
  assert.equal(escaped.ok, true)
  assert.equal(bare.ok, true)
  assert.equal(disassemble(escaped.bytecode), disassemble(bare.bytecode))
})

// Fix round 1: the inventory line used to render "plant <id> \"<name>\"",
// putting the bare token 'plant' immediately before an integer -- a
// surface echo of the non-parsing plant(<id>) form (rules address plants
// by name string only). Confirm that pattern is gone.
test('rule prompt inventory never echoes the non-parsing plant(<id>) form', () => {
  const plants = [{ id: 1, name: 'Fern', bindings: [{ cap: 0, name: 'soil.moisture', device: 'ble:80EACA892A0A', value: 12 }] }]
  const { user } = RULE_TEMPLATE.build({ plants, request: 'water when dry' })
  assert.doesNotMatch(user, /plant\s*\(?\s*\d/i)
})

test('provenance line names the template version and model', () => {
  const p = provenance('wrapper', 1, 'claude-sonnet-5')
  assert.match(p, /^# generated by planthub ai/)
  assert.match(p, /wrapper v1/)
  assert.match(p, /claude-sonnet-5/)
})
