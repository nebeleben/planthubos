const KEY = 'planthub-theme'

// null means "no explicit preference stored -- follow the OS". That case is
// handled entirely by the `@media (prefers-color-scheme: dark)` block in
// style.css; this module only ever writes 'light'/'dark' once the header
// toggle has been used, same shape as index.html's inline anti-FOUC script
// and lib/auth.js's key storage.
export function getStoredTheme() {
  const t = localStorage.getItem(KEY)
  return t === 'light' || t === 'dark' ? t : null
}

export function setStoredTheme(t) {
  localStorage.setItem(KEY, t)
  document.documentElement.setAttribute('data-theme', t)
}

export function systemPrefersDark() {
  return typeof window.matchMedia === 'function'
    && window.matchMedia('(prefers-color-scheme: dark)').matches
}
