const KEY = 'planthub_key'

export function getKey() {
  return localStorage.getItem(KEY) || ''
}

export function setKey(k) {
  if (k) localStorage.setItem(KEY, k)
  else localStorage.removeItem(KEY)
}

export function authHeaders() {
  const k = getKey()
  return k ? { Authorization: `Bearer ${k}` } : {}
}
