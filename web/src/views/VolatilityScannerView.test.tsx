// @vitest-environment jsdom
import { act } from "react"
import { createRoot } from "react-dom/client"
import { expect, it, vi } from "vitest"
import type { VolatilityScannerRow } from "../api/types"
import { sortedScanner, VolatilityScannerTable } from "./VolatilityScannerView"

const row = (symbol: string, rr25: number | null): VolatilityScannerRow => ({ symbol, rr25, iv30: 20, iv_basis: "own_atm", iv_rank: .2, iv_percentile: .4,
  history_sessions: 12, proxy_sessions: 10, proxy_name: "VIX", vrp: 3, rr25_rank: .25, rr25_sessions: 2, term_ratio: 1.1,
  today_percent: null, today_points: null, as_of: null, realized_as_of: "2026-09-21", version: 1, feed_state: "delayed", truncated: true, proxy: true, reason: null })
it("sorts signed values and always leaves missing metrics last without mutating input", () => {
  const rows = [row("SPY", null), row("SPX", -4), row("QQQ", -1)]
  expect(sortedScanner(rows, "rr25", false).map(r => r.symbol)).toEqual(["SPX", "QQQ", "SPY"])
  expect(sortedScanner(rows, "rr25", true).map(r => r.symbol)).toEqual(["QQQ", "SPX", "SPY"])
  expect(rows[0]!.symbol).toBe("SPY")
})
it("sorts by keyboard-accessible column controls and opens a symbol", async () => {
  const host = document.createElement("div")
  const root = createRoot(host)
  const onSymbol = vi.fn()
  await act(async () => root.render(<VolatilityScannerTable data={{ rows: [row("SPX", -4), row("QQQ", -1)], simulated: true, provider: "demo", units: "vol_points" }} onSymbol={onSymbol} />))
  expect(host.textContent).toContain("Simulated data")
  expect(host.textContent).toContain("ATM")
  expect(host.textContent).toContain("10 VIX proxy")
  expect(host.textContent).toContain("RV through 2026-09-21")
  const button = [...host.querySelectorAll("button")].find(b => b.textContent === "RR25")!
  await act(async () => button.click())
  expect(button.closest("th")!.getAttribute("aria-sort")).toBe("descending")
  const first = host.querySelector("tbody button") as HTMLButtonElement
  expect(first.textContent).toBe("QQQ")
  await act(async () => first.click())
  expect(onSymbol).toHaveBeenCalledWith("QQQ")
  await act(async () => root.unmount())
})
