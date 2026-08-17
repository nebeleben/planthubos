// PlantScript recursive-descent parser (spec section 1).
// Precedence, loose to tight: or -> and -> not -> cmp -> add -> mul -> unary -> primary.

import { PSError } from './lexer.js'
import { CAPS } from './caps.js'
import { PSVM_PLAN_MAX_READS, PSVM_PLAN_MAX_WRITES, PSVM_PLAN_WRITE_MAX, PSVM_PLAN_SLOT } from './plan-limits.js'

const DURATION_UNITS = { s: 1, sec: 1, m: 60, min: 60, h: 3600, hr: 3600 }
const CMP_OPS = new Set(['<', '<=', '>', '>=', '==', '!='])
const MODES = new Set(['edge', 'level'])
const ACTIONS = new Set(['log', 'notify'])

// Wrapper dialect (M3 spec section 3).
const MATCH_KIND_ID = { service: 0, manufacturer: 1, mac_prefix: 2 }
const MATCH_KEY_MAX = { service: 0xFFFF, manufacturer: 0xFFFF, mac_prefix: 0xFFFFFF }
// name -> byte width, for the single-literal-offset accessors (u8..u32_le).
// `bits` and `len` have their own arg shapes and aren't in this table.
const ACCESSOR_WIDTH = {
  u8: 1, u16_le: 2, u16_be: 2, i16_le: 2, i16_be: 2, u24_le: 3, u32_le: 4,
}
// Every payload-accessor primary parsePrimary() recognises in the wrapper
// dialect: the fixed-width loads above plus 'bits' and 'len', which have
// their own arg shapes (parseAccessorCall() branches on the name).
const ACCESSOR_NAMES = new Set([...Object.keys(ACCESSOR_WIDTH), 'bits', 'len'])

class Parser {
  // dialect: 'rules' (default) or 'wrapper' -- selects parseFile()'s entry
  // point and parsePrimary()'s payload-accessor/pct branch. Everything else
  // (tokenizer, or/and/not/cmp/shift/add/mul/unary precedence, string
  // interpolation) is shared, per the brief: "a second dialect in the same
  // toolchain, not a fork of it".
  constructor(tokens, dialect = 'rules') {
    this.tokens = tokens
    this.pos = 0
    this.dialect = dialect
    // name -> compile-time slot offset (0/16/32/48), set only while parsing
    // a wrapper's `decode` block that follows a `connect` block (M5a spec
    // section 2). null means "no connect block": accessors address
    // 'payload' exactly as M3 defined them, unchanged.
    this.buffers = null
    // name -> the fewest bytes that buffer's slot must actually contain for
    // the decode block to mean anything: max(offset + accessor width) over
    // every accessor that reads it. Emitted into the plan as each read's
    // min_len so the hub can reject a short read instead of zero-padding it
    // into a plausible wrong value (M5a spec section 4). Populated only
    // while this.buffers is set, i.e. only for a connect wrapper.
    this.bufMin = null
  }

  peek(offset = 0) {
    return this.tokens[Math.min(this.pos + offset, this.tokens.length - 1)]
  }

  advance() {
    const t = this.tokens[this.pos]
    if (this.pos < this.tokens.length - 1) this.pos++
    return t
  }

  isKeyword(value) {
    const t = this.peek()
    return t.type === 'KEYWORD' && t.value === value
  }

  isPunct(value) {
    const t = this.peek()
    return t.type === 'PUNCT' && t.value === value
  }

  expectKeyword(value) {
    if (!this.isKeyword(value)) {
      const t = this.peek()
      throw new PSError(`expected '${value}', got '${describeToken(t)}'`, t.line, t.col)
    }
    return this.advance()
  }

  expectPunct(value) {
    if (!this.isPunct(value)) {
      const t = this.peek()
      throw new PSError(`expected '${value}', got '${describeToken(t)}'`, t.line, t.col)
    }
    return this.advance()
  }

