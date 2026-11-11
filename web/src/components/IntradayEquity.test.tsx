import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import { renderToStaticMarkup } from "react-dom/server"
import { expect, it, vi } from "vitest"
import { account, trading } from "../test/trading-fixtures"
import { IntradayEquity } from "./IntradayEquity"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: () => ({ accountScope: 1, trading }) }))
it.each([false, true])("shows the equity failure time and recovery state %s", (recovered) => {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity } } })
  client.setQueryData(["trading", 1, "equity", trading.account_version], {
    samples: [], error: "equity: cannot append", error_time: "2026-09-22T14:01:00Z",
    error_market_time: "2026-09-22T14:00:00Z", error_recovered: recovered,
  })
  const html = renderToStaticMarkup(<QueryClientProvider client={client}><IntradayEquity account={account} /></QueryClientProvider>)
  expect(html).toContain("cannot append")
  expect(html).toContain("2026-09-22T14:01:00Z")
  expect(html).toContain("2026-09-22T14:00:00Z")
  expect(html).toContain(recovered ? "storage recovered" : "storage not recovered")
  client.clear()
})
