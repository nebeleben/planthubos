// Wrapper generation template, version 1 (M4 spec section 7). The version
// is part of the generated source's provenance header, so a wrapper that
// later misbehaves can be traced to what produced it.
import { redactUnknownDevice } from '../redact.js'
import { WRAPPER_DIALECT, CAPABILITIES } from './dialect.js'

export const WRAPPER_TEMPLATE = {
  name: 'wrapper',
  version: 1,
  build({ device }) {
    const r = redactUnknownDevice(device)
    const samples = r.samples
      .map((s, i) => `  sample ${i + 1}: ${s.hex}  (${s.len} bytes)`)
      .join('\n')
    const spacing = r.spacingSeconds === null
      ? 'only one sample was captured'
      : `the samples are about ${r.spacingSeconds} seconds apart`
    const oui = r.ouiHex
      ? `The device's vendor OUI is ${r.ouiHex}.`
      : 'The vendor OUI is unknown.'

    const system = `You write PlantScript wrapper programs that decode BLE advertisements into sensor readings.

${WRAPPER_DIALECT}

${CAPABILITIES}

Answer with exactly one fenced code block containing only wrapper source.
No explanation outside the block. If the payload does not look like sensor
data you recognise, still produce your best wrapper and add a source
comment (a line starting with #) saying what you were unsure about.`

    const user = `Here are raw BLE advertisement payloads captured from one unknown device.

${oui}
${spacing}

${samples}

Write a wrapper that decodes whatever sensor values this device reports.
Choose the match kind and key from what the payload shows. Only emit
capabilities from the list above, with values already scaled into the
capability's own unit.`

    return { system, user }
  },
}