  expectIdent() {
    const t = this.peek()
    if (t.type !== 'IDENT') {
      throw new PSError(`expected identifier, got '${describeToken(t)}'`, t.line, t.col)
    }
    return this.advance()
  }

  expectString() {
    const t = this.peek()
    if (t.type !== 'STRING') {
      throw new PSError(`expected string, got '${describeToken(t)}'`, t.line, t.col)
    }
    return this.advance()
  }

  // A payload accessor's offset/lsb/width args (wrapper dialect): must be a
  // compile-time non-negative integer literal, never a general expression
  // -- that's what makes "catch out-of-range literal offsets at compile
  // time" possible at all (brief, Step 4).
  expectIntLiteral() {
    const t = this.peek()
    if (t.type !== 'NUMBER' || t.unit || !Number.isInteger(t.value) || t.value < 0) {
      throw new PSError(`expected a non-negative integer literal, got '${describeToken(t)}'`, t.line, t.col)
    }
    return this.advance()
  }

  // A payload accessor's FIRST argument (wrapper dialect): 'payload' in a
  // wrapper with no connect block (M3, unchanged); a declared buffer name
  // in one that has a connect block, where 'payload' itself is not
  // addressable (M5a spec section 2: named buffers replace it, and section
  // 9 defers mixing advert and GATT bytes in the same wrapper). Returns
  // {name, slotOffset} either way -- slotOffset is always 0 for 'payload'.
  expectBufferIdent() {
    const t = this.peek()
    if (t.type !== 'IDENT') {
      throw new PSError(`expected 'payload' or a declared buffer name, got '${describeToken(t)}'`, t.line, t.col)
    }
    if (this.buffers) {
      if (t.value === 'payload') {
        throw new PSError(
          "'payload' is not addressable in a wrapper with a connect block (spec section 9 defers mixing advert and GATT bytes)",
          t.line, t.col
        )
      }
      if (!this.buffers.has(t.value)) {
        throw new PSError(`unknown buffer '${t.value}'`, t.line, t.col)
      }
      this.advance()
      return { name: t.value, slotOffset: this.buffers.get(t.value) }
    }
    if (t.value !== 'payload') {
      throw new PSError(`expected 'payload', got '${describeToken(t)}'`, t.line, t.col)
    }
    this.advance()
    return { name: 'payload', slotOffset: 0 }
  }

  // A bare hex token (M5a spec section 2: `read <uuid16> as <name>` /
  // `write <uuid16> = <hex>`), lexed by tokenize()'s hex mode right after
  // 'read', 'write' or '='. Never a general expression -- same reasoning as
  // expectIntLiteral: these values must be resolvable at compile time.
  expectHex() {
    const t = this.peek()
    if (t.type !== 'HEX') {
      throw new PSError(`expected a hex value, got '${describeToken(t)}'`, t.line, t.col)
    }
    return this.advance()
  }

  // ---- top level ----

  parseRuleFile() {
    this.expectKeyword('rule')
    const nameTok = this.expectString()
    const name = plainStringValue(nameTok)
    if (name.length < 1 || name.length > 48) {
      throw new PSError('rule name must be 1-48 characters', nameTok.line, nameTok.col)
    }

    this.expectKeyword('when')
    const when = this.parseOr()

    this.expectKeyword('then')
    const actions = this.parseActions()

    let mode = 'edge'
    let cooldown_s = 0
    let every_s = 0

    const order = ['mode', 'cooldown', 'every']
    let orderIdx = 0
    while (this.isKeyword('mode') || this.isKeyword('cooldown') || this.isKeyword('every')) {
      const clause = this.peek().value
      const idx = order.indexOf(clause)
      if (idx < orderIdx) {
        const t = this.peek()
        throw new PSError(`clause '${clause}' out of order`, t.line, t.col)
      }
      orderIdx = idx
      this.advance()
      if (clause === 'mode') {
        const t = this.expectIdent()
        if (!MODES.has(t.value)) {
          throw new PSError(`unknown mode '${t.value}' (expected edge or level)`, t.line, t.col)
        }
        mode = t.value
      } else if (clause === 'cooldown') {
        cooldown_s = this.parseDuration('cooldown', 0, Infinity)
      } else {
        every_s = this.parseDuration('every', 30, 86400)
      }
    }

    const t = this.peek()
    if (t.type !== 'EOF') {
      throw new PSError(`unexpected token '${describeToken(t)}'`, t.line, t.col)
    }

    return { name, when, actions, mode, cooldown_s, every_s }
  }

