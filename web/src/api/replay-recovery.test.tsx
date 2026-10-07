// @vitest-environment jsdom
import { writeToken } from "../lib/write-token"
import { captureDestination, withDestination, rememberReplay } from "./destination"
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { beforeEach, afterEach, expect, it, vi } from "vitest"
import { LiveProvider } from "./live"
import { AccountSwitcher } from "../components/AccountSwitcher"
import { ReplayView } from "../views/ReplayView"
import { ReplayBanner } from "../components/ReplayBanner"
import type { ReactNode } from "react"
import { api } from "./client"
import { connectLive } from "./connection"
import { dataSource } from "../lib/data-source"
import { activeAccount } from "../lib/active-account"
import { status } from "../test/trading-fixtures"
import type { ReplayState, Tick } from "./types"
vi.mock("./connection", () => ({ connectLive: vi.fn() }))
let root: Root, host: HTMLDivElement, client: QueryClient
let onReplay: (tick: Tick | null) => void
const replay: ReplayState = { id: "run", file: "demo", provider: "demo", symbols: [], started: null, delay_seconds: 0, speed: 1, paused: true, finished: false, time: null }
beforeEach(() => {
  vi.useFakeTimers(); vi.setSystemTime(0)
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  dataSource.set("replay"); activeAccount.set("main"); rememberReplay(undefined)
  vi.mocked(connectLive).mockImplementation((_url, _client, _tick, connection, callback) => {
    connection("open"); onReplay = callback!; return () => {}
  })
  vi.spyOn(api, "status").mockResolvedValue(status)
  vi.spyOn(api, "replay").mockResolvedValue({ directory: "", recordings: [], replay })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount()); client.clear(); host.remove()
  dataSource.set("live"); activeAccount.set("main"); writeToken.set("")
  vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.useRealTimers()
})
async function render(children: ReactNode = <div>Terminal</div>) {
  await act(async () => root.render(<QueryClientProvider client={client}><LiveProvider>{children}</LiveProvider></QueryClientProvider>))
}
async function advance(ms: number) { await act(async () => vi.advanceTimersByTimeAsync(ms)) }
it("shows a quiet reconnecting state without leaving an alive paused replay, and a tick clears it", async () => {
  await render(); await advance(6_000)
  expect(dataSource.get()).toBe("replay")
  expect(host.textContent).toContain("Reconnecting to the replay…")
  await act(async () => onReplay({ type: "replay_tick", replay, underlyings: [], engine: { events_per_second: 0, analytics_ms: 0, contracts: 0 }, feed: { state: "live", message: "" } } as Tick))
  expect(host.textContent).not.toContain("Reconnecting to the replay…")
})
it.each([false, true])("shows a persistent destination banner; Back to replay requires an existing run (%s)", async exists => {
  vi.mocked(api.replay).mockResolvedValue({ directory: "", recordings: [], replay: exists ? { ...replay, finished: true } : null })
  await render(); await advance(6_000)
  expect(dataSource.get()).toBe("live")
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("Orders now go to live account Practice")
  expect(host.textContent?.includes("Back to replay")).toBe(exists)
  await advance(30_000)
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
  await act(async () => [...host.querySelectorAll("button")].find(button => button.textContent === "OK")!.click())
  expect(host.querySelector('[role="alert"]')).toBeNull()
})
it("announces an account disappearing", async () => {
  dataSource.set("live"); activeAccount.set("missing")
  await render(); await advance(1)
  expect(activeAccount.get()).toBe("main")
  expect(host.textContent).toContain("active account is no longer available")
  expect(host.textContent).toContain("live account Practice")
})

