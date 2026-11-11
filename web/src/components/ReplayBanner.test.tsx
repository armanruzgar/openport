// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import type { ReplayState } from "../api/types"
import { status } from "../test/trading-fixtures"
import { replayClock } from "../views/ReplayView"
import { ReplayBanner } from "./ReplayBanner"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const replay: ReplayState = { id: "run-1", file: "Demo market: Selloff", demo: true, scenario: "selloff", seed: "42", generator: 1,
  date: "2026-09-16", start_at: "15:00", provider: "demo", plan: "intraday-25k", symbols: ["SPX"], started: "2026-09-16T13:30:00Z",
  delay_seconds: 0, speed: 10, paused: false, finished: false, time: "2026-09-16T19:00:00Z" }
let host: HTMLDivElement, root: Root, client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
async function draw(state: ReplayState) {
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, "main", () => {}, "replay", state))
  await act(async () => root.render(<QueryClientProvider client={client}><ReplayBanner /></QueryClientProvider>))
}

it("keeps the replay clock out of the live region and announces only changes of state", async () => {
  await draw(replay)
  const banner = host.querySelector('[aria-label="Replay"]')!
  expect(banner.getAttribute("role")).toBe("region")
  const regions = banner.querySelectorAll('[role="status"], [role="alert"], [aria-live]')
  expect(regions).toHaveLength(1)
  const announced = () => banner.querySelector('[role="status"]')!.textContent
  expect(announced()).toBe("Playing, 10×")
  expect(banner.textContent).toContain(replayClock(replay.time))
  expect(announced()).not.toContain(replayClock(replay.time))
  // A tick moves the clock; what is announced stays as it was.
  await draw({ ...replay, time: "2026-09-16T19:00:02Z" })
  expect(banner.textContent).toContain(replayClock("2026-09-16T19:00:02Z"))
  expect(announced()).toBe("Playing, 10×")
  await draw({ ...replay, paused: true, skip_pending: true })
  expect(announced()).toBe("Replay paused. Skip queued for next resume")
  await draw({ ...replay, finished: true })
  expect(announced()).toBe("Replay finished")
})