  // ---- wrapper dialect (M3 spec section 3) ----
  // `wrapper "<name>" match <service|manufacturer|mac_prefix> <value>`
  // followed by a `decode` block of require/emit/aes_ccm_decrypt statements.
  parseWrapperFile() {
    this.expectKeyword('wrapper')
    const nameTok = this.expectString()
    const name = plainStringValue(nameTok)
    if (name.length < 1 || name.length > 48) {
      throw new PSError('wrapper name must be 1-48 characters', nameTok.line, nameTok.col)
    }

    this.expectKeyword('match')
    const kindTok = this.peek()
    const kindName = kindTok.type === 'KEYWORD' ? kindTok.value : null
    if (!kindName || !(kindName in MATCH_KIND_ID)) {
      throw new PSError(
        `unknown match kind '${describeToken(kindTok)}' (expected service, manufacturer or mac_prefix)`,
        kindTok.line, kindTok.col
      )
    }
    this.advance()
    const kind = MATCH_KIND_ID[kindName]

    const keyTok = this.peek()
    if (keyTok.type !== 'NUMBER' || !Number.isInteger(keyTok.value) || keyTok.value < 0) {
      throw new PSError('expected a non-negative integer match key', keyTok.line, keyTok.col)
    }
    this.advance()
    const maxKey = MATCH_KEY_MAX[kindName]
    if (keyTok.value > maxKey) {
      throw new PSError(
        `match key 0x${keyTok.value.toString(16)} out of range for ${kindName} (max 0x${maxKey.toString(16)})`,
        keyTok.line, keyTok.col
      )
    }

    // connect block (M5a spec section 2): optional, between the header and
    // `decode`. Builds the name -> slot-offset table `decode` resolves
    // buffer identifiers against (expectBufferIdent()).
    let connect = null
    if (this.isKeyword('connect')) {
      connect = this.parseConnectBlock()
      this.buffers = new Map(connect.reads.map((r) => [r.name, r.offset]))
      this.bufMin = new Map(connect.reads.map((r) => [r.name, 0]))
    }

    this.expectKeyword('decode')
    const statements = []
    while (this.peek().type !== 'EOF') {
      statements.push(this.parseDecodeStmt())
    }
    if (statements.length === 0) {
      const t = this.peek()
      throw new PSError('decode block must contain at least one statement', t.line, t.col)
    }

    if (connect) {
      // Every accessor has now been parsed, so bufMin is final. A buffer no
      // accessor reads keeps minLen 1: the read still has to return
      // SOMETHING for the attempt to count, and demanding 0 would let an
      // empty response through as a success.
      for (const r of connect.reads) r.minLen = Math.max(1, this.bufMin.get(r.name) || 0)
    }

    return { name, match: { kind, key: keyTok.value }, connect, statements }
  }

  // Raises buffer `name`'s required length to `need` (offset + accessor
  // width). See this.bufMin.
  noteBufMin(name, need) {
    if (!this.bufMin) return
    const cur = this.bufMin.get(name) || 0
    if (need > cur) this.bufMin.set(name, need)
  }

