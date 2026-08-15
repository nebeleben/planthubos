// PlantScript compiler public API (spec section 1 grammar, section 2 PSBC v1).
import { tokenize, PSError } from './lexer.js'
import { parse } from './parser.js'
import { emit } from './codegen.js'

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
