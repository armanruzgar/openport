import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { api } from "../test/action-api"
import { liveState, marketNow, useLive } from "../api/live"
import type { ReplayListing, ReplayState, Status, Tick } from "../api/types"
import { DemoPrompt } from "../components/DemoPrompt"
import { AsOf } from "../components/AsOf"
import { ReplayBanner } from "../components/ReplayBanner"
import { activeAccount, MAIN_ACCOUNT } from "../lib/active-account"
import { dataSource } from "../lib/data-source"
import { createPromptStore, demoPrompt, idleReason, nothingTrades } from "../lib/demo"
import { portfolio, status } from "../test/trading-fixtures"
import { replayClock, ReplayView } from "./ReplayView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode, listing?: ReplayListing) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  if (listing) client.setQueryData(["replay-listing"], listing)
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
const replay: ReplayState = { file: "cboe-2026-09-22T133000Z.oprec", provider: "cboe", symbols: ["SPX", "SPY"], started: "2026-09-22T13:30:00.000Z",
  delay_seconds: 900, speed: 10, paused: false, finished: false, time: "2026-09-22T19:04:31.000Z" }
const recordings: ReplayListing = { directory: "/home/trader/.openport/recordings", replay: null, recordings: [
  { file: replay.file, bytes: 48_300_000, provider: "cboe", symbols: ["SPX", "SPY"], started: replay.started, delay_seconds: 900 },
  { file: "broken.oprec", bytes: 10, error: "recording truncated" },
  { file: "generated.oprec", bytes: 100, provider: "demo", simulated: true },
  { file: "databento-2026-09-22-imported.oprec", bytes: 100, provider: "databento", imported: true },
] }
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(status, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks(); vi.unstubAllGlobals(); dataSource.set("live"); activeAccount.set(MAIN_ACCOUNT) })

describe("trading a replay", () => {
  it("sends every route to the replay, but not its controls or an account", async () => {
    const fetcher = vi.fn(async () => new Response(JSON.stringify(portfolio), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    activeAccount.set("swing-50k")
    dataSource.set("replay")
    await api.portfolio()
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay/portfolio", expect.anything())
    await api.summary("SPX")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay/underlyings/SPX/summary", expect.anything())
    await api.status()
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay/status", expect.anything())
    await api.replay()
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.anything())
    await api.controlReplay({ paused: true }, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ method: "PUT", body: JSON.stringify({ paused: true }) }))
    dataSource.set("live")
    await api.portfolio()
    expect(fetcher).toHaveBeenLastCalledWith("/api/portfolio?account=swing-50k", expect.anything())
  })

  it("shows the replay's tick and state instead of the live one", () => {
    const replayTick = { type: "replay_tick", trading: status.trading, feed: { state: "delayed", message: "replay" },
      underlyings: [], engine: status.engine, replay } as unknown as Tick
    const state = liveState(status, replayTick, "open", 0, MAIN_ACCOUNT, () => {}, "replay", replay)
    expect(state.source).toBe("replay")
    expect(state.replay).toBe(replay)
    expect(state.tick).toBe(replayTick)
    expect(replayClock(replay.time)).toBe("Tue, Sep 22, 15:04:31 ET")
    expect(replayClock(null)).toBe("Not started")
    // Ages and times to expiry run on the replay's clock, not today's.
    expect(marketNow(state)).toBe(Date.parse(replay.time!))
    const live = liveState(status, null, "open", 0, MAIN_ACCOUNT, () => {}, "live", replay)
    expect(Math.abs(marketNow(live) - Date.now())).toBeLessThan(1_000)
    expect(render(<AsOf asOf="2026-09-22T19:04:01.000Z" now={Date.parse(replay.time!)} />)).toContain("15:04 ET, &lt;1 min old")
  })
})

