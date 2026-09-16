import { test } from 'node:test'
import assert from 'node:assert/strict'
import { permitRemaining, pendingActive, PAIR_PENDING_TIMEOUT_MS } from '../pairing.js'

// permitRemaining renders a smooth per-second countdown from a client-clock
// window-end: ceil so a window with 0.4 s left still reads "1s left" rather
// than flashing 0 while it is genuinely still open, and never negative.
test('permitRemaining: seconds left, ceil, clamped at zero', () => {
  const now = 10_000
  assert.equal(permitRemaining(now + 40_000, now), 40)
  assert.equal(permitRemaining(now + 39_600, now), 40)   // ceil: 39.6 -> 40
  assert.equal(permitRemaining(now + 200, now), 1)       // still open -> 1, not 0
  assert.equal(permitRemaining(now, now), 0)             // exactly elapsed
  assert.equal(permitRemaining(now - 5_000, now), 0)     // past -> clamped
})

test('permitRemaining: no window end reads as zero', () => {
  assert.equal(permitRemaining(0, 10_000), 0)
  assert.equal(permitRemaining(null, 10_000), 0)
  assert.equal(permitRemaining(undefined, 10_000), 0)
})

// pendingActive gates the "opening shortly…" state: true from the queued
// POST until the window opens or the timeout lapses, so a lost command
// reverts to the button instead of hanging forever.
test('pendingActive: within the timeout window only', () => {
  const posted = 1_000
  assert.equal(pendingActive(posted, posted, PAIR_PENDING_TIMEOUT_MS), true)          // just posted
  assert.equal(pendingActive(posted, posted + PAIR_PENDING_TIMEOUT_MS - 1, PAIR_PENDING_TIMEOUT_MS), true)
  assert.equal(pendingActive(posted, posted + PAIR_PENDING_TIMEOUT_MS, PAIR_PENDING_TIMEOUT_MS), false) // lapsed
  assert.equal(pendingActive(posted, posted + 999_999, PAIR_PENDING_TIMEOUT_MS), false)
})

test('pendingActive: not pending when unset', () => {
  assert.equal(pendingActive(0, 5_000, PAIR_PENDING_TIMEOUT_MS), false)
  assert.equal(pendingActive(null, 5_000, PAIR_PENDING_TIMEOUT_MS), false)
})

test('PAIR_PENDING_TIMEOUT_MS is a sane bridge-open budget', () => {
  assert.ok(PAIR_PENDING_TIMEOUT_MS >= 10_000 && PAIR_PENDING_TIMEOUT_MS <= 30_000)
})
