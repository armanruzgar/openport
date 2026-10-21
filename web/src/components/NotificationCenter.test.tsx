// @vitest-environment jsdom
import { act } from "react"
import { createRoot } from "react-dom/client"
import { expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { status } from "../test/trading-fixtures"
import { notificationHistory } from "../lib/notification-history"
import { AlertsButton } from "./Alerts"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
it("shows persisted unread entries for this account and marks or clears them without losing other accounts", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  notificationHistory.clear()
  notificationHistory.add({ id: "fill", kind: "fill", scope: "live:main", title: "Paper fill", body: "Sample fill" })
  notificationHistory.add({ id: "other", kind: "rule", scope: "live:other", title: "Other account", body: "Other rule" })
  const host = document.createElement("div"), root = createRoot(host)
  document.body.append(host)
  try {
    await act(async () => root.render(<AlertsButton />))
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Notifications, 1 unread"]')!.click())
    expect(host.textContent).toContain("Sample fill")
    expect(host.textContent).not.toContain("Other rule")
    expect(host.textContent).toContain("receipt times")
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Mark all read")!.click())
    expect(host.querySelector('[aria-label="Notifications, 0 unread"]')).not.toBeNull()
    await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Clear")!.click())
    expect(host.textContent).toContain("No notifications observed yet.")
    expect(notificationHistory.get().map((e) => e.id)).toEqual(["other"])
  } finally {
    await act(async () => root.unmount()); host.remove(); notificationHistory.clear(); vi.unstubAllGlobals()
  }
})
