// Pure formatting/parsing helpers for the M5b actuator-controls + alert-feed
// UI (Task 12). No DOM, no fetch -- this codebase's browser tests cover
// logic, not rendering (src/lib/psc/tests's existing convention), so every
// branch a control's wording depends on lives here where it is testable in
// isolation. Consumed by devices.jsx (actor controls, guard text) and
// alerts.jsx (the event feed).

// Guard shape: { cooldownS, lastFiredAtS }. lastFiredAtS is an ABSOLUTE
// epoch second, not a raw age: devices.jsx derives it once per poll from
// GET /api/v1/devices' `last_fired_s` (an age AT FETCH TIME) against the
// browser clock at that same instant -- fetchedAtS - last_fired_s -- so the
// countdown this renders can tick down against a live `nowS` between polls
// instead of jumping only once every 10s refresh.
//
// null lastFiredAtS (never fired) and cooldownS === 0 (guard disabled --
// actor_table.c's own convention: "if (slot->cooldown_s > 0 && ...)") both
// mean "no cooldown in effect".
//
// Returns null -- deliberately NOT "0s" -- once the cooldown has elapsed:
// devices.jsx branches on the distinction (render a countdown vs. render
// nothing), and a stale "0s" would read as still-counting-down forever.
export function fmtRemainingCooldown(guard, nowS) {
  if (!guard || !guard.cooldownS || guard.lastFiredAtS == null) return null
  const remaining = guard.cooldownS - (nowS - guard.lastFiredAtS)
  return remaining > 0 ? `${remaining}s` : null
}

// Guard shape: { activationsThisHour, maxPerHour }. maxPerHour === 0 is
// actor_table.c's own "no rate cap" convention ("if (slot->max_per_hour >
// 0) ..."), not a cap of zero activations -- rendered as "no limit", not
// "N/0 this hour".
export function fmtBudget(guard) {
  if (!guard || !guard.maxPerHour) return 'no limit'
  return `${guard.activationsThisHour}/${guard.maxPerHour} this hour`
}

// event_log.h's EVENT_LEVEL_* as sse.c's event_level_str() renders them for
// GET /api/v1/events ("log"/"notify"/"alert"/"critical") -- capitalised for
// display. Falls back to the raw string for a level this UI build doesn't
// know about, rather than throwing or rendering "undefined".
const LEVEL_LABELS = { log: 'Log', notify: 'Notify', alert: 'Alert', critical: 'Critical' }
export function levelLabel(level) {
  return LEVEL_LABELS[level] || level
}

// devices_json.c's live_verdict_str() / api_v1.c's verdict_reason_str() --
// the same six-value vocabulary a device's actions[] "would_refuse_now"
// entry (né "last_result" -- renamed in Task 12 fix round 1, see that
// field's own doc comment in devices_json.c: it is a live pre-check of a
// HYPOTHETICAL manual press, not a record of anything that happened) and a
// 409 refusal body's synchronous "error" both use, translated into
// operator-facing text instead of a wire keyword.
const VERDICT_LABELS = {
  ok: 'OK',
  unknown: 'unknown device or action',
  bound: 'over the limit',
  lockout: 'blocked — locked out',
  cooldown: 'cooling down',
  rate: 'hourly limit reached',
}
export function verdictLabel(v) {
  return VERDICT_LABELS[v] || v
}

// alert.h's alert_code_t, transcribed from that enum's own doc comments.
// GET /api/v1/events' alert/critical rows only ever embed the bare numeric
// code in `msg` (alert.c's alert_drain(): "alert code=%u dev=%d ..."),
// never a label -- this is what turns that number back into the sentence
// the firmware author already wrote once, in the enum comment.
const ALERT_CODE_LABELS = {
  0: 'unknown device or action',
  1: 'parameter exceeds the effective bound',
  2: 'blocked — device locked out (rule-triggered commands only)',
  3: 'blocked — cooldown still active',
  4: 'blocked — hourly limit reached',
  5: 'command expired before it could be dispatched',
  6: 'safety close sent but not confirmed',
  7: 'command queue full — refused',
  8: 'command evicted from the queue by a safety close',
  9: 'command dispatched but failed (connect, write or confirm)',
  10: 'device unreachable — safety close could not be attempted',
}
export function alertCodeLabel(code) {
  return ALERT_CODE_LABELS[code] ?? `alert code ${code}`
}

// alert.c's alert_drain() fixed message shape -- the ONLY structure GET
// /api/v1/events' plain `msg` string carries for an alert/critical row:
//   "alert code=%u dev=%d param=%u"                    (action_id == ACTION_NONE)
//   "alert code=%u dev=%d action=%s param=%u"           (a specific action)
// optionally suffixed " (x%u)" by alert_ring_push()'s repeat-collapsing
// (Task 11, Ruling T10-alertchurn). Returns null for anything else -- a
// rule's own log()/notify() text, or the ring-overflow message -- which is
// NOT a parse failure: most of the log/notify feed is exactly that free
// text and was never alert-shaped to begin with.
const ALERT_MSG_RE = /^alert code=(\d+) dev=(-?\d+)(?: action=(\S+))? param=(\d+)(?: \(x(\d+)\))?$/
export function parseAlertMessage(msg) {
  const m = ALERT_MSG_RE.exec(msg || '')
  if (!m) return null
  return {
    code: Number(m[1]),
    dev: Number(m[2]),
    action: m[3] || null,
    param: Number(m[4]),
    repeat: m[5] ? Number(m[5]) : 1,
  }
}

