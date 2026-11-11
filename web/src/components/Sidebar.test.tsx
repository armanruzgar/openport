// @vitest-environment jsdom
import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { act } from "react"
import { createRoot, type Root } from "react-dom/client"
import { afterEach, beforeEach, expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { tradingQueries } from "../api/trading"
import { account, portfolio, status } from "../test/trading-fixtures"
import { navigationId, Sidebar } from "./Sidebar"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
let host: HTMLDivElement, root: Root, client: QueryClient
const wide = (matches: boolean) => vi.stubGlobal("matchMedia", vi.fn(() => ({ matches, addEventListener: vi.fn(), removeEventListener: vi.fn() })))
beforeEach(() => {
  vi.stubGlobal("IS_REACT_ACT_ENVIRONMENT", true)
  vi.stubGlobal("fetch", vi.fn(() => new Promise(() => {})))
  vi.mocked(useLive).mockReturnValue(liveState(status, null, "open"))
  host = document.createElement("div"); document.body.append(host); root = createRoot(host)
  client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  client.setQueryData(tradingQueries(0, "17", true).account.queryKey, account)
  client.setQueryData(tradingQueries(0, "17", true).portfolio.queryKey, portfolio)
})
afterEach(async () => {
  await act(async () => root.unmount()); host.remove(); client.clear()
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})
const draw = (open: boolean) => act(async () => root.render(<QueryClientProvider client={client}>
  <Sidebar view="positions" onView={() => {}} open={open} onClose={() => {}} />
</QueryClientProvider>))

it("takes a closed drawer out of the tab order, moves focus into it when opened and back out when closed", async () => {
  wide(false)
  const menu = document.createElement("button")
  menu.textContent = "Open navigation"
  document.body.append(menu)
  try {
    await draw(false)
    const aside = document.getElementById(navigationId)!
    expect(aside.hasAttribute("inert")).toBe(true)
    expect(aside.getAttribute("aria-hidden")).toBe("true")
    menu.focus()
    await draw(true)
    expect(aside.hasAttribute("inert")).toBe(false)
    expect(aside.hasAttribute("aria-hidden")).toBe(false)
    expect(aside.contains(document.activeElement)).toBe(true)
    expect(document.activeElement?.getAttribute("aria-current")).toBe("page")
    await draw(false)
    expect(document.activeElement).toBe(menu)
    expect(aside.hasAttribute("inert")).toBe(true)
  } finally { menu.remove() }
})

it("leaves the docked sidebar reachable on wide screens and scrolls it as a whole", async () => {
  wide(true)
  await draw(false)
  const aside = document.getElementById(navigationId)!
  expect(aside.hasAttribute("inert")).toBe(false)
  expect(aside.hasAttribute("aria-hidden")).toBe(false)
  // A tall account block scrolls the whole sidebar instead of squeezing the page list.
  expect(aside.className).toContain("overflow-y-auto")
  expect(aside.querySelector('[aria-label="Pages"]')!.className).not.toContain("overflow-y-auto")
})