  // `connect every <duration>` followed by `read`/`write` lines (M5a spec
  // section 2). Reuses parseDuration() -- same grammar M1's rules `every`
  // clause already uses -- rather than writing a second duration parser.
  // Read offsets are assigned here, in declaration order, as index*16
  // (PSVM_PLAN_SLOT): this compile-time table is the entire mechanism that
  // lets `decode` address a named GATT buffer with the SAME accessor
  // opcodes it already has for `payload`.
  parseConnectBlock() {
    this.expectKeyword('connect')
    this.expectKeyword('every')
    const intervalS = this.parseDuration('connect', 60, 86400)

    const reads = []
    const writes = []
    const names = new Set()

    while (this.isKeyword('read') || this.isKeyword('write')) {
      if (this.isKeyword('write')) {
        const wTok = this.advance()
        // Every write is performed before every read, whatever order they
        // were written in (gatt_fsm.c's begin_writes_or_reads()). Accepting
        // a write after a read would compile to something that does not
        // match what the author wrote, so say so instead of reordering
        // silently -- a wake write placed after a read reads as "read, then
        // wake", which is never what was meant.
        if (reads.length > 0) {
          throw new PSError(
            'a connect block performs every write before every read, so `write` must come before the first `read`',
            wTok.line, wTok.col
          )
        }
        const uuidTok = this.expectHex()
        if (uuidTok.value > 0xFFFF) {
          throw new PSError(`write UUID 0x${uuidTok.raw} out of range for a 16-bit UUID`, uuidTok.line, uuidTok.col)
        }
        this.expectPunct('=')
        const dataTok = this.expectHex()
        if (dataTok.raw.length === 0 || dataTok.raw.length % 2 !== 0) {
          throw new PSError(
            `write data must be a whole number of bytes (an even number of hex digits), got '${dataTok.raw}'`,
            dataTok.line, dataTok.col
          )
        }
        const data = []
        for (let i = 0; i < dataTok.raw.length; i += 2) data.push(parseInt(dataTok.raw.slice(i, i + 2), 16))
        if (data.length > PSVM_PLAN_WRITE_MAX) {
          throw new PSError(
            `write data must be at most ${PSVM_PLAN_WRITE_MAX} bytes (PSVM_PLAN_WRITE_MAX), got ${data.length}`,
            dataTok.line, dataTok.col
          )
        }
        if (writes.length >= PSVM_PLAN_MAX_WRITES) {
          throw new PSError(
            `connect block allows at most ${PSVM_PLAN_MAX_WRITES} writes (PSVM_PLAN_MAX_WRITES)`,
            wTok.line, wTok.col
          )
        }
        writes.push({ uuid16: uuidTok.value, data })
      } else {
        const rTok = this.advance()
        const uuidTok = this.expectHex()
        if (uuidTok.value > 0xFFFF) {
          throw new PSError(`read UUID 0x${uuidTok.raw} out of range for a 16-bit UUID`, uuidTok.line, uuidTok.col)
        }
        // Two reads of the same characteristic cost a redundant round trip
        // and weaken gatt_fsm.c's completion-identity check, which
        // distinguishes reads by uuid16 alone: a stale completion for the
        // first would be accepted into the second's slot.
        if (reads.some((r) => r.uuid16 === uuidTok.value)) {
          throw new PSError(
            `duplicate read of UUID 0x${uuidTok.raw}: each characteristic may be read once per connect block`,
            uuidTok.line, uuidTok.col
          )
        }
        this.expectKeyword('as')
        const nameTok = this.expectIdent()
        if (names.has(nameTok.value)) {
          throw new PSError(`duplicate buffer name '${nameTok.value}'`, nameTok.line, nameTok.col)
        }
        if (reads.length >= PSVM_PLAN_MAX_READS) {
          throw new PSError(
            `connect block allows at most ${PSVM_PLAN_MAX_READS} reads (PSVM_PLAN_MAX_READS)`,
            rTok.line, rTok.col
          )
        }
        names.add(nameTok.value)
        reads.push({ uuid16: uuidTok.value, name: nameTok.value, offset: reads.length * PSVM_PLAN_SLOT })
      }
    }

    if (reads.length === 0) {
      const t = this.peek()
      throw new PSError('connect block must declare at least one read', t.line, t.col)
    }

    return { intervalS, reads, writes }
  }

