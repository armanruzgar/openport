import { expect, it, vi } from "vitest"
import { createNotificationHistory } from "./notification-history"

const entry = (id: string, scope = "live:main") => ({ id, scope, title: "Paper fill", body: "Simulated fill", kind: "fill" as const })
function storage() {
  const values = new Map<string, string>()
  return { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => { values.set(key, value) } }
}
it("persists, deduplicates and caps notifications without altering receipt times", () => {
  const disk = storage(), history = createNotificationHistory(() => disk, 2)
  const listener = vi.fn(), unsubscribe = history.subscribe(listener)
  history.add({ ...entry("a"), received: "2026-10-16T14:00:00Z" })
  history.add(entry("a"))
  expect(listener).toHaveBeenCalledTimes(1)
  expect(history.get()[0]?.received).toBe("2026-10-16T14:00:00Z")
  history.add(entry("b")); history.add(entry("c"))
  expect(history.get().map((e) => e.id)).toEqual(["b", "c"])
  expect(createNotificationHistory(() => disk).get()).toEqual(history.get())
  unsubscribe()
})
it("marks read and clears only the active account plus browser alerts", () => {
  const history = createNotificationHistory(storage)
  history.add(entry("a")); history.add(entry("b", "replay:main")); history.add(entry("c", "browser"))
  history.markAllRead("live:main")
  expect(history.get().map((e) => e.read)).toEqual([true, false, true])
  history.clear("live:main")
  expect(history.get().map((e) => e.id)).toEqual(["b"])
})
it("handles blocked storage and malformed history", () => {
  const history = createNotificationHistory(() => { throw Error("blocked") })
  history.add(entry("a")); expect(history.get()).toHaveLength(1)
  const disk = storage(); disk.setItem("openport.notification-history", '[null, {}, {"id":"x"}]')
  expect(createNotificationHistory(() => disk).get()).toEqual([])
})
