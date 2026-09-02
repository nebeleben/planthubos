// Every hub-side setting that only applies at boot ends the same way: the
// hub answers, reboots ~1.5 s later, and is back on the LAN well inside
// 15 s. One message + one hard reload, shared by Settings and the
// onboarding screens so the copy and the timing never drift apart.
export function rebootCountdown(setMsg, extra) {
  setMsg(`Saved — hub is rebooting to apply.${extra ? ` ${extra}` : ''} This page reloads in ~15 s.`)
  setTimeout(() => location.reload(), 15000)
}
