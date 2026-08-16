// PSBC v1 codegen (spec section 2 — byte layout is the ground truth mirrored
// here; planthubos/components/psvm/psvm.c is what ultimately validates it).
//
// and/or compile to EAGER AND/OR opcodes (not JZ/JMP short-circuit): both
// operand kinds in M1 are side-effect-free reads, so short-circuiting buys
// nothing but jump-offset bookkeeping.

import { CAPS } from './caps.js'

export const OPCODES = {
  HALT_BOOL: 0x00,
  PUSH_CONST: 0x01,
  LOAD_REF: 0x02,
  ADD: 0x10,
  SUB: 0x11,
  MUL: 0x12,
  DIV: 0x13,
  LT: 0x20,
  LE: 0x21,
  GT: 0x22,
  GE: 0x23,
  EQ: 0x24,
  NE: 0x25,
  AND: 0x30,
  OR: 0x31,
  NOT: 0x32,
  JZ: 0x40,
  JMP: 0x41,
  BUILD_STR: 0x50,
  CALL_BUILTIN: 0x51,
  // Wrapper dialect (M3 spec section 3), dialect=2. Appended after M1's
  // table (0x00-0x51, 0xFF); do not renumber the opcodes above.
  LOAD_U8: 0x60,
  LOAD_U16LE: 0x61,
  LOAD_U16BE: 0x62,
  LOAD_I16LE: 0x63,
  LOAD_I16BE: 0x64,
  LOAD_U24LE: 0x65,
  LOAD_U32LE: 0x66,
  LOAD_BITS: 0x67,
  PAYLOAD_LEN: 0x68,
  EMIT: 0x69,
  REQUIRE: 0x6A,
  AES_CCM: 0x6B,
  // FLOOR (0x6C, spec §3 as amended): pops a number, pushes floor(x);
  // negative x is a runtime error (PSVM_ERR_TYPE). Used by `>>`'s codegen
  // below so the shift idiom is bit-exact rather than leaving DIV's
  // fractional remainder.
  FLOOR: 0x6C,
  HALT: 0xFF,
}

const CMP_OPCODE = { '<': OPCODES.LT, '<=': OPCODES.LE, '>': OPCODES.GT, '>=': OPCODES.GE, '==': OPCODES.EQ, '!=': OPCODES.NE }
const ARITH_OPCODE = { '+': OPCODES.ADD, '-': OPCODES.SUB, '*': OPCODES.MUL, '/': OPCODES.DIV }
const BUILTIN_ID = { log: 0, notify: 1 }
// name -> LOAD_* opcode, for the fixed-width single-offset accessors
// (u8..u32_le). 'bits' and 'len' compile via their own emitWrapperExpr cases.
const ACCESSOR_OPCODE = {
  u8: OPCODES.LOAD_U8, u16_le: OPCODES.LOAD_U16LE, u16_be: OPCODES.LOAD_U16BE,
  i16_le: OPCODES.LOAD_I16LE, i16_be: OPCODES.LOAD_I16BE,
  u24_le: OPCODES.LOAD_U24LE, u32_le: OPCODES.LOAD_U32LE,
}

class ByteWriter {
  constructor() { this.bytes = [] }
  u8(v) { this.bytes.push(v & 0xFF) }
  u16(v) { this.u8(v & 0xFF); this.u8((v >> 8) & 0xFF) }
  u32(v) { this.u8(v & 0xFF); this.u8((v >>> 8) & 0xFF); this.u8((v >>> 16) & 0xFF); this.u8((v >>> 24) & 0xFF) }
  f32(v) {
    const buf = new ArrayBuffer(4)
    new DataView(buf).setFloat32(0, v, true)
    this.raw(new Uint8Array(buf))
  }
  raw(arr) { for (const b of arr) this.bytes.push(b) }
  str(s) {
    const enc = new TextEncoder().encode(s)
    this.u16(enc.length)
    this.raw(enc)
  }
  toUint8Array() { return new Uint8Array(this.bytes) }
}

