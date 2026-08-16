// Rule generation template, version 1 (M4 spec section 7).
import { redactPlants } from '../redact.js'
import { RULE_DIALECT, CAPABILITIES } from './dialect.js'

export const RULE_TEMPLATE = {
  name: 'rule',
  version: 1,
  build({ plants, request }) {
    const p = redactPlants(plants)
    const inventory = p.length === 0
      ? 'This hub has no plants configured yet.'
      : p.map((x) => {
          const caps = x.capabilities.map((c) => `${c.name} (${c.unit})`).join(', ')
          // No id here: rules reference plants by name string
          // (plant("<name>")), never by id, and "plant <int>" reads as
          // an echo of the non-parsing plant(<id>) form.
          return `  plant "${x.name}": ${caps || 'no capabilities bound'}`
        }).join('\n')

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
