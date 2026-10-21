// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { api } from "../api/client"
import { liveState, useLive } from "../api/live"
import type { ExposureMatrix } from "../api/types"
import { waitForRender } from "../test/render"
import { status } from "../test/trading-fixtures"
import { ExposureView } from "./ExposureView"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../charts/BarChart", () => ({ BarChart: (props: unknown) => <div data-testid="bars">{JSON.stringify(props)}</div> }))
vi.mock("../charts/Heatmap", () => ({ Heatmap: (props: unknown) => <div data-testid="heatmap">{JSON.stringify(props)}</div> }))

const level = { flip: 101, positive_wall: 110, negative_wall: 90 }
const data: ExposureMatrix = {
  symbol: "SPX", spot: 100, as_of: "2026-10-22T19:00:00Z", version: 1, convention: "long_calls_short_puts", slice: "all",
  strikes: [90, 110], expiries: [{ id: "2026-10-22PM", expiry: "2026-10-22", days: .1,
    gex: [-1, 2], vex: [-3, 4], dex: [5, 6], cex: [-7, 8], vomex: [-9, 10] }], total_gex: [-1, 2],
  exposure: { gex: 1, vex: 1, dex: 11, cex: 1, vomex: 1, gamma_flip: 101, call_wall: 110, put_wall: 90,
    oi_coverage: 1, available: true, unavailable_reason: null,
    levels: { gex: level, vex: level, dex: { ...level, flip: null, negative_wall: null }, cex: level, vomex: level } },
}
let root: Root
let host: HTMLDivElement
let client: QueryClient
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div")
  document.body.append(host)
  root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity, staleTime: Infinity } } })
})
afterEach(async () => {
  await act(async () => root.unmount())
  client.clear()
  host.remove()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})
async function render() {
  await act(async () => root.render(<QueryClientProvider client={client}><ExposureView symbol="SPX" /></QueryClientProvider>))
  await waitForRender(() => expect(host.textContent).toContain("Net GEX"))
}
async function click(text: string) {
  const button = [...host.querySelectorAll("button")].find((button) => button.textContent === text)!
  await act(async () => button.click())
}
async function convention(value: string) {
  const select = host.querySelector("select")!
  await act(async () => { select.value = value; select.dispatchEvent(new Event("change", { bubbles: true })) })
}

it("switches every metric's cells, units and levels without replacing missing flips", async () => {
  vi.spyOn(api, "exposure").mockResolvedValue(data)
  await render()
  for (const [metric, name, values] of [
    ["GEX", "Gamma", [-1, 2]], ["VEX", "Vanna", [-3, 4]], ["DEX", "Delta", [5, 6]],
    ["CEX", "Charm", [-7, 8]], ["VOMEX", "Vomma", [-9, 10]],
  ] as const) {
    await click(metric)
    const bars = JSON.parse(host.querySelector('[data-testid="bars"]')!.textContent!)
    expect(bars.bars.map((bar: { value: number }) => bar.value)).toEqual(values)
    expect(bars.markers.some((marker: { label: string }) => marker.label === "flip")).toBe(metric !== "DEX")
    expect(host.textContent).toContain(`${name} flip`)
    const heatmap = JSON.parse(host.querySelector('[data-testid="heatmap"]')!.textContent!)
    expect(heatmap.values).toEqual([values])
  }
  expect(host.textContent).toContain("dollars per vol point squared")
  expect(api.exposure).toHaveBeenCalledTimes(1)
})

it("requests conventions and slices without showing the preceding mode during loading", async () => {
  const request = vi.spyOn(api, "exposure").mockResolvedValue(data)
  await render()
  let resolve!: (value: ExposureMatrix) => void
  request.mockImplementationOnce(() => new Promise((done) => { resolve = done }))
  await click("0DTE only")
  await waitForRender(() => expect(request).toHaveBeenLastCalledWith("SPX", 8, .05, expect.any(AbortSignal), "long_calls_short_puts", "0dte"))
  expect(host.textContent).toContain("Loading exposure")
  expect(host.querySelector('[data-testid="bars"]')).toBeNull()
  await act(async () => resolve({ ...data, slice: "0dte" }))
  await waitForRender(() => expect(host.textContent).toContain("Same-day expiries"))
  request.mockResolvedValue({ ...data, slice: "ex_0dte" })
  await click("Ex-0DTE")
  await waitForRender(() => expect(request).toHaveBeenLastCalledWith("SPX", 8, .05, expect.any(AbortSignal), "long_calls_short_puts", "ex_0dte"))
  request.mockResolvedValue({ ...data, convention: "short_all", slice: "ex_0dte" })
  await convention("short_all")
  await waitForRender(() => expect(request).toHaveBeenLastCalledWith("SPX", 8, .05, expect.any(AbortSignal), "short_all", "ex_0dte"))
})

it("shows unavailable flow without charts and keeps controls usable", async () => {
  const request = vi.spyOn(api, "exposure").mockResolvedValue(data)
  await render()
  request.mockResolvedValue({ ...data, convention: "customer_flow", exposure: { ...data.exposure,
    gex: null, vex: null, dex: null, cex: null, vomex: null, levels: undefined,
    gamma_flip: null, call_wall: null, put_wall: null,
    available: false, unavailable_reason: "Customer flow unavailable: no aggressor side" } })
  await convention("customer_flow")
  await waitForRender(() => expect(host.textContent).toContain("Customer flow unavailable: no aggressor side"))
  expect(host.querySelector('[data-testid="bars"]')).toBeNull()
  expect(host.querySelector('[data-testid="heatmap"]')).toBeNull()
  expect(host.querySelector("select")).not.toBeNull()
  await convention("long_calls_short_puts")
  await waitForRender(() => expect(host.querySelector('[data-testid="bars"]')).not.toBeNull())
})


it("does not relabel an older server's default publication as another convention", async () => {
  vi.spyOn(api, "exposure").mockResolvedValue({ ...data, convention: undefined, slice: undefined })
  await render()
  await convention("short_all")
  await waitForRender(() => expect(host.textContent).toContain("This server did not return the selected exposure convention and slice"))
  expect(host.querySelector('[data-testid="bars"]')).toBeNull()
})

it("offers one default convention and accepts an older server's deprecated default spelling", async () => {
  const older = { ...data, convention: "open_interest" } as unknown as ExposureMatrix
  vi.spyOn(api, "exposure").mockResolvedValue(older)
  await render()
  const options = [...host.querySelectorAll("option")].map((option) => option.value)
  expect(options).toEqual(["long_calls_short_puts", "short_all", "customer_flow"])
  expect(host.querySelector('[data-testid="bars"]')).not.toBeNull()
  expect(host.textContent).not.toContain("This server did not return the selected exposure convention and slice")
})
