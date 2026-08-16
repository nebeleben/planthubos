// Condensed grammar reference shared by both templates (M4 spec section 7).
//
// Deliberately compact: this string ships inside the gzipped web bundle,
// which is embedded in the firmware image, and the esp32c5 is at 8% flash
// free. It is a cheat-sheet, not the spec -- everything a model needs to
// emit valid source and nothing it does not.
//
// Derived from the compiler itself (src/lib/psc/parser.js, lexer.js) and
// its golden tests (psc/tests/compile.test.mjs, wrapper.test.mjs), not
// transcribed from a hand sketch -- M4 has no retry loop, so a wrong
// grammar reference here means every generation fails to compile silently.
import { CAPS } from '../../psc/caps.js'

const capLines = Object.entries(CAPS)
  .map(([name, c]) => `  ${name} (${c.unit})`)
  .join('\n')

export const CAPABILITIES = `Capabilities you may emit, with their units:\n${capLines}`

export const WRAPPER_DIALECT = `PlantScript wrapper dialect:

wrapper "<name>" match <service|manufacturer|mac_prefix> <0xHEX>
decode
  emit <capability> <expression>
  require <expression>

service/manufacturer match keys are 16-bit (max 0xFFFF); mac_prefix is the
device's 3-byte MAC prefix (max 0xFFFFFF). A wrapper may contain at most 16
emit statements.

Payload accessors -- offset/lsb/width must be integer literals, never
expressions, and are checked at compile time against the 31-byte payload
cap:
  u8(payload, i)                                        i: 0-30
  u16_le/u16_be/i16_le/i16_be(payload, i)                i: 0-29
  u24_le(payload, i)                                     i: 0-28
  u32_le(payload, i)                                     i: 0-27
  bits(payload, i, lsb, width)      width 1-32, lsb+width<=32,
                                     i+ceil((lsb+width)/8)<=31
  len(payload)

Arithmetic: + - * / and >> (>> is bit-exact and needs a non-negative left
operand). Prefer bits() to >> for field extraction.

CRITICAL -- what "payload" is: the bytes AFTER the matched advertising
structure's own header. A "manufacturer" match gets manufacturer data past
its 2-byte company id, so payload[0] is the first vendor byte, NOT the
company id. A "service" match gets service data past its 2-byte UUID. A
"mac_prefix" match gets the whole raw advertisement.

"require" rejects a payload whose shape does not match; a decode that hits
a failed require emits nothing.

Emitted values are range-checked against the capability's own limits, so a
wrong scale factor is dropped rather than stored.`

export const RULE_DIALECT = `PlantScript rule dialect:

rule "<name>"
when <condition>
then <action>[; <action>...]
mode edge|level           (optional, default edge)
cooldown <duration>       (optional, e.g. 2h)
every <duration>          (optional, e.g. 30min, allowed range 30s-24h)

If given, mode/cooldown/every must appear in that order (each optional,
each at most once). A duration is a number plus a unit: s/sec, m/min, or
h/hr.

Conditions reference a plant or device BY NAME, never by numeric id, and
compare its capability to a literal carrying a matching unit, e.g.
  plant("Monstera").soil.moisture < 22%
  device("AA:BB:CC:DD:EE:FF").air.temperature > 30C
Combine conditions with and / or / not ("not" binds tightest, then "and",
then "or"). A ".age" suffix reads the reading's age in seconds instead of
its value, and compares against a plain number with no unit, e.g.
  device("AA:BB:CC:DD:EE:FF").air.temperature.age > 3600

Actions: log("<message>") or notify("<message>") -- no other actions
exist. A message may interpolate a reference in braces; escape its inner
quotes, since it is still inside the outer string literal:
  notify("Monstera is dry: {plant(\\"Monstera\\").soil.moisture}%")

Modes: edge fires once on the transition into true; level fires on every
evaluation while the condition stays true.`
