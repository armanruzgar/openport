import { useSyncExternalStore } from "react"

export type NotificationKind = "fill" | "assignment" | "rule" | "alert" | "dividend"
export interface HistoryEntry {
  id: string; title: string; body: string; kind: NotificationKind; scope: string
  /** Browser receipt time, never presented as an exchange or fill time. */
  received: string; read: boolean
}
const key = "openport.notification-history"
const kinds = new Set(["fill", "assignment", "rule", "alert", "dividend"])
const empty: HistoryEntry[] = []
export function createNotificationHistory(storage: () => Pick<Storage, "getItem" | "setItem">, limit = 200) {
  let entries: HistoryEntry[] | undefined
  const listeners = new Set<() => void>()
  const get = () => {
    if (entries) return entries
    try {
      const saved: unknown = JSON.parse(storage().getItem(key) ?? "[]")
      entries = Array.isArray(saved) ? saved.filter((e): e is HistoryEntry => e && typeof e.id === "string" &&
        typeof e.title === "string" && typeof e.body === "string" && typeof e.scope === "string" &&
        kinds.has(e.kind) && typeof e.read === "boolean" && typeof e.received === "string" && Number.isFinite(Date.parse(e.received))).slice(-limit) : []
    } catch { entries = [] }
    return entries
  }
  const set = (next: HistoryEntry[]) => {
    entries = next
    try { storage().setItem(key, JSON.stringify(next)) } catch { /* Keep the current session's history. */ }
    listeners.forEach((listener) => listener())
  }
  const matches = (entry: HistoryEntry, scope?: string) => scope == null || entry.scope === scope || entry.scope === "browser"
  return {
    get,
    add(entry: Omit<HistoryEntry, "received" | "read"> & Partial<Pick<HistoryEntry, "received" | "read">>) {
      if (get().some((e) => e.id === entry.id && e.scope === entry.scope)) return
      set([...get(), { ...entry, received: entry.received ?? new Date().toISOString(), read: entry.read ?? false }].slice(-limit))
    },
    markAllRead(scope?: string) { set(get().map((entry) => matches(entry, scope) ? { ...entry, read: true } : entry)) },
    clear(scope?: string) { set(get().filter((entry) => !matches(entry, scope))) },
    reload() { entries = undefined; get(); listeners.forEach((listener) => listener()) },
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const notificationHistory = createNotificationHistory(() => localStorage)
export function useNotificationHistory(scope: string) {
  const all = useSyncExternalStore(notificationHistory.subscribe, notificationHistory.get, () => empty)
  return all.filter((entry) => entry.scope === scope || entry.scope === "browser")
}