// Code-section byte buffer: opcodes only ever need u8/u16 operands here.
class CodeBuf {
  constructor() { this.bytes = [] }
  op(o) { this.bytes.push(o & 0xFF) }
  u8(v) { this.bytes.push(v & 0xFF) }
  u16(v) { this.bytes.push(v & 0xFF, (v >> 8) & 0xFF) }
}

class ConstPool {
  constructor() { this.entries = [] }
  addNum(v) {
    const idx = this.entries.findIndex((e) => e.tag === 0 && e.value === v)
    if (idx >= 0) return idx
    this.entries.push({ tag: 0, value: v })
    return this.entries.length - 1
  }
  addStr(s) {
    const idx = this.entries.findIndex((e) => e.tag === 1 && e.value === s)
    if (idx >= 0) return idx
    this.entries.push({ tag: 1, value: s })
    return this.entries.length - 1
  }
}

class RefTable {
  constructor(consts) { this.entries = []; this.consts = consts }
  add(kind, name, capability, field) {
    const capId = CAPS[capability].id
    const fieldId = field === 'age' ? 1 : 0
    const idx = this.entries.findIndex(
      (e) => e.kind === kind && e.name === name && e.capability === capId && e.field === fieldId
    )
    if (idx >= 0) return idx
    const nameConstIdx = this.consts.addStr(name)
    this.entries.push({ kind, name, capability: capId, field: fieldId, nameConstIdx })
    return this.entries.length - 1
  }
}

function emitExpr(node, buf, ctx) {
  switch (node.type) {
    case 'num': {
      const idx = ctx.consts.addNum(node.value)
      buf.op(OPCODES.PUSH_CONST); buf.u16(idx)
      return
    }
    case 'ref': {
      const idx = ctx.refs.add(node.kind, node.name, node.capability, node.field)
      buf.op(OPCODES.LOAD_REF); buf.u16(idx)
      return
    }
    case 'unary': {
      // No dedicated NEG opcode in PSBC v1: -x compiles to (0 - x).
      const zeroIdx = ctx.consts.addNum(0)
      buf.op(OPCODES.PUSH_CONST); buf.u16(zeroIdx)
      emitExpr(node.operand, buf, ctx)
      buf.op(OPCODES.SUB)
      return
    }
    case 'binop':
      emitExpr(node.left, buf, ctx)
      emitExpr(node.right, buf, ctx)
      buf.op(ARITH_OPCODE[node.op])
      return
    case 'shr': {
      // `>>` shares the or/and/not/cmp/shift/add/mul/unary precedence chain
      // with the rules dialect (parser.js), but there is no dedicated shift
      // opcode in M1's table -- x >> n compiles to x / 2^n (DIV) then FLOOR
      // (spec §3 as amended), matching a real bit-shift exactly rather than
      // leaving DIV's fractional remainder: e.g. 44086 >> 5 must be 1377,
      // not 1377.6875. bits() is still the recommended, always-bit-exact
      // idiom for extracting a sub-byte field directly; see the
      // wrapper-dialect 'shr' case below (identical codegen).
      emitExpr(node.operand, buf, ctx)
      const idx = ctx.consts.addNum(2 ** node.amount)
      buf.op(OPCODES.PUSH_CONST); buf.u16(idx)
      buf.op(OPCODES.DIV)
      buf.op(OPCODES.FLOOR)
      return
    }
    case 'cmp':
      emitExpr(node.left, buf, ctx)
      emitExpr(node.right, buf, ctx)
      buf.op(CMP_OPCODE[node.op])
      return
    case 'and':
      emitExpr(node.left, buf, ctx)
      emitExpr(node.right, buf, ctx)
      buf.op(OPCODES.AND)
      return
    case 'or':
      emitExpr(node.left, buf, ctx)
      emitExpr(node.right, buf, ctx)
      buf.op(OPCODES.OR)
      return
    case 'not':
      emitExpr(node.operand, buf, ctx)
      buf.op(OPCODES.NOT)
      return
    case 'str':
      emitStringNode(node, buf, ctx)
      return
    default:
      throw new Error(`codegen: unhandled node type '${node.type}'`)
  }
}

