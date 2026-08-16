// Rule generation template, version 1 (M4 spec section 7).
import { redactPlants } from '../redact.js'
import { RULE_DIALECT, CAPABILITIES } from './dialect.js'

export const RULE_TEMPLATE = {
  name: 'rule',
  version: 1,
  build({ plants, request }) {
    const p = redactPlants(plants)
    // An auto-created plant has an empty name (plants_table.c leaves
    // e->name[0] = '\0' until an operator names it) -- redactPlants passes
    // that empty string through unchanged (see its own comment: the
    // redaction layer is not where this gets decided). plant("") is not a
    // compile error: the resolver is a plain strcmp, so it silently binds
    // to the FIRST unnamed plant in table order. Two auto-created plants
    // on a fresh hub would otherwise render as identical inventory lines,
    // and a model asked about either one has no way to write a rule that
    // addresses the one the user meant -- it would compile, install, and
    // fire against whichever unnamed plant happens to be first. So an
    // unnamed plant is never listed as a referenceable entry; the model is
    // told plainly that it exists and why it can't be named.
    const named = p.filter((x) => x.name.trim() !== '')
    const unnamedCount = p.length - named.length
    const namedLines = named.map((x) => {
      const caps = x.capabilities.map((c) => `${c.name} (${c.unit})`).join(', ')
      // No id here: rules reference plants by name string
      // (plant("<name>")), never by id, and "plant <int>" reads as
      // an echo of the non-parsing plant(<id>) form.
      return `  plant "${x.name}": ${caps || 'no capabilities bound'}`
    })
    const unnamedNote = unnamedCount === 0 ? null
      : unnamedCount === 1
        ? '1 plant on this hub has no name and cannot be referenced in a rule until it is named in the Plants tab.'
        : `${unnamedCount} plants on this hub have no name and cannot be referenced in a rule until they are named in the Plants tab.`
    let inventory
    if (p.length === 0) {
      inventory = 'This hub has no plants configured yet.'
    } else if (namedLines.length === 0) {
      inventory = unnamedNote
    } else {
      inventory = unnamedNote ? `${namedLines.join('\n')}\n${unnamedNote}` : namedLines.join('\n')
    }

    const system = `You write PlantScript rules for a plant-monitoring hub.

${RULE_DIALECT}

${CAPABILITIES}

Answer with exactly one fenced code block containing only rule source. No
explanation outside the block. Only reference plants and capabilities that
appear in the list the user gives you -- a rule referencing anything else
will compile and then never fire.`

    const user = `Plants configured on this hub:

${inventory}

Write a rule for this request:

${request}`

    return { system, user }
  },
}
