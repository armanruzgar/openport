import { useSyncExternalStore } from "react"

export const releaseEndpoint = "https://api.github.com/repos/armanruzgar/openport/releases/latest"
export const updateInterval = 24 * 60 * 60 * 1000
const enabledKey = "openport.updates.enabled"
const cacheKey = "openport.updates.cache"
const dismissedKey = "openport.updates.dismissed"

function parseVersion(value: string) {
  const match = /^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([\da-zA-Z-]+(?:\.[\da-zA-Z-]+)*))?(?:\+[\da-zA-Z-]+(?:\.[\da-zA-Z-]+)*)?$/.exec(value)
  if (!match) return null
  const pre = match[4]?.split(".") ?? []
  if (pre.some((part) => /^0\d+$/.test(part))) return null
  return { core: match.slice(1, 4).map((part) => BigInt(part!)), pre }
}

/** SemVer precedence, including prereleases; build metadata has no precedence. */
export function isNewerVersion(candidate: string, current: string): boolean {
  const next = parseVersion(candidate), running = parseVersion(current)
  if (!next || !running) return false
  for (let index = 0; index < 3; index++) {
    if (next.core[index] !== running.core[index]) return next.core[index]! > running.core[index]!
  }
  if (!next.pre.length || !running.pre.length) return !next.pre.length && running.pre.length > 0
  for (let index = 0; index < Math.max(next.pre.length, running.pre.length); index++) {
    const left = next.pre[index], right = running.pre[index]
    if (left === right) continue
    if (left === undefined || right === undefined) return right === undefined
    const leftNumeric = /^\d+$/.test(left), rightNumeric = /^\d+$/.test(right)
    if (leftNumeric && rightNumeric) return BigInt(left) > BigInt(right)
    return leftNumeric !== rightNumeric ? rightNumeric : left > right
  }
  return false
}

export interface Release { version: string; url: string }
interface Cache { checkedAt: number; release: Release | null }
interface UpdateState { enabled: boolean; release: Release | null; dismissed: string | null }
type Storage = Pick<globalThis.Storage, "getItem" | "setItem">

function releaseFor(tag: unknown): Release | null {
  return typeof tag === "string" && parseVersion(tag)
    ? { version: tag, url: `https://github.com/armanruzgar/openport/releases/tag/${encodeURIComponent(tag)}` } : null
}

export function createUpdateStore(
  storage: () => Storage = () => localStorage,
  fetcher: typeof fetch = (...args) => fetch(...args),
  now = () => Date.now(),
  // Serialize checks across tabs too. Browsers offer locks only in secure contexts; on
  // plain HTTP the day reserved in storage still limits them. Without storage, skip the check.
  lock: (check: () => Promise<void>) => Promise<void> = (check) =>
    typeof navigator !== "undefined" && navigator.locks
      ? navigator.locks.request("openport.update-check", check) : check(),
) {
  let state: UpdateState | undefined
  let pending: Promise<void> | null = null
  const listeners = new Set<() => void>()
  const read = (key: string) => { try { return storage().getItem(key) } catch { return null } }
  const write = (key: string, value: string) => {
    try { storage().setItem(key, value); return true } catch { return false }
  }
  const cache = (): Cache | null => {
    try {
      const value = JSON.parse(read(cacheKey) ?? "null") as Cache | null
      if (!value || !Number.isFinite(value.checkedAt) || value.checkedAt < 0) return null
      return { checkedAt: value.checkedAt, release: releaseFor(value.release?.version) }
    } catch { return null }
  }
  const load = (): UpdateState => ({ enabled: read(enabledKey) === "true", release: cache()?.release ?? null, dismissed: read(dismissedKey) })
  const get = () => state ??= load()
  const emit = (next: UpdateState) => { state = next; listeners.forEach((listener) => listener()) }
  const refresh = () => emit(load())
  return {
    get,
    refresh,
    subscribe(listener: () => void) {
      listeners.add(listener)
      return () => { listeners.delete(listener) }
    },
    setEnabled(enabled: boolean) {
      write(enabledKey, String(enabled))
      emit({ ...get(), enabled })
    },
    dismiss(version: string) {
      write(dismissedKey, version)
      emit({ ...get(), dismissed: version })
    },
    check(current: string | undefined): Promise<void> {
      if (!get().enabled || !current || !parseVersion(current)) return Promise.resolve()
      if (pending) return pending
      pending = lock(async () => {
        if (read(enabledKey) !== "true") return
        const saved = cache()
        const checkedAt = now()
        // A clock moved backwards still waits; it cannot cause another request.
        if (saved && checkedAt - saved.checkedAt < updateInterval) { refresh(); return }
        // Reserve the day before the request, including failed or interrupted attempts.
        if (!write(cacheKey, JSON.stringify({ checkedAt, release: null }))) return
        refresh()
        const controller = new AbortController()
        const timer = setTimeout(() => controller.abort(), 10_000)
        let release: Release | null = null
        try {
          const response = await fetcher(releaseEndpoint, {
            credentials: "omit", referrerPolicy: "no-referrer", redirect: "error", cache: "no-store", signal: controller.signal,
          })
          if (response.ok) {
            const body: unknown = await response.json()
            if (body && typeof body === "object" && "tag_name" in body && "draft" in body && body.draft === false
              && "prerelease" in body && body.prerelease === false) release = releaseFor(body.tag_name)
          }
        } catch { /* Offline, rate-limited and malformed responses stay silent. */ }
        finally { clearTimeout(timer) }
        write(cacheKey, JSON.stringify({ checkedAt, release }))
        refresh()
      }).catch(() => { /* Unavailable browser locks stay silent too. */ }).finally(() => { pending = null })
      return pending
    },
  }
}

export const updates = createUpdateStore()
function subscribe(listener: () => void) {
  const unsubscribe = updates.subscribe(listener)
  const onStorage = (event: StorageEvent) => {
    if (event.key === null || [enabledKey, cacheKey, dismissedKey].includes(event.key)) updates.refresh()
  }
  window.addEventListener("storage", onStorage)
  return () => { unsubscribe(); window.removeEventListener("storage", onStorage) }
}
export const useUpdates = () => useSyncExternalStore(subscribe, updates.get, updates.get)
