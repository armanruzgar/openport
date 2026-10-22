// @vitest-environment jsdom
import { act, useState } from "react"
import { createRoot } from "react-dom/client"
import { describe, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { PlanEditor, customPlan, planForm } from "../components/PlanEditor"
import type { AccountRules, EventWindow } from "../api/trading-types"
import { account } from "../test/trading-fixtures"
import { planEntryNotice, timeRuleFacts, timeRuleNotices } from "./plan-rules"
import { reasonEvidence } from "./orders"
vi.mock("../api/live", () => ({ useLive: () => ({ underlyings: [{ symbol: "SPY" }] }) }))
const rules: AccountRules = { ...account.rules, events: [{ kind: "news", time: "2026-09-22T14:00:00Z", symbol: "SPY", label: "CPI" }],
  news_before_minutes: 5, news_after_minutes: 10, news_action: "flatten", hold_restrictions: ["weekend", "earnings"], hold_cutoff: "15:45" }
const cutoff: EventWindow = { kind: "earnings", symbol: "SPY", label: "Results", start: "2026-09-22T19:45:00Z", end: "2026-09-22T21:00:00Z", active: true }
describe("F17/F59 calendar rules", () => {
  it("blocks scoped news openings at the inclusive start, allows reductions and exclusive end", () => {
    expect(planEntryNotice(rules, "SPY", "2026-09-22T13:54:59Z", false)).toBeNull()
    expect(planEntryNotice(rules, "SPY", "2026-09-22T13:55:00Z", false)).toContain("NEWS_BLACKOUT")
    expect(planEntryNotice(rules, "SPX", "2026-09-22T14:00:00Z", false)).toBeNull()
    expect(planEntryNotice(rules, "SPY", "2026-09-22T14:00:00Z", true)).toBeNull()
    expect(planEntryNotice(rules, "SPY", "2026-09-22T14:10:00Z", false)).toBeNull()
  })
  it("uses server calendar windows to block holdings and explain the next restriction", () => {
    const evaluation = { ...account.evaluation, next_event: cutoff, active_events: [cutoff] }
    expect(planEntryNotice(rules, "SPY", cutoff.start, false, { ...account, evaluation })).toContain("HOLD_RESTRICTED")
    expect(planEntryNotice(rules, "SPY", cutoff.start, true, { ...account, evaluation })).toBeNull()
    expect(planEntryNotice(rules, "SPX", cutoff.start, false, { ...account, evaluation })).toBeNull()
    expect(planEntryNotice(rules, "SPY", cutoff.end, false, { ...account, evaluation })).toBeNull()
    expect(timeRuleNotices(evaluation, rules).join(" ")).toContain("Active earnings holding cutoff")
    expect(timeRuleFacts(rules).join(" ")).toContain("5 minutes before / 10 after")
    expect(reasonEvidence({ code: "HOLD_RESTRICTED", message: "cutoff", limit: "15:45", scope: "earnings:SPY" })).toBe("Limit 15:45 · earnings:SPY")
  })
  it.each(["FLAT_TIME", "OUTSIDE_PLAN_HOURS", "HOLD_RESTRICTED"])("renders %s clock evidence without numeric conversion", (code) => {
    expect(reasonEvidence({ code, message: "cutoff", actual: "10:00", limit: "15:45", scope: "account" }))
      .toBe("10:00 against a limit of 15:45 · account")
  })
  it("formats saved event facts and active calendar notices in New York time", () => {
    expect(timeRuleFacts({ ...rules, events: [
      { kind: "news", time: "2026-10-07T18:00:00Z", label: "FOMC" },
      { kind: "earnings", time: "2026-10-08", symbol: "SPY", session: "after_close" },
    ] }).join(" ")).toContain("2 saved events: FOMC all underlyings Wed, Oct 7, 14:00 ET; earnings SPY 2026-10-08 after close")
    const news: EventWindow = { kind: "news", symbol: null, label: "FOMC", start: "2026-10-02T19:40:00.000Z", end: "2026-10-02T22:00:00.000Z", active: true }
    const weekend: EventWindow = { kind: "weekend", symbol: null, label: "Weekend", start: "2026-10-02T19:40:00.000Z", end: "2026-10-05T13:30:00.000Z", active: false }
    expect(timeRuleNotices({ ...account.evaluation, active_events: [news, news], next_event: news }, rules)).toEqual([
      "Active news blackout: FOMC, all underlyings, until Fri, Oct 2, 18:00 ET. Closing orders still work.",
    ])
    expect(timeRuleNotices({ ...account.evaluation, active_events: [], next_event: weekend }, rules)).toEqual([
      "Next weekend holding cutoff: Weekend, all underlyings, Fri, Oct 2, 15:40 ET until Mon, Oct 5, 09:30 ET.",
    ])
  })
  it("round trips and validates editor fields", () => {
    const form = planForm({ initial_cash: "100000", rules })
    const result = customPlan(form, rules)
    expect(result).toMatchObject({ rules: { events: rules.events, news_action: "flatten", hold_cutoff: "15:45", hold_restrictions: rules.hold_restrictions } })
    for (const patch of [{ news_before_minutes: "241" }, { news_after_minutes: "1.5" }, { hold_cutoff: "17:00" },
      { events: [{ kind: "split" as const, time: "2026-09-22" }] },
      { events: [{ kind: "earnings" as const, time: "2026-02-30", symbol: "SPY" }] }])
      expect(customPlan({ ...form, ...patch }, rules)).toHaveProperty("error")
  })
  it("preserves all combined rules and gives flat time priority in tickets", () => {
    const combined: AccountRules = { ...rules, flat_time: "15:45", no_overnight: true,
      scaling: [{ profit: "0.00", contracts: 2 }, { profit: "1500.000001", contracts: 4 }],
      time_limit_days: 30, inactivity_days: 14, underlyings: ["SPY"], trading_start: "09:30", trading_end: "16:00",
      max_contracts_held: 5, require_stop_loss: true, max_trade_risk: "123.456789", max_trade_risk_percent: 25 }
    const result = customPlan(planForm({ initial_cash: "100000", rules: combined }), combined)
    expect(result).toMatchObject({ rules: { ...combined, plan: "Custom plan", plan_id: null } })
    const evaluation = { ...account.evaluation, flat_now: true, next_event: cutoff, active_events: [cutoff] }
    const current = { ...account, time: cutoff.start, evaluation }
    expect(planEntryNotice(combined, "SPY", current.time, false, current)).toContain("FLAT_TIME")
    expect(planEntryNotice(combined, "SPY", current.time, true, current)).toBeNull()
    const facts = timeRuleFacts(combined).join(" ")
    expect(facts).toContain("Flat by 15:45")
    expect(facts).toContain("OVERNIGHT_HOLD")
    expect(facts).toContain("News:")
    expect(facts).toContain("holding")
    const notices = timeRuleNotices(evaluation, combined, current.time).join(" ")
    expect(notices).toContain("Flat time passed")
    expect(notices).toContain("earnings holding cutoff")
  })
  it("adds/removes rows and imports the daemon and known dividend calendars", async () => {
    vi.spyOn(api, "calendarEvents").mockResolvedValue({ events: [{ kind: "news", time: "2026-10-01T14:00:00Z", label: "FOMC" }] })
    vi.spyOn(api, "summary").mockResolvedValue({ symbol: "SPY", expiries: [{ dividends: [{ ex_date: "2026-10-02", amount: 1 }] }] } as Awaited<ReturnType<typeof api.summary>>)
    function Editor() { const [form, setForm] = useState(planForm({ initial_cash: "100000", rules: account.rules })); return <PlanEditor form={form} onChange={setForm} /> }
    vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
    const host = document.createElement("div"), root = createRoot(host)
    document.body.append(host)
    const click = async (text: string) => {
      const button = [...host.querySelectorAll("button")].find((b) => b.textContent === text)
      expect(button).toBeDefined()
      await act(async () => { button!.click() })
    }
    try {
      await act(async () => root.render(<Editor />))
      await click("Add event")
      expect(host.textContent).toContain("Event 1 kind")
      await click("Remove event 1")
      await click("Import server calendar")
      expect([...host.querySelectorAll("input")].some((e) => e.value === "FOMC")).toBe(true)
      await click("Import known dividend ex-dates")
      expect([...host.querySelectorAll("input")].some((e) => e.value === "Dividend ex-date")).toBe(true)
      expect([...host.querySelectorAll("input")].some((e) => e.value === "2026-10-02")).toBe(true)
    } finally { await act(async () => root.unmount()); host.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals() }

  })
  it("keeps edits made while an import loads", async () => {
    let finish!: (value: Awaited<ReturnType<typeof api.calendarEvents>>) => void
    vi.spyOn(api, "calendarEvents").mockReturnValue(new Promise((resolve) => { finish = resolve }))
    let form = planForm({ initial_cash: "100000", rules: account.rules })
    vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
    const host = document.createElement("div"), root = createRoot(host)
    const draw = () => root.render(<PlanEditor form={form} onChange={(next) => { form = next; draw() }} />)
    try {
      await act(async () => draw())
      await act(async () => [...host.querySelectorAll("button")].find((b) => b.textContent === "Import server calendar")!.click())
      await act(async () => { form = { ...form, profit_target: "7777" }; draw() })
      await act(async () => finish({ events: [{ kind: "news", time: "2026-10-01T14:00:00Z", label: "FOMC" }] }))
      expect(form.profit_target).toBe("7777")
      expect(form.events).toHaveLength(1)
    } finally { await act(async () => root.unmount()); vi.restoreAllMocks(); vi.unstubAllGlobals() }
  })
})