  parseDecodeStmt() {
    if (this.isKeyword('require')) {
      const t = this.advance()
      const expr = this.parseOr()
      return { type: 'require', expr, line: t.line, col: t.col }
    }
    if (this.isKeyword('emit')) {
      const t = this.advance()
      const seg1 = this.expectIdent()
      this.expectPunct('.')
      const seg2 = this.expectIdent()
      const capability = `${seg1.value}.${seg2.value}`
      if (!CAPS[capability]) {
        throw new PSError(`unknown capability '${capability}'`, seg1.line, seg1.col)
      }
      const expr = this.parseOr()
      return { type: 'emit', capability, expr, line: t.line, col: t.col }
    }
    const t = this.peek()
    if (t.type === 'IDENT' && t.value === 'aes_ccm_decrypt') {
      return this.parseDecryptStmt()
    }
    throw new PSError(
      `expected 'require', 'emit' or 'aes_ccm_decrypt', got '${describeToken(t)}'`,
      t.line, t.col
    )
  }

  // aes_ccm_decrypt(payload, <expr>, <expr>) -- offset/len may be any
  // expression (unlike the fixed-offset accessors), matching AES_CCM's
  // pop-two-operands-off-the-stack opcode shape (no immediate operand).
  //
  // The AES_CCM opcode has no buffer operand at all: it always decrypts in
  // place against the VM's one physical working buffer, i.e. the
  // advertisement payload. There is no way to scope it to a single 16-byte
  // GATT slot, so it is rejected outright in a connect wrapper rather than
  // silently doing something other than what a buffer name would suggest.
  parseDecryptStmt() {
    const t = this.advance() // 'aes_ccm_decrypt'
    if (this.buffers) {
      throw new PSError(
        "aes_ccm_decrypt() operates on the advertisement payload and is not scoped to a named buffer, so it cannot be used in a wrapper with a connect block",
        t.line, t.col
      )
    }
    this.expectPunct('(')
    this.expectBufferIdent()
    this.expectPunct(',')
    const offsetExpr = this.parseOr()
    this.expectPunct(',')
    const lenExpr = this.parseOr()
    this.expectPunct(')')
    return { type: 'decrypt', offsetExpr, lenExpr, line: t.line, col: t.col }
  }

