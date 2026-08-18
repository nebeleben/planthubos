import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  fmtRemainingCooldown, fmtBudget, levelLabel, verdictLabel, alertCodeLabel,
  parseAlertMessage, switchStateLabel, resolveActionSend, validateDuration,
} from '../../actuators.js'

// fmtRemainingCooldown(guard, nowS) -- guard: { cooldownS, lastFiredAtS }.
// lastFiredAtS is an ABSOLUTE epoch second (devices.jsx derives it once per
// poll from GET /api/v1/devices' last_fired_s -- an age at fetch time --
// against the browser clock at that same instant), so a countdown can tick
// against a live `nowS` between polls. Task 12 brief: must return null, not
// "0s", once the cooldown has elapsed -- the UI branches on that.
test('fmtRemainingCooldown: mid-cooldown returns the remaining seconds', () => {
  assert.equal(fmtRemainingCooldown({ cooldownS: 30, lastFiredAtS: 100 }, 110), '20s')
})

test('fmtRemainingCooldown: returns null (not "0s") exactly when the cooldown has elapsed', () => {
  assert.equal(fmtRemainingCooldown({ cooldownS: 30, lastFiredAtS: 100 }, 130), null)
})

test('fmtRemainingCooldown: returns null once past elapsed, not just at the boundary', () => {
  assert.equal(fmtRemainingCooldown({ cooldownS: 30, lastFiredAtS: 100 }, 200), null)
})

test('fmtRemainingCooldown: never fired (lastFiredAtS null) returns null', () => {
  assert.equal(fmtRemainingCooldown({ cooldownS: 30, lastFiredAtS: null }, 110), null)
})

test('fmtRemainingCooldown: cooldownS 0 (guard disabled, actor_table.c convention) returns null even just after firing', () => {
  assert.equal(fmtRemainingCooldown({ cooldownS: 0, lastFiredAtS: 100 }, 100), null)
})

// fmtBudget(guard) -- guard: { activationsThisHour, maxPerHour }. maxPerHour
// === 0 is actor_table.c's own "no rate cap" convention, not a cap of zero.
test('fmtBudget: renders used/max when a cap is configured', () => {
  assert.equal(fmtBudget({ activationsThisHour: 2, maxPerHour: 5 }), '2/5 this hour')
})

test('fmtBudget: maxPerHour 0 means unlimited, not zero-allowed', () => {
  assert.equal(fmtBudget({ activationsThisHour: 9, maxPerHour: 0 }), 'no limit')
})

test('levelLabel: maps every GET /api/v1/events level string to a display label', () => {
  assert.equal(levelLabel('log'), 'Log')
  assert.equal(levelLabel('notify'), 'Notify')
  assert.equal(levelLabel('alert'), 'Alert')
  assert.equal(levelLabel('critical'), 'Critical')
})

test('levelLabel: an unrecognised level falls back to itself rather than throwing', () => {
  assert.equal(levelLabel('mystery'), 'mystery')
})

test('verdictLabel: maps devices_json.c\'s would_refuse_now vocabulary to operator text', () => {
  assert.equal(verdictLabel('ok'), 'OK')
  assert.equal(verdictLabel('lockout'), 'blocked — locked out')
  assert.equal(verdictLabel('cooldown'), 'cooling down')
  assert.equal(verdictLabel('rate'), 'hourly limit reached')
  assert.equal(verdictLabel('bound'), 'over the limit')
  assert.equal(verdictLabel('unknown'), 'unknown device or action')
})

test('alertCodeLabel: maps alert.h\'s alert_code_t (as embedded in the msg text) to operator text', () => {
  assert.equal(alertCodeLabel(2), 'blocked — device locked out (rule-triggered commands only)')
  assert.equal(alertCodeLabel(6), 'safety close sent but not confirmed')
  assert.equal(alertCodeLabel(10), 'device unreachable — safety close could not be attempted')
})

test('alertCodeLabel: an unrecognised code still renders something, not undefined', () => {
  assert.equal(alertCodeLabel(99), 'alert code 99')
})