describe("the replay page", () => {
  it("lists recordings to replay, with a starting speed", () => {
    const html = render(<ReplayView />, recordings)
    expect(html).toContain("Trade a recorded day with its own paper account")
    expect(html).toContain(replay.file)
    expect(html).toContain("SPX, SPY")
    expect(html).toContain("48.3 MB")
    expect(html).toContain("demo · simulated")
    expect(html).toContain("cboe · recording")
    expect(html).toContain("databento · imported")
    expect(html).toContain("Tue, Sep 22, 09:30:00 ET")
    expect(html).toContain("recording truncated")
    expect(html).toContain(`aria-label="Replay ${replay.file}"`)
    expect(html).toContain('aria-label="Starting speed"')
    expect(html).not.toContain("Now replaying")
  })

  it("explains how to record when there is nothing to replay", () => {
    const html = render(<ReplayView />, { ...recordings, recordings: [] })
    expect(html).toContain("--record-dir /home/trader/.openport/recordings")
  })

  it("labels an active historical import with its source provider", () => {
    const imported = { ...replay, provider: "thetadata", imported: true }
    const html = render(<ReplayView />, { ...recordings, replay: imported })
    expect(html).toContain("thetadata · imported · SPX, SPY")
    expect(html).not.toContain("simulated prices, not market data")
  })

  it("controls a running replay and offers to trade it", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, MAIN_ACCOUNT, () => {}, "live", replay))
    const html = render(<ReplayView />, { ...recordings, replay })
    expect(html).toContain("Now replaying")
    expect(html).toContain("Tue, Sep 22, 15:04:31 ET")
    expect(html).toContain('aria-label="Replay speed"')
    expect(html).toContain(">Pause</button>")
    expect(html).toContain(">Skip gap</button>")
    expect(html).toContain("Trade this replay")
    expect(html).toContain(`aria-label="Replay ${replay.file}">Restart</button>`)
  })
})

const reversal = { goal: "Stay above the floor", session: "regular" as const, seed: "81723", id: "reversal", title: "Slide and rebound", description: "SPX slides about 1% into late morning, then rallies into the close.",
  provider: "demo", symbols: ["SPX", "SPY", "QQQ"], started: "2026-09-16T13:30:00.000Z" }
const overnight = { ...reversal, session: "overnight" as const, id: "overnight", title: "Overnight session", description: "SPX options in Cboe's global trading hours.",
  symbols: ["SPX"], started: "2026-09-16T00:15:00.000Z" }
const demoListing: ReplayListing = { ...recordings, recordings: [], demo: reversal, demos: [reversal, overnight] }
const demoReplay: ReplayState = { ...replay, file: "Demo market: Slide and rebound", provider: "demo", demo: true, delay_seconds: 0 }
const idle = { ...status, underlyings: status.underlyings.map((u) => ({ ...u,
  paper: { accepting: false, reason: "FEED_STALLED", message: `${u.symbol} quotes are behind`, session: "global" as const } })) }

describe("the demo market", () => {
  it("is offered on the replay page and plays as a simulated day", () => {
    const html = render(<ReplayView />, demoListing)
    expect(html).toContain("Demo market")
    expect(html).toContain("Stay above the floor")
    expect(html).toContain("simulated · overnight")
    expect(html).toContain('aria-label="Replay plan"')
    expect(html).toContain('aria-label="Start at"')
    expect(html).toContain('aria-label="Seed mode"')
    expect(html).toContain("Pause at start")
    expect(html).toContain("Simulated trading days in SPX, SPY and QQQ options")
    expect(html).toContain("not market data")
    expect(html).toContain("SPX slides about 1% into late morning")
    expect(html).toContain('aria-label="Start Slide and rebound">Start</button>')
    expect(html).toContain('aria-label="Start Overnight session">Start</button>')
    expect(html).toContain('aria-label="Starting speed"')
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, MAIN_ACCOUNT, () => {}, "live", demoReplay))
    const playing = render(<ReplayView />, { ...demoListing, replay: demoReplay })
    expect(playing).toContain("Demo market: Slide and rebound · simulated prices, not market data")
    expect(playing).toContain('aria-label="Restart Slide and rebound">Restart</button>')
    expect(playing).toContain('aria-label="Start Overnight session">Start</button>')
  })

  it("is labelled simulated in the banner", () => {
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, MAIN_ACCOUNT, () => {}, "replay", demoReplay))
    const html = render(<ReplayBanner />)
    expect(html).toContain(">Demo</span>")
    expect(html).toContain("Simulated prices, not market data")
    expect(html).toContain(demoReplay.file)
  })

  it("starts from the API with demo instead of a file", async () => {
    const fetcher = vi.fn(async () => new Response(JSON.stringify({ replay: demoReplay }), { status: 201 }))
    vi.stubGlobal("fetch", fetcher)
    await api.startReplay({ demo: true }, 10, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ method: "POST", body: JSON.stringify({ demo: true, speed: 10 }) }))
    await api.startReplay({ demo: "selloff" }, 60, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ body: JSON.stringify({ demo: "selloff", speed: 60 }) }))
    await api.startReplay({ file: replay.file }, 1, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ body: JSON.stringify({ file: replay.file, speed: 1 }) }))
  })

  it("is offered under the header only while nothing on the live feed trades", () => {
    const store = createPromptStore(() => { throw new Error("blocked") })
    expect(store.get()).toBe(false)
    expect(render(<DemoPrompt onNavigate={() => {}} />, demoListing)).toBe("")
    vi.mocked(useLive).mockReturnValue(liveState(idle as Status, null, "open"))
    const html = render(<DemoPrompt onNavigate={() => {}} />, demoListing)
    expect(html).toContain("The market data feed has stalled. The demo market plays a simulated day in SPX, SPY and QQQ options")
    expect(html).toContain(">Try the demo</button>")
    expect(html).toContain(">Not now</button>")
    // Not while a replay runs, nor on a server without the demo.
    expect(render(<DemoPrompt onNavigate={() => {}} />, { ...demoListing, replay: demoReplay })).toBe("")
    expect(render(<DemoPrompt onNavigate={() => {}} />, { ...demoListing, demo: null })).toBe("")
    demoPrompt.hide()
    expect(render(<DemoPrompt onNavigate={() => {}} />, demoListing)).toBe("")
  })

  it("says why nothing trades", () => {
    const closed = { accepting: false, reason: "SESSION_CLOSED", message: null, session: "closed" as const }
    const stalled = { accepting: false, reason: "FEED_STALLED", message: null, session: "global" as const }
    const open = { accepting: true, reason: null, message: null, session: "regular" as const }
    const halted = { accepting: false, reason: "MARKET_HALTED", message: null, session: "regular" as const }
    const u = (symbol: string, paper: typeof closed | typeof stalled | typeof open | typeof halted) =>
      ({ ...status.underlyings[0]!, symbol, paper })
    expect(nothingTrades([])).toBe(false)
    expect(nothingTrades([u("SPX", halted), u("SPY", halted)])).toBe(false)  // a halt lasts minutes
    expect(nothingTrades([u("SPX", closed), u("SPY", open)])).toBe(false)
    expect(nothingTrades([u("SPX", stalled), u("SPY", closed)])).toBe(true)
    expect(idleReason([u("SPX", closed), u("SPY", closed)])).toBe("Options markets are closed")
    expect(idleReason([u("SPX", stalled)])).toBe("The market data feed has stalled")
    expect(idleReason([u("SPX", stalled), u("SPY", closed), u("QQQ", closed)])).toBe("Options markets are closed and the SPX feed has stalled")
    // Without paper trading, sessions decide.
    const session = { ...status.underlyings[0]!, paper: null, session: { name: "closed" as const, open: false, note: "closed (weekend)" } }
    expect(nothingTrades([session])).toBe(true)
    expect(idleReason([session])).toBe("Options markets are closed")
  })
})

