// @vitest-environment jsdom
import { act } from "react"
import { createRoot } from "react-dom/client"
import { afterEach, expect, it, vi } from "vitest"
import type { Candle } from "../api/types"
import { CandleChart } from "./CandleChart"

afterEach(() => vi.unstubAllGlobals())
const open = Date.UTC(2026, 8, 22, 13, 30) / 1000
const bars: Candle[] = Array.from({ length: 400 }, (_, i) => ({ t: open + i * 60, o: 6990 + i / 2, h: 6991 + i / 2, l: 6989.5 + i / 2, c: 6990.25 + i / 2 }))

it("adds up slow sideways swipes smaller than half a bar", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("ResizeObserver", class { observe() {} disconnect() {} })
  const host = document.createElement("div"), root = createRoot(host)
  document.body.append(host)
  try {
    await act(async () => root.render(<CandleChart bars={bars} interval="1m" height={300} width={800} formatPrice={(x) => x.toFixed(2)} label="SPX" />))
    const before = host.querySelector("svg")!.innerHTML
    // About 7px a bar: each 2px swipe is under a third of one.
    for (let i = 0; i < 10; i++)
      await act(async () => { host.querySelector("svg")!.dispatchEvent(new WheelEvent("wheel", { deltaX: -2, bubbles: true, cancelable: true })) })
    expect(host.querySelector("svg")!.innerHTML).not.toBe(before)
  } finally { await act(async () => root.unmount()); host.remove() }
})
