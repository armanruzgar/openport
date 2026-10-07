// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act, type ReactNode } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import type { ReplayListing, Status } from "../api/types"
import { status } from "../test/trading-fixtures"
import { MAIN_ACCOUNT } from "../lib/active-account"
import { DemoPrompt } from "./DemoPrompt"
import { createWelcomeStore, Welcome, welcome } from "./Welcome"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))

/** Every market closed, so the demo is offered. */
const closedStatus = () => ({ ...status, underlyings: status.underlyings.map((u) => ({ ...u,
  paper: { accepting: false, reason: "SESSION_CLOSED", message: `${u.symbol} options are closed`, session: "closed" as const } })) }) as Status
const reversal = { id: "reversal", title: "Slide and rebound", provider: "demo", symbols: ["SPX", "SPY", "QQQ"], started: "2026-09-16T13:30:00.000Z" }
const demoListing: ReplayListing = { directory: "", recordings: [], replay: null, demo: reversal,
  demos: [reversal, { ...reversal, id: "overnight", title: "Overnight session", symbols: ["SPX"] }] }

let root: Root
let host: HTMLDivElement
let client: QueryClient
const wrap = (node: ReactNode) => <QueryClientProvider client={client}>{node}</QueryClientProvider>
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount())
  host.remove()
  client.clear()
  welcome.close()
  vi.unstubAllGlobals()
})

describe("the welcome", () => {
  it("shows once per browser, and again when asked", () => {
    const items = new Map<string, string>()
    const store = createWelcomeStore(() => ({ getItem: (k) => items.get(k) ?? null, setItem: (k, v) => { items.set(k, v) } }))
    expect(store.get()).toBe(true)
    store.close()
    expect(store.get()).toBe(false)
    expect(items.get("openport.welcome")).toBe("seen")
    expect(createWelcomeStore(() => ({ getItem: (k) => items.get(k) ?? null, setItem: () => {} })).get()).toBe(false)
    store.show()
    expect(store.get()).toBe(true)
    // Without storage it waits to be asked rather than returning on every visit.
    expect(createWelcomeStore(() => { throw new Error("blocked") }).get()).toBe(false)
  })

  it("says where the data comes from and starts on the chain", async () => {
    const navigate = vi.fn()
    welcome.show()
    await act(async () => root.render(wrap(<Welcome onNavigate={navigate} />)))
    const text = host.textContent ?? ""
    expect(text).not.toContain("demo")
    expect(text).toContain("Welcome to openport")
    expect(text).toContain(`${status.provider.name}, `)
    expect(text).toContain("nothing reaches an exchange")
    expect(text).toContain("not investment advice")
    const start = [...host.querySelectorAll("button")].find((b) => b.textContent === "Start on the chain")!
    await act(async () => start.click())
    expect(navigate).toHaveBeenCalledWith("chain")
    expect(welcome.get()).toBe(false)
    expect(host.textContent).toBe("")
  })

  it("offers the demo market while nothing trades", async () => {
    const closed = { ...status, underlyings: status.underlyings.map((u) => ({ ...u,
      paper: { accepting: false, reason: "SESSION_CLOSED", message: `${u.symbol} options are closed`, session: "closed" as const } })) }
    vi.mocked(useLive).mockReturnValue(liveState(closed as Status, null, "open"))
    const listing: ReplayListing = { directory: "", recordings: [], replay: null,
      demo: { provider: "demo", symbols: ["SPX", "SPY"], started: "2026-09-16T13:30:00.000Z" } }
    client.setQueryData(["replay-listing"], listing)
    welcome.show()
    await act(async () => root.render(wrap(<Welcome onNavigate={() => {}} />)))
    const text = host.textContent ?? ""
    expect(text).toContain("Options markets are closed. In the meantime the demo market plays a simulated day")
    expect([...host.querySelectorAll("button")].map((b) => b.textContent)).toContain("Try the demo")
  })

  it("stays open to say why the demo did not start, and closes once it does", async () => {
    const switchSource = vi.fn()
    vi.mocked(useLive).mockReturnValue(liveState(closedStatus(), null, "open", 0, MAIN_ACCOUNT, () => {}, "live", null, switchSource))
    client.setQueryData(["replay-listing"], demoListing)
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ error: { code: "INTERNAL", message: "Demo recording failed: disk full" } }), { status: 500 }))
    vi.stubGlobal("fetch", fetcher)
    welcome.show()
    await act(async () => root.render(wrap(<Welcome onNavigate={() => {}} />)))
    const tryIt = () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Try the demo")!
    await act(async () => tryIt().click())
    expect(welcome.get()).toBe(true)
    expect(host.querySelector("[role=alert]")?.textContent).toBe("INTERNAL: Demo recording failed: disk full")
    fetcher.mockImplementation(async () => new Response(JSON.stringify({ replay: null }), { status: 201 }))
    await act(async () => tryIt().click())
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ method: "POST", body: JSON.stringify({ demo: true, speed: 10 }) }))
    expect(switchSource).toHaveBeenCalledWith("replay", "user")
    expect(welcome.get()).toBe(false)
  })
})

describe("the demo prompt", () => {
  it("plays the day picked", async () => {
    vi.mocked(useLive).mockReturnValue(liveState(closedStatus(), null, "open"))
    client.setQueryData(["replay-listing"], demoListing)
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ replay: null }), { status: 201 }))
    vi.stubGlobal("fetch", fetcher)
    await act(async () => root.render(wrap(<DemoPrompt onNavigate={() => {}} />)))
    expect(host.textContent).toContain("plays a simulated day in SPX, SPY and QQQ options")
    const day = host.querySelector<HTMLSelectElement>("select[aria-label='Demo day']")!
    expect([...day.options].map((o) => o.textContent)).toEqual(["Slide and rebound", "Overnight session"])
    await act(async () => { day.value = "overnight"; day.dispatchEvent(new Event("change", { bubbles: true })) })
    expect(host.textContent).toContain("plays a simulated day in SPX options")
    const tryIt = [...host.querySelectorAll("button")].find((b) => b.textContent === "Try the demo")!
    await act(async () => tryIt.click())
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ body: JSON.stringify({ demo: "overnight", speed: 10 }) }))
  })
})
