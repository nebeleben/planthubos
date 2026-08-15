// PlantScript lexer (spec section 1). Produces a flat token stream ending in
// an EOF token. String interpolation (`{expr}`) is lexed by capturing the
// raw span between matching braces, un-escaping it the same way the
// enclosing string literal is un-escaped, and recursively tokenizing that
// clean substring into its own independent token stream (so a nested ref's
// string argument can be written either as `plant("X")` or, since it is
// still lexically inside the outer string literal, `plant(\"X\")`).

export class PSError extends Error {
  constructor(message, line, col) {
    super(message)
    this.name = 'PSError'
    this.line = line
    this.col = col
  }
}

const KEYWORDS = new Set([
  'rule', 'when', 'then', 'mode', 'cooldown', 'every', 'and', 'or', 'not',
])

const UNIT_CHAR = /[A-Za-z%°]/
const IDENT_START = /[A-Za-z_]/
const IDENT_PART = /[A-Za-z0-9_]/
const DIGIT = /[0-9]/

const TWO_CHAR_PUNCT = new Set(['<=', '>=', '==', '!='])
const ONE_CHAR_PUNCT = new Set(['(', ')', '.', ',', ';', '<', '>', '+', '-', '*', '/'])

function unescape(s) {
  let out = ''
  for (let i = 0; i < s.length; i++) {
    if (s[i] === '\\' && i + 1 < s.length) { out += s[i + 1]; i++ }
    else out += s[i]
  }
  return out
}

export function tokenize(source) {
  let i = 0
  let line = 1
  let col = 1

  function peek(offset = 0) {
    return source[i + offset]
  }

  function advance() {
    const c = source[i++]
    if (c === '\n') { line++; col = 1 } else { col++ }
    return c
  }

  function skipWhitespaceAndComments() {
    for (;;) {
      const c = peek()
      if (c === undefined) return
      if (c === ' ' || c === '\t' || c === '\r' || c === '\n') { advance(); continue }
      if (c === '#') {
        while (peek() !== undefined && peek() !== '\n') advance()
        continue
      }
      return
    }
  }

  function readNumber(startLine, startCol) {
    let s = ''
    while (peek() !== undefined && DIGIT.test(peek())) s += advance()
    if (peek() === '.' && DIGIT.test(peek(1))) {
      s += advance()
      while (peek() !== undefined && DIGIT.test(peek())) s += advance()
    }
    let unit = null
    if (peek() !== undefined && UNIT_CHAR.test(peek())) {
      let u = ''
      while (peek() !== undefined && UNIT_CHAR.test(peek())) u += advance()
      unit = u
    }
    return { type: 'NUMBER', value: parseFloat(s), unit, line: startLine, col: startCol }
  }

  // Captures raw text up to the matching unescaped '}' (braces may nest;
  // '\' escapes the following character so `\"`/`\\`/`\}` survive intact),
  // un-escapes it, and tokenizes the result as an independent expression.
  function readInterpolationSpan(strLine, strCol) {
    let depth = 1
    let raw = ''
    for (;;) {
      const c = peek()
      if (c === undefined) throw new PSError('unterminated interpolation', strLine, strCol)
      if (c === '\\') {
        raw += advance()
        if (peek() !== undefined) raw += advance()
        continue
      }
      if (c === '{') { depth++; raw += advance(); continue }
      if (c === '}') {
        depth--
        advance()
        if (depth === 0) break
        raw += '}'
        continue
      }
      raw += advance()
    }
    return tokenize(unescape(raw))
  }

  function readStringToken() {
    const startLine = line
    const startCol = col
    advance() // opening quote
    const parts = []
    let lit = ''
    for (;;) {
      const c = peek()
      if (c === undefined || c === '\n') {
        throw new PSError('unterminated string', startLine, startCol)
      }
      if (c === '"') { advance(); break }
      if (c === '\\') {
        advance()
        const esc = peek()
        if (esc === undefined) throw new PSError('unterminated string', startLine, startCol)
        if (esc === '"') { lit += '"'; advance() }
        else if (esc === '\\') { lit += '\\'; advance() }
        else if (esc === 'n') { lit += '\n'; advance() }
        else { lit += esc; advance() }
        continue
      }
      if (c === '{') {
        advance()
        parts.push({ type: 'lit', value: lit })
        lit = ''
        parts.push({ type: 'expr', tokens: readInterpolationSpan(startLine, startCol) })
        continue
      }
      lit += advance()
    }
    parts.push({ type: 'lit', value: lit })
    return { type: 'STRING', parts, line: startLine, col: startCol }
  }

  function readToken() {
    skipWhitespaceAndComments()
    const startLine = line
    const startCol = col
    const c = peek()
    if (c === undefined) return { type: 'EOF', value: null, line: startLine, col: startCol }
    if (c === '"') return readStringToken()
    if (DIGIT.test(c) || (c === '.' && DIGIT.test(peek(1)))) return readNumber(startLine, startCol)
    if (IDENT_START.test(c)) {
      let s = ''
      while (peek() !== undefined && IDENT_PART.test(peek())) s += advance()
      const type = KEYWORDS.has(s) ? 'KEYWORD' : 'IDENT'
      return { type, value: s, line: startLine, col: startCol }
    }
    const two = source.slice(i, i + 2)
    if (TWO_CHAR_PUNCT.has(two)) {
      advance(); advance()
      return { type: 'PUNCT', value: two, line: startLine, col: startCol }
    }
    if (ONE_CHAR_PUNCT.has(c)) {
      advance()
      return { type: 'PUNCT', value: c, line: startLine, col: startCol }
    }
    throw new PSError(`unexpected character '${c}'`, startLine, startCol)
  }

  const tokens = []
  for (;;) {
    const tok = readToken()
    tokens.push(tok)
    if (tok.type === 'EOF') break
  }
  return tokens
}
