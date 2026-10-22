// @vitest-environment jsdom
import { act } from "react"
import { createRoot } from "react-dom/client"
import { afterEach, expect, it, vi } from "vitest"
import { expiry } from "../test/trading-fixtures"
import { ExpiryPicker } from "./ExpiryPicker"

afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals() })

it("scrolls an off-screen selection into the strip on a page with a sidebar", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  // jsdom has no layout: the strip sits 260px into the page, 900px wide, with 80px tabs.
  vi.spyOn(HTMLElement.prototype, "offsetLeft", "get").mockImplementation(function (this: HTMLElement) {
    return this.getAttribute("role") === "tablist" ? 260 : [...this.parentElement!.children].indexOf(this) * 80
  })
  vi.spyOn(HTMLElement.prototype, "offsetWidth", "get").mockReturnValue(80)
  vi.spyOn(HTMLElement.prototype, "clientWidth", "get").mockReturnValue(900)
  const scrollTo = vi.fn()
  HTMLElement.prototype.scrollTo = scrollTo
  const expiries = Array.from({ length: 16 }, (_, i) => ({ ...expiry, id: `2026-10-${String(i + 1).padStart(2, "0")}PM`, expiry: `2026-10-${String(i + 1).padStart(2, "0")}` }))
  const host = document.createElement("div"), root = createRoot(host)
  try {
    await act(async () => root.render(<ExpiryPicker expiries={expiries} value={expiries[0]!.id} onChange={() => {}} />))
    expect(scrollTo).not.toHaveBeenCalled()
    // The 13th tab starts at 960px, past the strip's 900px.
    await act(async () => root.render(<ExpiryPicker expiries={expiries} value={expiries[12]!.id} onChange={() => {}} />))
    expect(scrollTo).toHaveBeenCalledWith({ left: 960 - 300, behavior: "smooth" })
  } finally { await act(async () => root.unmount()) }
})
