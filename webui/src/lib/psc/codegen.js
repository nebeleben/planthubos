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
  HALT: 0xFF,
}

const CMP_OPCODE = { '<': OPCODES.LT, '<=': OPCODES.LE, '>': OPCODES.GT, '>=': OPCODES.GE, '==': OPCODES.EQ, '!=': OPCODES.NE }
const ARITH_OPCODE = { '+': OPCODES.ADD, '-': OPCODES.SUB, '*': OPCODES.MUL, '/': OPCODES.DIV }
const BUILTIN_ID = { log: 0, notify: 1 }

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