// parseAlertMessage(msg) -- alert.c's alert_drain() fixed message shape is
// the ONLY structure GET /api/v1/events' plain `msg` string carries for an
// alert/critical row. Everything else (a rule's own log()/notify() text,
// the ring-overflow message) is free text with no such structure -- that is
// not a parse failure, it must come back null so the UI falls back to
// rendering the raw message instead of a bogus partial match.
test('parseAlertMessage: parameterless-action shape (no action= clause)', () => {
  assert.deepEqual(parseAlertMessage('alert code=4 dev=2 param=0'), {
    code: 4, dev: 2, action: null, param: 0, repeat: 1,
  })
})

test('parseAlertMessage: parameterised-action shape with an action clause', () => {
  assert.deepEqual(parseAlertMessage('alert code=2 dev=3 action=pump.run param=30'), {
    code: 2, dev: 3, action: 'pump.run', param: 30, repeat: 1,
  })
})

test('parseAlertMessage: alert_ring_push()\'s collapsed-repeat suffix', () => {
  assert.deepEqual(parseAlertMessage('alert code=4 dev=1 action=switch.on param=0 (x3)'), {
    code: 4, dev: 1, action: 'switch.on', param: 0, repeat: 3,
  })
})

test('parseAlertMessage: a negative dev_idx (alert_rec_t.dev_idx is int8_t) still parses', () => {
  assert.deepEqual(parseAlertMessage('alert code=0 dev=-1 param=0'), {
    code: 0, dev: -1, action: null, param: 0, repeat: 1,
  })
})

test('parseAlertMessage: a rule\'s own notify() text is not alert-shaped -- returns null', () => {
  assert.equal(parseAlertMessage('Monstera is dry: 18%'), null)
})

test('parseAlertMessage: the ring-overflow message is not alert-shaped -- returns null', () => {
  assert.equal(parseAlertMessage('alert ring overflow: 2 alert(s) dropped'), null)
})

// switchStateLabel(value) -- the switch.state capability's decoded value
// (capability_decode(): NaN when unset/never reported). Must render three
// distinct states, not collapse "never reported" into "OFF".
test('switchStateLabel: on/off/unknown are three distinct states', () => {
  assert.equal(switchStateLabel(1), 'ON')
  assert.equal(switchStateLabel(0), 'OFF')
  assert.equal(switchStateLabel(NaN), 'unknown')
  assert.equal(switchStateLabel(null), 'unknown')
  assert.equal(switchStateLabel(undefined), 'unknown')
})

// resolveActionSend(state) -- Task 12 fix round 1, CRITICAL finding 1.
// devices_json.c's `would_refuse_now` (né `last_result`) is a live pre-check
// of a HYPOTHETICAL manual press evaluated right now, not the outcome of the
// command a row just sent -- only ACTOR_OK/COOLDOWN/RATE are even reachable
// there (actor_table.h), so a genuinely successful dispatch reads "cooldown"
// the instant it lands in its own fresh cooldown window, and a genuinely
// failed dispatch (no cooldown configured) reads "ok". This resolver takes
// NO verdict/would_refuse_now input at all -- by construction it cannot
// reproduce that misreport -- and instead answers strictly from two real
// signals: last_fired_s advancing past the baseline captured at send time
// (dispatch reached the radio -- actor_table_record() runs at dispatch,
// after the guard re-check passed) and the switch.state capability's own
// confirmed-at timestamp advancing past ITS baseline (the confirm read
// landed on the same connection).
const TIMEOUT_S = 30

test('resolveActionSend: nothing advanced yet, still inside the window -> pending', () => {
  assert.equal(resolveActionSend({
    dispatchBaselineS: 100, dispatchNowS: 100,
    confirmBaselineS: 50, confirmNowS: 50,
    sentAtS: 100, nowS: 110, timeoutS: TIMEOUT_S,
  }), 'pending')
})

test('resolveActionSend: dispatch advanced but confirm has not, still inside the window -> still pending (confirm may yet arrive)', () => {
  assert.equal(resolveActionSend({
    dispatchBaselineS: 100, dispatchNowS: 105,
    confirmBaselineS: 50, confirmNowS: 50,
    sentAtS: 100, nowS: 110, timeoutS: TIMEOUT_S,
  }), 'pending')
})

