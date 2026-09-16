// Client-side helpers for the Zigbee pairing-window UI (tabs/zigbee.jsx).
//
// The hub only reports a coordinator's remaining permit time (`permit_s`) on
// its poll cadence (2 s while a window is open), so rendering that snapshot
// directly makes the countdown jump every couple of seconds. Instead the tab
// anchors a client-clock window-end (now + permit_s * 1000) each time it
// hears a fresh permit_s and ticks a 1 s clock between polls; permitRemaining
// turns that window-end into the seconds to show. Each poll re-anchors it, so
// it stays server-accurate rather than drifting off the client clock.

// How long the "opening pairing window…" pending state may stand before the
// tab gives up waiting for a bridge to open its window and reverts to the
// button. A bridge receives the queued permit command in its ~2 s POLL/RX
// window, so a window that has not opened in 15 s means the command was lost
// or the node is offline -- long enough to never trip on a healthy bridge,
// short enough that a stuck request does not hang the UI.
export const PAIR_PENDING_TIMEOUT_MS = 15000

// Seconds left in a permit window, from a client-clock end time and the
// current client clock. Ceil so a window with a fraction of a second left
// still reads at least 1 (it is genuinely still open); never negative; a
// falsy end (no window) reads as 0.
export function permitRemaining(windowEndMs, nowMs) {
  if (!windowEndMs) return 0
  return Math.max(0, Math.ceil((windowEndMs - nowMs) / 1000))
}

// Whether a queued open is still "pending" -- the window we asked a bridge to
// open has neither appeared (the caller stops treating it as pending once
// permit_s > 0) nor timed out. False when unset (postedAtMs falsy).
export function pendingActive(postedAtMs, nowMs, timeoutMs) {
  if (!postedAtMs) return false
  return (nowMs - postedAtMs) < timeoutMs
}
