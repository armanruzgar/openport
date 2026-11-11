import { describe, expect, it } from "vitest"
import type { NotificationHistory, NotificationHistoryEvent } from "../api/types"
import { newServerEntries, serverEntry, serverTime } from "./server-notifications"

const event = (fields: Partial<NotificationHistoryEvent>): NotificationHistoryEvent => ({
  type: "event", version: 1, sequence: "1", event: "feed_stalled", account: "", source: "live", run_id: null,
  market_time: "2026-09-22T14:05:00.000000000Z", simulated: true, message: "SPX: feed stalled; check the terminal's feed status.",
  details: { underlying: "SPX" }, received: "2026-09-22T14:20:00.123456789Z", ...fields,
})
const history = (events: NotificationHistoryEvent[]): NotificationHistory =>
  ({ account: "main", source: "live", capacity: 200, sequence: "9", events })
function memory() {
  const items = new Map<string, string>()
  return { getItem: (name: string) => items.get(name) ?? null, setItem: (name: string, value: string) => { items.set(name, value) } }
}

describe("server notifications", () => {
  it("reads the server's nanosecond times", () => {
    expect(serverTime("2026-09-22T14:20:00.123456789Z")).toBe("2026-09-22T14:20:00.123Z")
    expect(serverTime("not a time")).toBeNull()
  })

  it("turns the kinds a browser cannot observe into drawer entries and leaves the rest", () => {
    expect(serverEntry(event({}), "live:main")).toEqual({ id: "server:1:2026-09-22T14:20:00.123456789Z", scope: "live:main",
      received: "2026-09-22T14:20:00.123Z", body: "SPX: feed stalled; check the terminal's feed status.", kind: "alert", title: "Feed stalled" })
    expect(serverEntry(event({ event: "floor", details: { warning: true, room: "400.00" } }), "live:main")?.title).toBe("Near the account floor")
    expect(serverEntry(event({ event: "floor", details: { warning: false, room: "900.00" } }), "live:main")?.kind).toBe("rule")
    expect(serverEntry(event({ event: "playbook_ready" }), "live:main")?.title).toBe("Playbook order staged")
    // An alert firing takes the ID the alert watcher gives it, so neither repeats the other.
    expect(serverEntry(event({ event: "alert", details: { alert_id: "7", fired: 3 } }), "live:main")?.id).toBe("server-alert:7:3")
    expect(serverEntry(event({ event: "alert", details: {} }), "live:main")?.id).toBe("server:1:2026-09-22T14:20:00.123456789Z")
    for (const kind of ["fill", "order_state", "rule_trip", "assignment", "exercise", "order_rejected", "a_later_kind"])
      expect(serverEntry(event({ event: kind }), "live:main")).toBeNull()
    expect(serverEntry(event({ received: "garbled" }), "live:main")).toBeNull()
  })

  it("takes each event once per scope, so cleared entries stay cleared", () => {
    const storage = memory()
    const first = event({ sequence: "1", received: "2026-09-22T14:20:00.000000000Z" })
    const second = event({ sequence: "2", event: "playbook_ready", received: "2026-09-22T14:21:00.000000000Z" })
    expect(newServerEntries(history([first]), "live:main", storage).map((entry) => entry.id)).toEqual(["server:1:2026-09-22T14:20:00.000000000Z"])
    expect(newServerEntries(history([first]), "live:main", storage)).toEqual([])
    expect(newServerEntries(history([first, second]), "live:main", storage).map((entry) => entry.title)).toEqual(["Playbook order staged"])
    // Another account's drawer takes them for itself.
    expect(newServerEntries(history([first, second]), "live:practice", storage)).toHaveLength(2)
    // Kinds the drawer leaves out still move the mark.
    const fill = event({ sequence: "3", event: "fill", received: "2026-09-22T14:22:00.000000000Z" })
    expect(newServerEntries(history([first, second, fill]), "live:main", storage)).toEqual([])
    const later = event({ sequence: "4", received: "2026-09-22T14:23:00.000000000Z" })
    expect(newServerEntries(history([fill, later]), "live:main", storage).map((entry) => entry.title)).toEqual(["Feed stalled"])
  })
})