it("announces sandbox expiry with the new live account", async () => {
  dataSource.set("live"); activeAccount.set("sandbox-one"); writeToken.set("sandbox-token", "sandbox-one")
  vi.mocked(api.status).mockResolvedValue({ ...status, sandboxes: { enabled: true } } as typeof status)
  await render()
  const send = vi.fn(async () => new Response(JSON.stringify({ error: { code: "SANDBOX_EXPIRED", message: "Expired" } }), { status: 403 }))
  vi.stubGlobal("fetch", send)
  await act(async () => {
    await expect(withDestination(captureDestination(), () => api.cancelAllOrders(null, "token"))).rejects.toMatchObject({ code: "SANDBOX_EXPIRED" })
  })
  expect(activeAccount.get()).toBe("main")
  expect(host.textContent).toContain("sandbox expired")
  expect(host.textContent).toContain("Orders now go to live account Practice")
})

async function click(label: string) {
  const button = [...host.querySelectorAll("button")].find(button => button.textContent === label)
  expect(button, label).toBeDefined()
  await act(async () => button!.click())
}
async function replayTick(id = "run") {
  await act(async () => onReplay({ type: "replay_tick", replay: { ...replay, id, demo: true, scenario: "trend", seed: "42" }, underlyings: [], engine: { events_per_second: 0, analytics_ms: 0, contracts: 0 }, feed: { state: "live", message: "" } } as Tick))
}
it("keeps a deliberate sidebar account switch silent, then announces its automatic fallback", async () => {
  dataSource.set("live")
  const accounts = [{ id: "main", name: "Practice", trading: status.trading! }, { id: "side", name: "Side", trading: status.trading! }]
  vi.mocked(api.status).mockResolvedValue({ ...status, accounts })
  await render(<AccountSwitcher />); await advance(1)
  const select = host.querySelector<HTMLSelectElement>("#account-switcher")!
  await act(async () => { select.value = "side"; select.dispatchEvent(new Event("change", { bubbles: true })) })
  expect(activeAccount.get()).toBe("side")
  expect(host.querySelector('[role="alert"]')).toBeNull()
  vi.mocked(api.status).mockResolvedValue({ ...status, accounts: accounts.slice(0, 1) })
  await act(async () => { await client.invalidateQueries({ queryKey: ["status"] }) }); await advance(1)
  expect(activeAccount.get()).toBe("main")
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("active account is no longer available")
})
it("keeps Trade this replay and both Back to live controls silent", async () => {
  dataSource.set("live")
  vi.spyOn(api, "plans").mockResolvedValue({ plans: [] })
  vi.spyOn(api, "liveAccounts").mockResolvedValue({ accounts: [] })
  await render(<><ReplayView /><ReplayBanner /></>); await advance(1)
  await click("Trade this replay"); await advance(1)
  expect(dataSource.get()).toBe("replay")
  expect(host.querySelector('[role="alert"]')).toBeNull()
  await click("Back to live")
  expect(dataSource.get()).toBe("live")
  expect(host.querySelector('[role="alert"]')).toBeNull()
  await advance(1); await click("Trade this replay"); await replayTick()
  const banner = host.querySelector('[aria-label="Replay"]')!
  await act(async () => [...banner.querySelectorAll("button")].find(button => button.textContent === "Back to live")!.click())
  expect(dataSource.get()).toBe("live")
  expect(host.querySelector('[role="alert"]')).toBeNull()
})
it.each([false, true])("keeps Replay this seed silent when the new tick precedes its response: %s", async tickFirst => {
  let resolve!: (result: { replay: ReplayState }) => void
  vi.spyOn(api, "startReplay").mockImplementation(() => new Promise(done => { resolve = done }))
  await render(<ReplayBanner />); await advance(1); await replayTick()
  await click("Replay this seed")
  if (tickFirst) await replayTick("new-run")
  await act(async () => resolve({ replay: { ...replay, id: "new-run" } }))
  if (!tickFirst) await replayTick("new-run")
  expect(host.querySelector('[role="alert"]')).toBeNull()
  await replayTick("elsewhere")
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("run changed in another window")
})
it("announces the two-minute unreachable fallback", async () => {
  vi.mocked(api.replay).mockRejectedValue(new Error("Offline"))
  await render(); await advance(126_000)
  expect(dataSource.get()).toBe("live")
  expect(host.querySelector('[role="alert"]')?.textContent).toContain("unreachable for two minutes")
})
