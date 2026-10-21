// @vitest-environment jsdom
import { afterEach, expect, it, vi } from "vitest"
import { captureWorkspace, defaultChainPrefs, loadChainPrefs, loadWorkspaces, restoreWorkspace, saveChainPrefs, saveWorkspaces } from "./chain-prefs"
import { loadChartPrefs, saveChartPrefs } from "./candles"
import { loadDrawings, saveDrawings, drawingIndex } from "./drawings"
import { defaultStudies } from "./studies"

afterEach(() => { vi.unstubAllGlobals(); localStorage.clear() })
it("persists chain preferences independently per symbol and validates old or corrupt values", () => {
  saveChainPrefs("SPX", { ...defaultChainPrefs, window: 0, greeks: true, mode: "strategy", stacked: true })
  expect(loadChainPrefs("spx")).toMatchObject({ window: 0, greeks: true, mode: "strategy", stacked: true })
  expect(loadChainPrefs("SPY")).toEqual(defaultChainPrefs)
  localStorage.setItem("openport.chain.SPX", '{"window":-1,"liquidity":"false","mode":"invalid"}')
  expect(loadChainPrefs("SPX")).toEqual(defaultChainPrefs)
  localStorage.setItem("openport.chain.SPX", "bad json")
  expect(loadChainPrefs("SPX")).toEqual(defaultChainPrefs)
})
it("restores a named workspace including route, chain and chart preferences", () => {
  const route = { symbol: "SPX", view: "chain" as const, expiry: "2026-10-16PM" }
  saveChartPrefs({ interval: "1h", hidden: false, studies: { ...defaultStudies, sma: true } })
  saveChainPrefs("SPX", { ...defaultChainPrefs, window: 0 })
  const workspace = captureWorkspace("Index desk", route)
  saveWorkspaces([workspace])
  expect(loadWorkspaces()).toEqual([workspace])
  saveChainPrefs("SPX", defaultChainPrefs)
  restoreWorkspace(workspace)
  expect(window.location.hash).toBe("#/SPX/chain/2026-10-16PM")
  expect(loadChainPrefs("SPX").window).toBe(0)
  expect(loadChartPrefs().studies?.sma).toBe(true)
})
it("keeps malformed workspaces and drawings out and limits history", () => {
  localStorage.setItem("openport.workspaces", "{}")
  expect(loadWorkspaces()).toEqual([])
  localStorage.setItem("openport.drawings.SPX", '[null,{}, {"id":"bad","kind":"horizontal","price":-1}]')
  expect(loadDrawings("SPX")).toEqual([])
  saveDrawings("SPX", [{ id: "line", kind: "horizontal", price: 7000 }])
  expect(loadDrawings("SPX")).toHaveLength(1)
  expect(loadDrawings("SPY")).toEqual([])
  expect(drawingIndex([100, 200, 400], 300)).toBe(1.5)
  expect(drawingIndex([100, 200], 99)).toBeNull()
})
it("falls back safely when browser storage is blocked", () => {
  vi.stubGlobal("localStorage", { getItem: () => { throw Error("blocked") }, setItem: () => { throw Error("quota") } })
  expect(loadChainPrefs("SPX")).toEqual(defaultChainPrefs)
  expect(loadWorkspaces()).toEqual([])
  expect(loadDrawings("SPX")).toEqual([])
  expect(() => { saveChainPrefs("SPX", defaultChainPrefs); saveWorkspaces([]); saveDrawings("SPX", []) }).not.toThrow()
})