// A string with no interpolation pushes its constant directly (already a
// V_STR on the VM stack). Anything with parts (interpolated numbers/refs,
// or 2+ literal/expr parts) pushes each part then BUILD_STR n.
function emitStringNode(node, buf, ctx) {
  const parts = node.parts.filter((p) => !(p.type === 'lit' && p.value === ''))
  if (parts.length === 0) {
    const idx = ctx.consts.addStr('')
    buf.op(OPCODES.PUSH_CONST); buf.u16(idx)
    return
  }
  if (parts.length === 1 && parts[0].type === 'lit') {
    const idx = ctx.consts.addStr(parts[0].value)
    buf.op(OPCODES.PUSH_CONST); buf.u16(idx)
    return
  }
  for (const p of parts) {
    if (p.type === 'lit') {
      const idx = ctx.consts.addStr(p.value)
      buf.op(OPCODES.PUSH_CONST); buf.u16(idx)
    } else {
      emitExpr(p.ast, buf, ctx)
    }
  }
  buf.op(OPCODES.BUILD_STR); buf.u8(parts.length)
}

export function emit(ast) {
  const consts = new ConstPool()
  const refs = new RefTable(consts)
  const ctx = { consts, refs }

  const whenBuf = new CodeBuf()
  emitExpr(ast.when, whenBuf, ctx)
  whenBuf.op(OPCODES.HALT_BOOL)

  const thenBuf = new CodeBuf()
  let builtins = 0
  for (const action of ast.actions) {
    emitStringNode(action.arg, thenBuf, ctx)
    thenBuf.op(OPCODES.CALL_BUILTIN)
    const bid = BUILTIN_ID[action.name]
    thenBuf.u8(bid)
    builtins |= (1 << bid)
  }
  thenBuf.op(OPCODES.HALT)

  const code = [...whenBuf.bytes, ...thenBuf.bytes]

  const w = new ByteWriter()
  w.raw([0x50, 0x53, 0x42, 0x43]) // "PSBC"
  w.u8(1) // fmt_ver
  w.u8(1) // dialect (rules)
  w.u16(0) // flags — psvm.c rejects nonzero
  w.u32(builtins)
  w.u16(consts.entries.length)
  w.u16(refs.entries.length)
  w.u16(code.length)
  for (const c of consts.entries) {
    if (c.tag === 0) { w.u8(0); w.f32(c.value) }
    else { w.u8(1); w.str(c.value) }
  }
  for (const r of refs.entries) {
    w.u8(r.kind); w.u16(r.nameConstIdx); w.u8(r.capability); w.u8(r.field)
  }
  w.raw(code)

  return {
    bytecode: w.toUint8Array(),
    refs: refs.entries.map((r) => ({ kind: r.kind, name: r.name, capability: r.capability, field: r.field })),
  }
}