describe("the replay banner", () => {
  it("shows while trading a replay, with the way back", () => {
    expect(render(<ReplayBanner />)).toBe("")
    vi.mocked(useLive).mockReturnValue(liveState(status, null, "open", 0, MAIN_ACCOUNT, () => {}, "replay", { ...replay, paused: true }))
    const html = render(<ReplayBanner />)
    expect(html).toContain("Tue, Sep 22, 15:04:31 ET")
    expect(html).toContain("paused")
    expect(html).toContain(">Resume</button>")
    expect(html).not.toContain("Skip gap")
    expect(html).toContain("Back to live")
  })
})

it("backtest comparison and saved-run mutations keep global routes in replay history", async () => {
  const fetcher = vi.fn(async () => new Response("{}", { status: 200 }))
  vi.stubGlobal("fetch", fetcher)
  dataSource.set("history:saved")
  await api.compareBacktests(["000001", "000002"])
  expect(fetcher).toHaveBeenLastCalledWith("/api/backtests/compare?ids=000001,000002", expect.anything())
  await api.keepBacktest("000001", true, "open")
  expect(fetcher).toHaveBeenLastCalledWith("/api/backtests/000001", expect.objectContaining({ method: "PUT", body: '{"keep":true}' }))
  await api.deleteBacktest("000002", "open")
  expect(fetcher).toHaveBeenLastCalledWith("/api/backtests/000002?purge=true", expect.objectContaining({ method: "DELETE" }))
})

it("keeps F32 requests on the replay route with their exact wire fields", async () => {
  const fetcher = vi.fn(async () => new Response(JSON.stringify({ replay }), { status: 200 }))
  vi.stubGlobal("fetch", fetcher)
  dataSource.set("history:old-run")
  for (const body of [{ until: "+15s" }, { until: "next" }, { play_until: "10:30:15", speed: 60 }, { abort: true }, { skip: false }]) {
    await api.controlReplay(body, "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ method: "PUT", body: JSON.stringify(body) }))
  }
  await api.restartReplay("old-run", "10:30:15", "open")
  expect(fetcher).toHaveBeenLastCalledWith("/api/replay", expect.objectContaining({ method: "POST", body: JSON.stringify({ restart: "old-run", at: "10:30:15" }) }))
})
