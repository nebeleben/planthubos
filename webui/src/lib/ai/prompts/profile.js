// The device-profile prompt template. Lives in the WebUI (not the hub) so the
// wording can iterate without reflashing firmware (spec §5). Produces a
// system + user pair for an OpenAI-compatible chat-completions call.
export function profilePrompt(inputs) {
  const caps = inputs.caps.map(c => `${c.name} (${c.unit || 'no unit'})`).join(', ')
  const dps = inputs.observed_dps.map(d => `dp_id ${d.dp_id}: value=${d.value} (type ${d.type}, ${d.age_s}s ago)`).join('\n')
  const system = [
    'You map raw Tuya EF00 Zigbee datapoints to a fixed set of capabilities.',
    'Return ONLY a JSON object: {"entries":[{"kind":"dp","dp_id":N,"cap":"<one of the allowed caps>","scale":number,"offset":number}]}.',
    'Use ONLY the allowed cap names. Do not invent caps. If unsure about a datapoint, omit it.',
  ].join(' ')
  const user = [
    `Device fingerprint: manufacturer="${inputs.fingerprint.manufacturer}" model="${inputs.fingerprint.model}".`,
    `Allowed caps: ${caps}.`,
    `Observed datapoints:\n${dps}`,
    'Propose a mapping. Reply with the JSON object only, in a fenced code block.',
  ].join('\n\n')
  return { system, user }
}