  // u8(payload, i) / u16_le/u16_be/i16_le/i16_be(payload, i) / u24_le /
  // u32_le(payload, i) / bits(payload, i, lsb, width) / len(payload).
  // Bounds-checks the literal offset(s) against the 31-byte payload cap at
  // COMPILE time (brief: "an offset literal beyond 30 is a compile error"),
  // in addition to psvm_run()'s own runtime check against the actual advert
  // length (which may be shorter).
  parseAccessorCall() {
    const nameTok = this.advance()
    const name = nameTok.value
    this.expectPunct('(')
    const buf = this.expectBufferIdent()
    if (name === 'len') {
      // A named GATT buffer has no runtime length to report: slots are
      // fixed-size and zero-padded (never "short"), and a read failure
      // aborts the whole plan before decode ever runs -- so by the time
      // len() would execute, every declared slot is exactly PSVM_PLAN_SLOT
      // bytes. PAYLOAD_LEN also has no buffer operand: it reports the
      // WHOLE 64-byte concatenated working buffer's length, not any one
      // slot's, so `len(temp)` would silently compare against 64 rather
      // than a length that means anything -- a guard that never passes and
      // a wrapper that silently emits nothing. Reject at compile time.
      if (this.buffers) {
        throw new PSError(
          `len(${buf.name}) is not available in a connect wrapper: a named GATT buffer has no runtime length -- each slot is fixed-size and zero-padded, and a failed read aborts the plan before decode runs, so a length check is neither knowable at compile time nor needed at runtime`,
          nameTok.line, nameTok.col
        )
      }
      this.expectPunct(')')
      return { type: 'plen', line: nameTok.line, col: nameTok.col }
    }
    this.expectPunct(',')
    const offTok = this.expectIntLiteral()
    if (name === 'bits') {
      this.expectPunct(',')
      const lsbTok = this.expectIntLiteral()
      this.expectPunct(',')
      const widthTok = this.expectIntLiteral()
      this.expectPunct(')')
      if (widthTok.value < 1 || widthTok.value > 32 || lsbTok.value + widthTok.value > 32) {
        throw new PSError(
          `bits(): lsb+width must be between 1 and 32 bits, got lsb=${lsbTok.value} width=${widthTok.value}`,
          lsbTok.line, lsbTok.col
        )
      }
      const need = Math.ceil((lsbTok.value + widthTok.value) / 8)
      // Bound to the 16-byte slot at COMPILE time (brief: "the whole reason
      // slots are fixed" -- a runtime bounds error would only say "out of
      // range", with no hint which buffer, since the VM has no buffer names).
      if (this.buffers) {
        if (offTok.value + need > PSVM_PLAN_SLOT) {
          throw new PSError(
            `offset ${offTok.value} (width ${need}) is outside buffer '${buf.name}' (each connect-block slot is ${PSVM_PLAN_SLOT} bytes)`,
            offTok.line, offTok.col
          )
        }
        this.noteBufMin(buf.name, offTok.value + need)
        return {
          type: 'bits', offset: buf.slotOffset + offTok.value, lsb: lsbTok.value, width: widthTok.value,
          line: nameTok.line, col: nameTok.col,
        }
      }
      if (offTok.value + need > 31) {
        throw new PSError(
          `offset ${offTok.value} out of range for bits() (payload is at most 31 bytes)`,
          offTok.line, offTok.col
        )
      }
      return {
        type: 'bits', offset: offTok.value, lsb: lsbTok.value, width: widthTok.value,
        line: nameTok.line, col: nameTok.col,
      }
    }
    this.expectPunct(')')
    const size = ACCESSOR_WIDTH[name]
    if (this.buffers) {
      if (offTok.value + size > PSVM_PLAN_SLOT) {
        throw new PSError(
          `offset ${offTok.value} (width ${size}) is outside buffer '${buf.name}' (each connect-block slot is ${PSVM_PLAN_SLOT} bytes)`,
          offTok.line, offTok.col
        )
      }
      this.noteBufMin(buf.name, offTok.value + size)
      return { type: 'load', accessor: name, offset: buf.slotOffset + offTok.value, line: nameTok.line, col: nameTok.col }
    }
    if (offTok.value + size > 31) {
      throw new PSError(
        `offset ${offTok.value} out of range for ${name}() (payload is at most 31 bytes)`,
        offTok.line, offTok.col
      )
    }
    return { type: 'load', accessor: name, offset: offTok.value, line: nameTok.line, col: nameTok.col }
  }

  // pct(<expr>) -- identity at the codegen level (spec section 3: "pct(x)
  // ... reuse[s] M1's numeric ops", i.e. no dedicated opcode; capability
  // range/clamping is capability_encode()'s job downstream, same as any
  // other emitted value).
  parsePct() {
    const t = this.advance() // 'pct'
    this.expectPunct('(')
    const operand = this.parseOr()
    this.expectPunct(')')
    return { type: 'pct', operand, line: t.line, col: t.col }
  }

  parseDuration(clauseName, min, max) {
    const t = this.peek()
    if (t.type !== 'NUMBER') {
      throw new PSError(`expected duration after '${clauseName}'`, t.line, t.col)
    }
    this.advance()
    const unit = t.unit
    if (!unit || !(unit in DURATION_UNITS)) {
      throw new PSError(
        `invalid duration '${t.value}${unit || ''}' (use Ns, Nm/Nmin, or Nh)`,
        t.line, t.col
      )
    }
    const seconds = Math.round(t.value * DURATION_UNITS[unit])
    if (seconds < min || seconds > max) {
      throw new PSError(
        `${clauseName} must be between ${min}s and ${max}s`,
        t.line, t.col
      )
    }
    return seconds
  }

