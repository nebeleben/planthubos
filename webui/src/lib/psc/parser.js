// PlantScript recursive-descent parser (spec section 1).
// Precedence, loose to tight: or -> and -> not -> cmp -> add -> mul -> unary -> primary.

import { PSError } from './lexer.js'
import { CAPS } from './caps.js'

const DURATION_UNITS = { s: 1, sec: 1, m: 60, min: 60, h: 3600, hr: 3600 }
const CMP_OPS = new Set(['<', '<=', '>', '>=', '==', '!='])
const MODES = new Set(['edge', 'level'])
const ACTIONS = new Set(['log', 'notify'])

class Parser {
  constructor(tokens) {
    this.tokens = tokens
    this.pos = 0
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

  // ---- expressions (or -> and -> not -> cmp -> add -> mul -> unary -> primary) ----

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
    const left = this.parseAdd()
    const t = this.peek()
    if (t.type === 'PUNCT' && CMP_OPS.has(t.value)) {
      this.advance()
      const right = this.parseAdd()
      checkUnitMatch(left, right, t)
      return { type: 'cmp', op: t.value, left, right, line: t.line, col: t.col }
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
    if (t.type === 'IDENT' && (t.value === 'plant' || t.value === 'device')) {
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
