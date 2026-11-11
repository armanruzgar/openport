import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { renderToStaticMarkup } from "react-dom/server"
import { expect, it, vi } from "vitest"
import { liveState, useLive } from "../api/live"
import { status } from "../test/trading-fixtures"
import { Header } from "./Header"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
vi.mock("../lib/theme", () => ({ useTheme: () => ({ theme: "dark", toggleTheme: vi.fn() }) }))

it("tells assistive technology which underlying the terminal shows", () => {
  const two = { ...status, underlyings: [status.underlyings[0]!, { ...status.underlyings[0]!, symbol: "SPY", spot: 700 }] }
  vi.mocked(useLive).mockReturnValue(liveState(two, null, "open"))
  const html = renderToStaticMarkup(<QueryClientProvider client={new QueryClient()}><Header symbol="SPY" onSymbol={() => {}} /></QueryClientProvider>)
  const nav = html.slice(html.indexOf('aria-label="Underlyings"'), html.indexOf("</nav>"))
  const buttons = nav.match(/<button[^>]*>[^<]*<\/button>/g) ?? []
  expect(buttons).toHaveLength(2)
  expect(buttons.find((b) => b.endsWith(">SPY</button>"))).toContain('aria-current="true"')
  expect(buttons.find((b) => b.endsWith(">SPX</button>"))).not.toContain("aria-current")
  expect(buttons.every((b) => b.includes('type="button"'))).toBe(true)
})