  parseActions() {
    const actions = []
    for (;;) {
      const nameTok = this.expectIdent()
      if (!ACTIONS.has(nameTok.value)) {
        throw new PSError(`unknown action '${nameTok.value}'`, nameTok.line, nameTok.col)
      }
      this.expectPunct('(')
      const argTok = this.expectString()
      const arg = buildStringAst(argTok)
      this.expectPunct(')')
      actions.push({ name: nameTok.value, arg, line: nameTok.line, col: nameTok.col })
      if (this.isPunct(';')) { this.advance(); continue }
      break
    }
    return actions
  }

  // ---- expressions (or -> and -> not -> cmp -> shift -> add -> mul -> unary -> primary) ----

  parseOr() {
    let left = this.parseAnd()
    while (this.isKeyword('or')) {
      const t = this.advance()
      const right = this.parseAnd()
      left = { type: 'or', left, right, line: t.line, col: t.col }
    }
    return left
  }

  parseAnd() {
    let left = this.parseNot()
    while (this.isKeyword('and')) {
      const t = this.advance()
      const right = this.parseNot()
      left = { type: 'and', left, right, line: t.line, col: t.col }
    }
    return left
  }

  parseNot() {
    if (this.isKeyword('not')) {
      const t = this.advance()
      const operand = this.parseNot()
      return { type: 'not', operand, line: t.line, col: t.col }
    }
    return this.parseCmp()
  }

  parseCmp() {
    const left = this.parseShift()
    const t = this.peek()
    if (t.type === 'PUNCT' && CMP_OPS.has(t.value)) {
      this.advance()
      const right = this.parseShift()
      checkUnitMatch(left, right, t)
      return { type: 'cmp', op: t.value, left, right, line: t.line, col: t.col }
    }
    return left
  }

  // `>>` (dialect-agnostic, but only the wrapper dialect's grammar produces
  // it in practice -- extracting a sub-field from a payload accessor's
  // result, e.g. `u16_be(payload, 13) >> 5`). Right operand must be a
  // compile-time integer literal in 0..31. Spec §3 (as amended): `>>` is
  // BIT-EXACT -- codegen compiles `x >> n` to `x / 2^n` followed by FLOOR
  // (see codegen.js's 'shr' case and psvm.c's 0x6C), and a negative `x` at
  // runtime is an error rather than a silently platform-dependent
  // arithmetic shift. `bits(payload, i, lsb, width)` remains the clearer,
  // preferred way to express a field extraction directly.
  parseShift() {
    let left = this.parseAdd()
    while (this.isPunct('>>')) {
      const t = this.advance()
      const amtTok = this.peek()
      if (amtTok.type !== 'NUMBER' || amtTok.unit || !Number.isInteger(amtTok.value) ||
          amtTok.value < 0 || amtTok.value > 31) {
        throw new PSError('right-shift amount must be an integer literal between 0 and 31', amtTok.line, amtTok.col)
      }
      this.advance()
      left = { type: 'shr', operand: left, amount: amtTok.value, line: t.line, col: t.col }
    }
    return left
  }

  parseAdd() {
    let left = this.parseMul()
    while (this.isPunct('+') || this.isPunct('-')) {
      const t = this.advance()
      const right = this.parseMul()
      left = { type: 'binop', op: t.value, left, right, line: t.line, col: t.col }
    }
    return left
  }

  parseMul() {
    let left = this.parseUnary()
    while (this.isPunct('*') || this.isPunct('/')) {
      const t = this.advance()
      const right = this.parseUnary()
      left = { type: 'binop', op: t.value, left, right, line: t.line, col: t.col }
    }
    return left
  }

  parseUnary() {
    if (this.isPunct('-')) {
      const t = this.advance()
      const operand = this.parseUnary()
      return { type: 'unary', op: '-', operand, line: t.line, col: t.col }
    }
    return this.parsePrimary()
  }

