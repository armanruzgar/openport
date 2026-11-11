import type { NotificationHistory, NotificationHistoryEvent } from "../api/types"
import type { HistoryEntry } from "./notification-history"

export type ServerEntry = Omit<HistoryEntry, "read">

/** The server's clock in its nanosecond ISO form, as an ISO string a browser parses; null when unreadable. */
export function serverTime(text: string): string | null {
  const time = Date.parse(text.replace(/(\.\d{3})\d*Z$/, "$1Z"))
  return Number.isFinite(time) ? new Date(time).toISOString() : null
}

/**
 * A kept server notification as a drawer entry, for the kinds a browser cannot observe for
 * itself: a feed stall, a floor warning, a staged playbook order and each firing of an alert.
 * Fills, deliveries and rule trips come from the account, whose watchers announce them.
 */
export function serverEntry(event: NotificationHistoryEvent, scope: string): ServerEntry | null {
  const received = serverTime(event.received)
  if (!received) return null
  const id = `server:${event.sequence}:${event.received}`
  const base = { scope, received, body: event.message }
  const details = event.details ?? {}
  switch (event.event) {
    case "feed_stalled": return { ...base, id, kind: "alert", title: "Feed stalled" }
    case "floor": return { ...base, id, kind: "rule", title: details.warning === false ? "Clear of the floor band" : "Near the account floor" }
    case "playbook_ready": return { ...base, id, kind: "alert", title: "Playbook order staged" }
    case "alert":
      // The ID the alert watcher gives a firing it saw itself, so neither repeats the other.
      return { ...base, kind: "alert", title: "Alert",
        id: typeof details.alert_id === "string" && typeof details.fired === "number" ? `server-alert:${details.alert_id}:${details.fired}` : id }
    default: return null
  }
}

const key = "openport.notification-server"
/** The newest server event already taken into the drawer, per scope: the server's own time text. */
function taken(storage: Pick<Storage, "getItem">): Record<string, string> {
  try {
    const saved: unknown = JSON.parse(storage.getItem(key) ?? "{}")
    return saved && typeof saved === "object" && !Array.isArray(saved)
      ? Object.fromEntries(Object.entries(saved).filter((entry): entry is [string, string] => typeof entry[1] === "string")) : {}
  } catch { return {} }
}

/**
 * The drawer entries of a history answer not taken before, oldest first. What was taken is
 * remembered per scope, so entries cleared from the drawer do not come back with the next read.
 * Received times are compared as the server writes them, which a restart keeps in order.
 */
export function newServerEntries(history: NotificationHistory, scope: string,
  storage: Pick<Storage, "getItem" | "setItem"> = localStorage): ServerEntry[] {
  const marks = taken(storage)
  const after = marks[scope] ?? ""
  const fresh = history.events.filter((event) => event.received > after)
  if (!fresh.length) return []
  const newest = fresh.reduce((last, event) => event.received > last ? event.received : last, after)
  try { storage.setItem(key, JSON.stringify({ ...marks, [scope]: newest })) } catch { /* Keep this session's entries. */ }
  return fresh.flatMap((event) => serverEntry(event, scope) ?? [])
}