// capability.c's CAP_SWITCH_STATE decodes to 0/1, or NaN when unset/never
// reported (capability_decode()'s documented contract). Three distinct
// rendered states, not two -- collapsing "never reported" into "OFF" would
// tell an operator an actuator is off when the hub simply doesn't know yet.
export function switchStateLabel(value) {
  if (value == null || Number.isNaN(value)) return 'unknown'
  return value ? 'ON' : 'OFF'
}

// Resolves what a just-sent manual command's row should show (Task 12 fix
// round 1, CRITICAL finding 1). devices_json.c's "would_refuse_now" is a
// live pre-check of a HYPOTHETICAL press evaluated right now -- NOT the
// outcome of the command this row actually sent (only ACTOR_OK/COOLDOWN/
// RATE are even reachable there, actor_table.h) -- so it must never be used
// to decide whether a just-sent command succeeded: a genuinely successful
// dispatch reads back "cooldown" the instant it lands in its own fresh
// cooldown window, and a genuinely failed dispatch (no cooldown configured)
// reads back "ok". This function therefore takes no verdict input at all;
// it answers strictly from two real signals, both ABSOLUTE epoch seconds
// (the same fetchedAtS-derived shape fmtRemainingCooldown's `lastFiredAtS`
// uses) captured as a BASELINE at send time and compared against the
// latest poll:
//
//   dispatchBaselineS/dispatchNowS -- the action's last_fired_s. Advancing
//     past the baseline means the command reached the radio:
//     actor_table_record() runs at dispatch, after the guard re-check
//     passed, so this can only ever be true once dispatch actually
//     happened.
//   confirmBaselineS/confirmNowS -- the device's switch.state capability's
//     confirmed-at time. Advancing past the baseline means a confirm read
//     landed on the same connection.
//
// Four outcomes, a strict partition:
//   'confirmed'  -- both advanced. Reported the instant this is true, even
//                   before `timeoutS` has elapsed -- good news doesn't wait.
//   'pending'    -- still inside the window and not yet confirmed (whether
//                   or not dispatch has been observed yet: confirm may
//                   still arrive on a later poll).
//   'dispatched' -- the window has closed, dispatch was observed, but
//                   confirm never arrived. The honest ceiling for an action
//                   whose wrapper declares no confirm block at all
//                   (confirmNowS stays null forever, so `confirmed` can
//                   never become true for it) -- reported as dispatched,
//                   never guessed at as confirmed.
//   'timeout'    -- the window has closed and dispatch was never observed
//                   either -- no evidence the command reached the radio.
export function resolveActionSend({
  dispatchBaselineS, dispatchNowS, confirmBaselineS, confirmNowS, sentAtS, nowS, timeoutS,
}) {
  const dispatched = dispatchNowS != null && (dispatchBaselineS == null || dispatchNowS > dispatchBaselineS)
  const confirmed = dispatched && confirmNowS != null && (confirmBaselineS == null || confirmNowS > confirmBaselineS)
  if (confirmed) return 'confirmed'
  if (nowS - sentAtS <= timeoutS) return 'pending'
  return dispatched ? 'dispatched' : 'timeout'
}

// Validates a duration_s action's typed input against `param_max` (the
// EFFECTIVE bound GET /api/v1/devices reports) -- Task 12 fix round 1,
// finding 2. Must REFUSE an out-of-range value, not silently clamp it: the
// UI previously clamped an over-max value down to param_max at send time
// with no feedback, so typing 500 into a 300s field silently fired a
// 300-second command -- irrigation.open/pump.run are not reversible once
// dispatched, so a value the operator never actually chose must never reach
// the radio. Returns { valid, param, reason }: `param` is only set (and
// only ever a value action_param_ok() itself would accept, action.c) when
// `valid` is true; `reason` is operator-facing text naming why, including
// the actual bound, so a refusal is exactly as predictable as the brief's
// cooldown/budget text asks for elsewhere.
export function validateDuration(paramStr, paramMax) {
  const trimmed = (paramStr ?? '').trim()
  if (trimmed === '') return { valid: false, param: null, reason: 'enter a duration' }
  const n = Math.trunc(Number(trimmed))
  if (!Number.isFinite(n) || n < 1) return { valid: false, param: null, reason: `enter 1–${paramMax}s` }
  if (n > paramMax) return { valid: false, param: null, reason: `must be ${paramMax}s or less` }
  return { valid: true, param: n, reason: null }
}
