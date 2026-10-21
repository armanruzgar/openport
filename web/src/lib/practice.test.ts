import { describe, expect, it } from "vitest"
import type { ReplayDemo, ReplayHistory } from "../api/types"
import { shareTrades, trades } from "../test/trading-fixtures"
import { coachFlags, mistakeCosts, tradeBuckets } from "./journal"
import { personalBestLabel, practiceQueue, practiceStreak, scenarioMarkers } from "./practice"

const trade = { ...trades[0]!, opened: "2026-09-22T14:30:00Z", closed: "2026-09-22T15:00:00Z", status: "closed" as const,
  net: "100", entry_context: { spot: 6000, spot_source: "quote", iv: .25, delta: -.4, years: 8 / 365, equity: "25000", floor_room: "1000", buying_power: "500" },
  review: { mae: "-200", mfe: "400", planned_risk: "100", give_back: "300", heat: 2, r_multiple: 1, worst: null, best: null } }
describe("edge reports", () => {
  it("uses entry observations at bucket boundaries, with Unknown for missing evidence", () => {
    for (const [dimension, label] of [["time", "10:30–12:00"], ["dte", "8–30 DTE"], ["delta", "0.40–0.60"], ["iv", "25–40%"], ["underlying", trade.underlying]] as const)
      expect(tradeBuckets([trade], dimension)[0]).toMatchObject({ label, trades: 1, expectancy: 100, averageR: 1, giveBack: 300, profitFactor: Infinity })
    expect(tradeBuckets([{ ...trade, entry_context: null }], "iv")[0]?.label).toBe("Unknown")
    expect(tradeBuckets([{ ...trade, tags: ["playbook:fade@v2", "put-credit-20d-5w"] }], "playbook")[0]?.label).toBe("fade")
    expect(tradeBuckets([{ ...trade, tags: ["put-credit-20d-5w"] }], "template")[0]?.label).toBe("put-credit-20d-5w")
  })
  it("counts breakevens in expectancy, and only recorded R and give-back values", () => {
    const rows = [trade, { ...trade, net: "-50", review: undefined }, { ...trade, net: "0", review: undefined }]
    expect(tradeBuckets(rows, "underlying")[0]).toMatchObject({ trades: 3, winRate: .5, expectancy: 50 / 3, averageR: 1, profitFactor: 2, giveBack: 300 })
  })
  it("keeps fixed mistakes independent of free-text tags and excludes open trades", () => {
    const rows = [trade, { ...trade, net: "-30", tags: ["chased"], mistakes: ["oversized"] }, { ...trade, status: "open" as const, mistakes: ["oversized"] }]
    expect(mistakeCosts(rows).find(r => r.label === "chased")?.with.trades).toBe(0)
    expect(mistakeCosts(rows).find(r => r.label === "oversized")).toMatchObject({ with: { net: -30, trades: 1 }, without: { net: 100, trades: 1 } })
  })
  it("flags only supported breaches, filters scenario symbols, and uses a median cohort", () => {
    const big = { ...trade, entry_order: "42", max_quantity: 10 }
    const peers = [big, { ...trade, max_quantity: 2 }, { ...trade, max_quantity: 3 }]
    const context = { events: [{ type: "crush", time: "2026-09-22T14:45:00Z", symbols: [trade.underlying] }], brokenOrders: new Set(["42"]) }
    expect(coachFlags(big, peers, context)).toEqual(["Gave back >50%", "Heat >1R", "Broke playbook rule", "Held through scenario event", "Size >2× median"])
    expect(tradeBuckets(peers, "coach", "all", context).find(b => b.label === "Size >2× median")?.trades).toBe(1)
    expect(coachFlags({ ...trade, status: "open" }, peers, context)).toEqual([])
    expect(coachFlags({ ...trade, review: undefined }, [], { events: [{ ...context.events[0]!, symbols: ["OTHER"] }] })).toEqual([])
    const shares = { ...shareTrades[0]!, status: "closed" as const, max_shares: 500, opened_shares: 500 }
    expect(coachFlags(shares, [shares, { ...shares, max_shares: 100, opened_shares: 1000 }, { ...shares, max_shares: 100 }])).toEqual(["Size >2× median"])
  })
})
const run = (scenario: string, completed: string, score: number) => ({ scenario, completed_at: `${completed}T18:00:00Z`, seed: "7", plan: "practice", drill: { complete_run: true, score } } as ReplayHistory)
it("schedules spaced fresh practice by recent weakness and real completion days", () => {
  const demos = ["weak", "strong", "new"].map(id => ({ id, title: id, objectives: [{ id: "loss", type: "max_loss", limit: 500 }] } as ReplayDemo))
  const history = [run("weak", "2026-10-20", 20), run("strong", "2026-10-20", 100), run("weak", "2026-10-19", 100)]
  const queue = practiceQueue(demos, history, "2026-10-21", () => "7")
  expect(queue.map(q => q.scenario)).toEqual(["weak", "new"])
  expect(queue[0]?.seed).toBe("8")
  expect(practiceStreak(history, "2026-10-21")).toBe(2)
  expect(practiceStreak(history, "2026-10-22")).toBe(0)
  expect(practiceQueue(demos, [run("weak", "2026-10-21", 20)], "2026-10-21").some(q => q.scenario === "weak")).toBe(false)
})
it("puts authored events on both second and millisecond chart axes without fabricating prices", () => {
  const events = [{ type: "halt", time: "2026-09-22T14:45:00Z", end: "2026-09-22T14:50:00Z", symbols: ["SPX"] }]
  const from = Date.parse(trade.opened), to = Date.parse(trade.closed)
  expect(scenarioMarkers(events, from, to, 1000, "SPX")).toHaveLength(2)
  expect(scenarioMarkers(events, from, to)[0]?.x).toBe(Date.parse(events[0]!.time))
  expect(scenarioMarkers(events, from, to, 1, "QQQ")).toEqual([])
  expect(scenarioMarkers(events, to, to + 60_000)).toEqual([])
})

it("brings a drill back sooner when its underlying matches a weak edge group", () => {
  const demos = [{ id: "crush", title: "Crush", symbols: [trade.underlying], objectives: [{ id: "loss", type: "max_loss", limit: 500 }] } as ReplayDemo]
  const history = [run("crush", "2026-10-20", 100)]
  const weak = tradeBuckets([trade, { ...trade, net: "-200" }, { ...trade, net: "-200" }], "underlying")
  expect(practiceQueue(demos, history, "2026-10-21", () => "9")).toHaveLength(0)
  expect(practiceQueue(demos, history, "2026-10-21", () => "9", weak)[0]).toMatchObject({ scenario: "crush", interval: 1, weak: [trade.underlying] })
})

it("labels a sidecar personal best unverified until a verification recomputes it", () => {
  expect(personalBestLabel({})).toBe("")
  expect(personalBestLabel({ personal_best: 85 })).toBe(" · personal best 85/100 (unverified)")
  expect(personalBestLabel({ personal_best: 85, personal_best_verified: false })).toContain("(unverified)")
  expect(personalBestLabel({ personal_best: 85, personal_best_verified: true })).toBe(" · personal best 85/100 (verified)")
})