test('resolveActionSend: dispatch advanced, confirm never available (no confirm block declared) -> dispatched once the window closes', () => {
  assert.equal(resolveActionSend({
    dispatchBaselineS: 100, dispatchNowS: 105,
    confirmBaselineS: null, confirmNowS: null,
    sentAtS: 100, nowS: 131, timeoutS: TIMEOUT_S,
  }), 'dispatched')
})

test('resolveActionSend: dispatch AND confirm both advanced -> confirmed, even if reported before the window closes', () => {
  assert.equal(resolveActionSend({
    dispatchBaselineS: 100, dispatchNowS: 105,
    confirmBaselineS: 50, confirmNowS: 106,
    sentAtS: 100, nowS: 108, timeoutS: TIMEOUT_S,
  }), 'confirmed')
})

test('resolveActionSend: confirmed is reported even after the window has closed', () => {
  assert.equal(resolveActionSend({
    dispatchBaselineS: 100, dispatchNowS: 105,
    confirmBaselineS: 50, confirmNowS: 106,
    sentAtS: 100, nowS: 200, timeoutS: TIMEOUT_S,
  }), 'confirmed')
})

test('resolveActionSend: neither advanced and the window has closed -> timeout', () => {
  assert.equal(resolveActionSend({
    dispatchBaselineS: 100, dispatchNowS: 100,
    confirmBaselineS: 50, confirmNowS: 50,
    sentAtS: 100, nowS: 131, timeoutS: TIMEOUT_S,
  }), 'timeout')
})

test('resolveActionSend: a null baseline (device never fired/confirmed before) treats any observed value as new', () => {
  assert.equal(resolveActionSend({
    dispatchBaselineS: null, dispatchNowS: 105,
    confirmBaselineS: null, confirmNowS: 106,
    sentAtS: 100, nowS: 108, timeoutS: TIMEOUT_S,
  }), 'confirmed')
})

// The actual regression: the old design read devices_json.c's verdict field
// to decide "refused after queueing" vs "confirmed" -- a command dispatched
// straight into its own fresh cooldown would have shown as refused even
// though it succeeded. resolveActionSend has no verdict parameter to read,
// so the identical dispatch-then-cooldown scenario (dispatch advanced,
// confirm not yet in, still inside the window) can only ever come back
// 'pending' here -- never a refusal -- regardless of what would_refuse_now
// says at that same instant.
test('resolveActionSend: a fresh cooldown right after a successful dispatch is never read as a refusal', () => {
  assert.equal(resolveActionSend({
    dispatchBaselineS: 100, dispatchNowS: 101,   // just fired -- now inside its own cooldown
    confirmBaselineS: 50, confirmNowS: 50,       // confirm hasn't landed yet
    sentAtS: 100, nowS: 102, timeoutS: TIMEOUT_S,
  }), 'pending')
})

// validateDuration(paramStr, paramMax) -- Task 12 fix round 1, finding 2.
// An over-max value used to be silently clamped to paramMax at send time
// with no feedback; this must refuse instead, the same principle already
// applied to a blank/invalid value.
test('validateDuration: blank input is refused, not defaulted', () => {
  const r = validateDuration('', 300)
  assert.equal(r.valid, false)
  assert.equal(r.param, null)
})

test('validateDuration: exactly at the max is valid (inclusive bound)', () => {
  const r = validateDuration('300', 300)
  assert.equal(r.valid, true)
  assert.equal(r.param, 300)
})

test('validateDuration: one over the max is refused, not silently clamped', () => {
  const r = validateDuration('301', 300)
  assert.equal(r.valid, false)
  assert.equal(r.param, null)
  assert.match(r.reason, /300/)
})

test('validateDuration: zero is refused (a zero-second command is not a real one)', () => {
  assert.equal(validateDuration('0', 300).valid, false)
})

test('validateDuration: non-numeric input is refused', () => {
  assert.equal(validateDuration('abc', 300).valid, false)
})

test('validateDuration: exactly 1 (the minimum) is valid', () => {
  const r = validateDuration('1', 120)
  assert.equal(r.valid, true)
  assert.equal(r.param, 1)
})
