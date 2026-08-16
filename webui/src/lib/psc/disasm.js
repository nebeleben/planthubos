// Disassembler: reverse-walks PSBC v1 bytecode (spec section 2) into one
// mnemonic per line, with ref/const annotations, for tests and the Rules
// tab's compile-error/debug view.

import { OPCODES } from './codegen.js'
import { CAPS_BY_ID } from './caps.js'

const OPNAME = Object.fromEntries(Object.entries(OPCODES).map(([k, v]) => [v, k]))
const BUILTIN_NAME = { 0: 'log', 1: 'notify' }

const HEADER_LEN = 18

function readConsts(view, bytes, offset, count) {
  const consts = []
  let o = offset
  for (let i = 0; i < count; i++) {
    const tag = bytes[o]
    if (tag === 0) {
      consts.push({ tag, value: view.getFloat32(o + 1, true) })
      o += 5
    } else {
      const len = view.getUint16(o + 1, true)
      const strBytes = bytes.subarray(o + 3, o + 3 + len)
      consts.push({ tag, value: new TextDecoder().decode(strBytes) })
      o += 3 + len
    }
  }
  return { consts, offset: o }
}

function readRefs(bytes, offset, count) {
  const refs = []
  let o = offset
  for (let i = 0; i < count; i++) {
    refs.push({
      kind: bytes[o],
      nameConstIdx: bytes[o + 1] | (bytes[o + 2] << 8),
      capability: bytes[o + 3],
      field: bytes[o + 4],
    })
    o += 5
  }
  return { refs, offset: o }
}

function fmtConst(c) {
  if (!c) return '?'
  return c.tag === 0 ? String(c.value) : JSON.stringify(c.value)
}

function fmtRef(r, consts) {
  if (!r) return '?'
  const capName = CAPS_BY_ID[r.capability] ? CAPS_BY_ID[r.capability].name : `cap${r.capability}`
  const name = consts[r.nameConstIdx] ? consts[r.nameConstIdx].value : '?'
  const kindName = r.kind === 0 ? 'plant' : 'device'
  return `${kindName} "${name}" ${capName}${r.field === 1 ? '.age' : ''}`
}

export function disassemble(bytecode) {
  const bytes = bytecode instanceof Uint8Array ? bytecode : new Uint8Array(bytecode)
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)

  const constCount = view.getUint16(12, true)
  const refCount = view.getUint16(14, true)
  const codeLen = view.getUint16(16, true)

  let offset = HEADER_LEN
  const consts = readConsts(view, bytes, offset, constCount)
  offset = consts.offset
  const refs = readRefs(bytes, offset, refCount)
  offset = refs.offset
  const codeStart = offset

  const lines = []
  let pc = 0
  while (pc < codeLen) {
    const addr = pc.toString().padStart(4, '0')
    const op = bytes[codeStart + pc]
    const name = OPNAME[op] || `UNKNOWN(0x${op.toString(16)})`
    switch (op) {
      case OPCODES.PUSH_CONST: {
        const idx = view.getUint16(codeStart + pc + 1, true)
        lines.push(`${addr}: PUSH_CONST ${idx} ; ${fmtConst(consts.consts[idx])}`)
        pc += 3
        break
      }
      case OPCODES.LOAD_REF: {
        const idx = view.getUint16(codeStart + pc + 1, true)
        lines.push(`${addr}: LOAD_REF ${idx} ; ${fmtRef(refs.refs[idx], consts.consts)}`)
        pc += 3
        break
      }
      case OPCODES.JZ:
      case OPCODES.JMP: {
        const off = view.getInt16(codeStart + pc + 1, true)
        lines.push(`${addr}: ${name} ${off}`)
        pc += 3
        break
      }
      case OPCODES.BUILD_STR: {
        const n = bytes[codeStart + pc + 1]
        lines.push(`${addr}: BUILD_STR ${n}`)
        pc += 2
        break
      }
      case OPCODES.CALL_BUILTIN: {
        const b = bytes[codeStart + pc + 1]
        lines.push(`${addr}: CALL_BUILTIN ${BUILTIN_NAME[b] !== undefined ? BUILTIN_NAME[b] : b}`)
        pc += 2
        break
      }
      // ---- wrapper dialect (M3 spec section 3) ----
      case OPCODES.LOAD_U8:
      case OPCODES.LOAD_U16LE:
      case OPCODES.LOAD_U16BE:
      case OPCODES.LOAD_I16LE:
      case OPCODES.LOAD_I16BE:
      case OPCODES.LOAD_U24LE:
      case OPCODES.LOAD_U32LE: {
        const off = view.getUint16(codeStart + pc + 1, true)
        lines.push(`${addr}: ${name} ${off}`)
        pc += 3
        break
      }
      case OPCODES.LOAD_BITS: {
        const off = view.getUint16(codeStart + pc + 1, true)
        const lsb = bytes[codeStart + pc + 3]
        const width = bytes[codeStart + pc + 4]
        lines.push(`${addr}: LOAD_BITS ${off} ${lsb} ${width}`)
        pc += 5
        break
      }
      case OPCODES.EMIT: {
        const cap = bytes[codeStart + pc + 1]
        const capName = CAPS_BY_ID[cap] ? CAPS_BY_ID[cap].name : `cap${cap}`
        lines.push(`${addr}: EMIT ${capName}`)
        pc += 2
        break
      }
      case OPCODES.HALT_BOOL:
      case OPCODES.HALT:
      case OPCODES.ADD:
      case OPCODES.SUB:
      case OPCODES.MUL:
      case OPCODES.DIV:
      case OPCODES.LT:
      case OPCODES.LE:
      case OPCODES.GT:
      case OPCODES.GE:
      case OPCODES.EQ:
      case OPCODES.NE:
      case OPCODES.AND:
      case OPCODES.OR:
      case OPCODES.NOT:
      case OPCODES.PAYLOAD_LEN:
      case OPCODES.REQUIRE:
      case OPCODES.AES_CCM:
      case OPCODES.FLOOR:
        lines.push(`${addr}: ${name}`)
        pc += 1
        break
      default:
        // Unknown opcode: stop decoding rather than loop forever.
        lines.push(`${addr}: ${name}`)
        return lines.join('\n')
    }
  }
  return lines.join('\n')
}