  parsePrimary() {
    const t = this.peek()
    if (t.type === 'PUNCT' && t.value === '(') {
      this.advance()
      const expr = this.parseOr()
      this.expectPunct(')')
      return expr
    }
    if (t.type === 'NUMBER') {
      this.advance()
      return { type: 'num', value: t.value, unit: t.unit, line: t.line, col: t.col }
    }
    if (t.type === 'STRING') {
      this.advance()
      return buildStringAst(t)
    }
    if (this.dialect === 'wrapper' && t.type === 'IDENT' && ACCESSOR_NAMES.has(t.value)) {
      return this.parseAccessorCall()
    }
    if (this.dialect === 'wrapper' && t.type === 'IDENT' && t.value === 'pct') {
      return this.parsePct()
    }
    if (this.dialect !== 'wrapper' && t.type === 'IDENT' && (t.value === 'plant' || t.value === 'device')) {
      return this.parseRef()
    }
    throw new PSError(`unexpected token '${describeToken(t)}'`, t.line, t.col)
  }

  parseRef() {
    const kindTok = this.advance() // 'plant' | 'device'
    const kind = kindTok.value === 'plant' ? 0 : 1
    this.expectPunct('(')
    const nameTok = this.expectString()
    const name = plainStringValue(nameTok)
    this.expectPunct(')')
    this.expectPunct('.')
    const seg1 = this.expectIdent()
    this.expectPunct('.')
    const seg2 = this.expectIdent()
    const capability = `${seg1.value}.${seg2.value}`
    if (!CAPS[capability]) {
      throw new PSError(`unknown capability '${capability}'`, seg1.line, seg1.col)
    }
    let field = 'value'
    if (this.isPunct('.') && this.peek(1).type === 'IDENT' && this.peek(1).value === 'age') {
      this.advance()
      this.advance()
      field = 'age'
    }
    return { type: 'ref', kind, name, capability, field, line: kindTok.line, col: kindTok.col }
  }
}

function describeToken(t) {
  if (t.type === 'EOF') return '<eof>'
  if (t.type === 'STRING') return '<string>'
  if (t.type === 'NUMBER') return String(t.value)
  if (t.type === 'HEX') return `0x${t.raw}`
  return String(t.value)
}

function plainStringValue(tok) {
  if (tok.parts.length !== 1 || tok.parts[0].type !== 'lit') {
    throw new PSError('string must not contain interpolation here', tok.line, tok.col)
  }
  return tok.parts[0].value
}

// Converts a lexer STRING token into a 'str' AST node, recursively parsing
// each `{expr}` part's nested token stream as an expression.
function buildStringAst(tok) {
  const parts = tok.parts.map((p) => {
    if (p.type === 'lit') return { type: 'lit', value: p.value }
    // p.tokens already ends in EOF (it's a full tokenize() result).
    const sub = new Parser(p.tokens)
    const ast = sub.parseOr()
    const trailing = sub.peek()
    if (trailing.type !== 'EOF') {
      throw new PSError(`unexpected token '${describeToken(trailing)}' in interpolation`, trailing.line, trailing.col)
    }
    return { type: 'expr', ast }
  })
  return { type: 'str', parts, line: tok.line, col: tok.col }
}

function isBareValueRef(node) {
  return node.type === 'ref' && node.field === 'value'
}

// Unit check (brief): a comparison whose one side is a bare ref and the
// other a unit-suffixed literal must match the ref's capability unit.
function checkUnitMatch(left, right, opTok) {
  let refNode = null
  let numNode = null
  if (isBareValueRef(left) && right.type === 'num' && right.unit) {
    refNode = left; numNode = right
  } else if (isBareValueRef(right) && left.type === 'num' && left.unit) {
    refNode = right; numNode = left
  } else {
    return
  }
  const cap = CAPS[refNode.capability]
  if (!cap.aliases.includes(numNode.unit)) {
    throw new PSError(
      `unit mismatch: '${numNode.unit}' does not match ${refNode.capability}'s unit (${cap.unit})`,
      numNode.line, numNode.col
    )
  }
}

export function parse(tokens) {
  const p = new Parser(tokens)
  return p.parseRuleFile()
}

export function parseWrapper(tokens) {
  const p = new Parser(tokens, 'wrapper')
  return p.parseWrapperFile()
}