// ---- wrapper dialect (M3 spec section 3) ----
// Same expression precedence as emitExpr, extended with the payload
// accessors and pct(); no LOAD_REF ever appears (wrapper bytecode always
// has ref_count=0) and no string ops either (no log/notify in this
// dialect), so string-shaped rule AST nodes are simply not reachable here.
function emitWrapperExpr(node, buf, ctx) {
  switch (node.type) {
    case 'num': {
      const idx = ctx.consts.addNum(node.value)
      buf.op(OPCODES.PUSH_CONST); buf.u16(idx)
      return
    }
    case 'unary': {
      const zeroIdx = ctx.consts.addNum(0)
      buf.op(OPCODES.PUSH_CONST); buf.u16(zeroIdx)
      emitWrapperExpr(node.operand, buf, ctx)
      buf.op(OPCODES.SUB)
      return
    }
    case 'shr': {
      // Bit-exact: DIV then FLOOR (spec §3 as amended). bits() remains the
      // recommended idiom for a sub-byte field extracted directly.
      emitWrapperExpr(node.operand, buf, ctx)
      const idx = ctx.consts.addNum(2 ** node.amount)
      buf.op(OPCODES.PUSH_CONST); buf.u16(idx)
      buf.op(OPCODES.DIV)
      buf.op(OPCODES.FLOOR)
      return
    }
    case 'binop':
      emitWrapperExpr(node.left, buf, ctx)
      emitWrapperExpr(node.right, buf, ctx)
      buf.op(ARITH_OPCODE[node.op])
      return
    case 'cmp':
      emitWrapperExpr(node.left, buf, ctx)
      emitWrapperExpr(node.right, buf, ctx)
      buf.op(CMP_OPCODE[node.op])
      return
    case 'and':
      emitWrapperExpr(node.left, buf, ctx)
      emitWrapperExpr(node.right, buf, ctx)
      buf.op(OPCODES.AND)
      return
    case 'or':
      emitWrapperExpr(node.left, buf, ctx)
      emitWrapperExpr(node.right, buf, ctx)
      buf.op(OPCODES.OR)
      return
    case 'not':
      emitWrapperExpr(node.operand, buf, ctx)
      buf.op(OPCODES.NOT)
      return
    case 'load':
      buf.op(ACCESSOR_OPCODE[node.accessor]); buf.u16(node.offset)
      return
    case 'bits':
      buf.op(OPCODES.LOAD_BITS); buf.u16(node.offset); buf.u8(node.lsb); buf.u8(node.width)
      return
    case 'plen':
      buf.op(OPCODES.PAYLOAD_LEN)
      return
    case 'pct':
      // Identity at codegen level -- see parser.js's parsePct() doc comment.
      emitWrapperExpr(node.operand, buf, ctx)
      return
    default:
      throw new Error(`codegen: unhandled wrapper node type '${node.type}'`)
  }
}

// emitWrapper(ast) -> { bytecode, capsUsed } for a parseWrapper() AST.
// dialect=2, ref_count always 0 (no LOAD_REF in this dialect), builtins
// header bitmap always 0 (no CALL_BUILTIN either -- EMIT/REQUIRE/AES_CCM
// are dedicated opcodes, not builtin calls).
export function emitWrapper(ast) {
  const consts = new ConstPool()
  const ctx = { consts }
  const buf = new CodeBuf()
  const capsUsed = []

  for (const stmt of ast.statements) {
    if (stmt.type === 'require') {
      emitWrapperExpr(stmt.expr, buf, ctx)
      buf.op(OPCODES.REQUIRE)
    } else if (stmt.type === 'emit') {
      emitWrapperExpr(stmt.expr, buf, ctx)
      const capId = CAPS[stmt.capability].id
      buf.op(OPCODES.EMIT); buf.u8(capId)
      if (!capsUsed.includes(capId)) capsUsed.push(capId)
    } else if (stmt.type === 'decrypt') {
      emitWrapperExpr(stmt.offsetExpr, buf, ctx)
      emitWrapperExpr(stmt.lenExpr, buf, ctx)
      buf.op(OPCODES.AES_CCM)
    } else {
      throw new Error(`codegen: unhandled wrapper statement type '${stmt.type}'`)
    }
  }
  buf.op(OPCODES.HALT)

  const w = new ByteWriter()
  w.raw([0x50, 0x53, 0x42, 0x43]) // "PSBC"
  w.u8(1) // fmt_ver
  w.u8(2) // dialect (wrappers)
  w.u16(0) // flags — psvm.c rejects nonzero
  w.u32(0) // builtins — unused by this dialect
  w.u16(consts.entries.length)
  w.u16(0) // ref_count — always 0
  w.u16(buf.bytes.length)
  for (const c of consts.entries) {
    if (c.tag === 0) { w.u8(0); w.f32(c.value) }
    else { w.u8(1); w.str(c.value) }
  }
  // no refs section
  w.raw(buf.bytes)

  return { bytecode: w.toUint8Array(), capsUsed }
}
