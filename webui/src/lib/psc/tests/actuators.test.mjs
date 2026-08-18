import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  fmtRemainingCooldown, fmtBudget, levelLabel, verdictLabel, alertCodeLabel,
  parseAlertMessage, switchStateLabel,
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

test('verdictLabel: maps devices_json.c\'s last_result vocabulary to operator text', () => {
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
