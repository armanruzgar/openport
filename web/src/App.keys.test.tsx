// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot } from "react-dom/client"
import { afterEach, expect, it, vi } from "vitest"
import { App } from "./App"
import { liveState, useLive } from "./api/live"
import type { Summary } from "./api/types"
import { expiry, status } from "./test/trading-fixtures"

const navigate = vi.fn()
vi.mock("./api/live", async (original) => ({ ...await original<typeof import("./api/live")>(), useLive: vi.fn() }))
// A bookmarked expiry that has since rolled off.
vi.mock("./lib/route", async (original) => ({ ...await original<typeof import("./lib/route")>(),
  useRoute: () => [{ view: "chain", symbol: "SPX", expiry: "2026-10-05PM" }, navigate] }))
vi.mock("./lib/theme", () => ({ useTheme: () => ({ theme: "dark", toggleTheme: vi.fn() }) }))
afterEach(() => { vi.clearAllMocks(); vi.unstubAllGlobals() })

it("steps expiries from the one the chain shows, while a new version's summary loads", async () => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  Object.defineProperty(HTMLDialogElement.prototype, "showModal", { configurable: true, value() { this.open = true } })
  Object.defineProperty(HTMLDialogElement.prototype, "close", { configurable: true, value() { this.open = false } })
  vi.mocked(useLive).mockReturnValue(liveState({ ...status, underlyings: [{ ...status.underlyings[0]!, version: 2 }] }, null, "open"))
  const client = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  // The chain shows the first expiry still open: the 0-day one has passed.
  const expiries = [{ ...expiry, id: "2026-10-09PM", days: 0 }, { ...expiry, id: "2026-10-16PM" }, { ...expiry, id: "2026-10-23PM" }]
  client.setQueryData(["summary", "SPX", 1], { symbol: "SPX", as_of: status.underlyings[0]!.as_of, expiries } as unknown as Summary)
  const host = document.createElement("div"), root = createRoot(host)
  try {
    await act(async () => root.render(<QueryClientProvider client={client}><App /></QueryClientProvider>))
    const key = (name: string) => act(async () => { window.dispatchEvent(new KeyboardEvent("keydown", { key: name, cancelable: true })) })
    await key("ArrowRight")
    await key("ArrowLeft")
    expect(navigate.mock.calls).toEqual([[{ expiry: "2026-10-23PM" }], [{ expiry: "2026-10-09PM" }]])
  } finally { await act(async () => root.unmount()); client.clear() }
})
