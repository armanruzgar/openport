// @vitest-environment jsdom
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { Heatmap } from "./Heatmap"

// jsdom lays nothing out: give each row 20 px and the scroller a 20 px view.
const scrollTops = new WeakMap<Element, number>()
let host: HTMLDivElement, root: Root
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  Object.defineProperty(HTMLTableRowElement.prototype, "offsetTop", { configurable: true, get() { return (this as HTMLTableRowElement).sectionRowIndex * 20 } })
  Object.defineProperty(HTMLDivElement.prototype, "clientHeight", { configurable: true, get: () => 20 })
  Object.defineProperty(HTMLDivElement.prototype, "scrollTop", { configurable: true,
    get() { return scrollTops.get(this as Element) ?? 0 }, set(value: number) { scrollTops.set(this as Element, value) } })
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); vi.unstubAllGlobals()
  for (const [prototype, key] of [[HTMLTableRowElement.prototype, "offsetTop"], [HTMLDivElement.prototype, "clientHeight"], [HTMLDivElement.prototype, "scrollTop"]] as const)
    delete (prototype as unknown as Record<string, unknown>)[key]
})
// Drawn high to low: 7020 is row 0, 7010 row 1, 7000 row 2.
const rows = [6980, 6990, 7000, 7010, 7020]
const draw = (highlightRow: number) => act(async () => root.render(<Heatmap rows={rows} columns={[{ id: "2026-10-16PM", label: "Oct 16" }]}
  values={[rows.map((strike) => strike - 7000)]} highlightRow={highlightRow} format={String} formatRow={String} />))
const scroller = () => host.querySelector("table")!.parentElement as HTMLDivElement

it("follows the spot row until the trader scrolls, then leaves the view where they put it", async () => {
  await draw(7000)
  expect(scroller().scrollTop).toBe(30)
  await draw(7010)
  expect(scroller().scrollTop).toBe(10)
  scroller().scrollTop = 0
  await draw(7000)
  expect(scroller().scrollTop).toBe(0)
  await draw(7020)
  expect(scroller().scrollTop).toBe(0)
})
