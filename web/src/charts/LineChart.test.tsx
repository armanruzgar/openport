// @vitest-environment jsdom
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { BarChart } from "./BarChart"
import { LineChart } from "./LineChart"

vi.mock("./useSize", () => ({ useSize: () => [{ current: null }, { width: 640, height: 320 }] }))
let host: HTMLDivElement, root: Root
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); vi.unstubAllGlobals()
})
const press = (target: Element, key: string) => act(async () => { target.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true })) })
const live = () => host.querySelector('[aria-live="polite"]')!.textContent
const equity = [{ id: "equity", label: "Equity", color: "red", points: [{ x: 0, y: 100 }, { x: 50, y: 110 }, { x: 100, y: 105 }] },
  { id: "floor", label: "Floor", color: "blue", points: [{ x: 0, y: 90 }, { x: 25, y: null }, { x: 100, y: 95 }] }]

it("names a line chart and reads its points out from the keyboard", async () => {
  await act(async () => root.render(<LineChart series={equity} formatX={(x) => `t${x}`} formatY={(y) => `$${y}`} />))
  const svg = host.querySelector("svg")!
  expect(svg.getAttribute("role")).toBe("img")
  expect(svg.getAttribute("aria-label")).toBe("Equity, Floor")
  expect(svg.getAttribute("tabindex")).toBe("0")
  expect(live()).toBe("")
  await press(svg, "ArrowRight")
  expect(live()).toBe("t0: Equity $100, Floor $90")
  // Floor's x of 25 has no value, so the crosshair passes it by; each series reads its nearest point.
  await press(svg, "ArrowRight")
  expect(live()).toBe("t50: Equity $110, Floor $90")
  await press(svg, "End")
  expect(live()).toBe("t100: Equity $105, Floor $95")
  await press(svg, "Escape")
  expect(live()).toBe("")
  await act(async () => root.render(<LineChart label="Intraday equity" series={equity} formatX={String} formatY={String} />))
  expect(host.querySelector("svg")!.getAttribute("aria-label")).toBe("Intraday equity")
})

it("keeps a draggable marker's slider reachable, bounded by the x axis", async () => {
  const onStep = vi.fn()
  await act(async () => root.render(<LineChart series={equity} formatX={String} formatY={String}
    markers={[{ x: 50, label: "50C", color: "green", onDrag: () => {}, onStep }]} />))
  expect(host.querySelector("svg")!.getAttribute("role")).toBe("group")
  const slider = host.querySelector('[role="slider"]')!
  expect(slider.getAttribute("aria-valuemin")).toBe("0")
  expect(slider.getAttribute("aria-valuemax")).toBe("100")
  expect(slider.getAttribute("aria-valuenow")).toBe("50")
  await press(slider, "ArrowLeft")
  expect(onStep).toHaveBeenCalledWith(-1)
  // The marker's keys move the marker, not the chart's readout.
  expect(live()).toBe("")
})

it("names a bar chart and reads each bar out from the keyboard", async () => {
  await act(async () => root.render(<BarChart bars={[{ x: 6900, value: -5 }, { x: 7000, value: 10 }]} formatX={String} formatY={(y) => `$${y}`} />))
  const svg = host.querySelector("svg")!
  expect(svg.getAttribute("role")).toBe("img")
  expect(svg.getAttribute("aria-label")).toBe("2 bars from 6900 to 7000")
  await press(svg, "ArrowRight")
  expect(live()).toBe("6900: $-5")
  await press(svg, "ArrowRight")
  expect(live()).toBe("7000: $10")
  await press(svg, "Home")
  expect(live()).toBe("6900: $-5")
})
