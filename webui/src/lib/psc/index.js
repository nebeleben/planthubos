// PlantScript compiler public API (spec section 1 grammar, section 2 PSBC v1).
import { tokenize, PSError } from './lexer.js'
import { parse, parseWrapper } from './parser.js'
import { emit, emitWrapper } from './codegen.js'

export { disassemble } from './disasm.js'
export { CAPS } from './caps.js'

// compile(source) -> {ok:true, name, mode, cooldown_s, every_s, bytecode, refs}
//                  | {ok:false, errors:[{line,col,message}]}
export function compile(source) {
  try {
    const tokens = tokenize(source)
    const ast = parse(tokens)
    const { bytecode, refs } = emit(ast)
    return {
      ok: true,
      name: ast.name,
      mode: ast.mode,
      cooldown_s: ast.cooldown_s,
      every_s: ast.every_s,
      bytecode,
      refs,
    }
  } catch (err) {
    if (err instanceof PSError) {
      return { ok: false, errors: [{ line: err.line, col: err.col, message: err.message }] }
    }
    throw err
  }
}

// compileWrapper(source) -> {ok:true, name, match:{kind,key}, bytecode, capsUsed, plan}
//                         | {ok:false, errors:[{line,col,message}]}
// The wrapper dialect (M3 spec section 3, dialect=2). match.kind matches
// wmatch_kind_t in components/wrappers/include/wrapper_index.h
// (0 service, 1 manufacturer, 2 mac_prefix). `plan` (M5a spec section 2) is
// {intervalS, reads:[{uuid16,name,offset}], writes:[{uuid16,data}]} when the
// wrapper has a `connect` block, else null.
export function compileWrapper(source) {
  try {
    const tokens = tokenize(source)
    const ast = parseWrapper(tokens)
    const { bytecode, capsUsed, plan } = emitWrapper(ast)
    return { ok: true, name: ast.name, match: ast.match, bytecode, capsUsed, plan }
  } catch (err) {
    if (err instanceof PSError) {
      return { ok: false, errors: [{ line: err.line, col: err.col, message: err.message }] }
    }
    throw err
  }
}
