import { QueryClient, QueryClientProvider } from "@tanstack/react-query"
import type { ReactNode } from "react"
import { renderToStaticMarkup } from "react-dom/server"
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest"
import { dataSource } from "../lib/data-source"
import { api } from "../test/action-api"
import { liveState, useLive } from "../api/live"
import type { AccountBrief, Status } from "../api/types"
import { AccountSwitcher, NewAccountDialog } from "../components/AccountSwitcher"
import { activeAccount, createAccountStore, MAIN_ACCOUNT } from "../lib/active-account"
import { plans, portfolio, status, trading } from "../test/trading-fixtures"

vi.mock("../api/live", async (original) => ({ ...await original<typeof import("../api/live")>(), useLive: vi.fn() }))
const clients: QueryClient[] = []
function render(node: ReactNode) {
  const client = new QueryClient({ defaultOptions: { queries: { retry: false, staleTime: Infinity, gcTime: Infinity } } })
  clients.push(client)
  client.setQueryData(["plans"], { plans })
  return renderToStaticMarkup(<QueryClientProvider client={client}>{node}</QueryClientProvider>)
}
const swing: AccountBrief = { id: "swing-50k", name: "Swing 50k", trading: { ...trading, account_version: "4", plan: "End-of-day 50K", plan_id: "eod-50k", evaluation: "active" } }
const withAccounts: Status = { ...status, accounts: [{ id: "main", name: "Main", trading }, swing] }
beforeEach(() => vi.mocked(useLive).mockReturnValue(liveState(withAccounts, null, "open")))
afterEach(() => { clients.splice(0).forEach((client) => client.clear()); vi.clearAllMocks(); vi.unstubAllGlobals(); activeAccount.set(MAIN_ACCOUNT) })

describe("the active account", () => {
  it("is remembered, falling back to the main account without storage", () => {
    const saved = new Map<string, string>()
    const store = createAccountStore(() => ({ getItem: (k) => saved.get(k) ?? null, setItem: (k, v) => void saved.set(k, v) }))
    expect(store.get()).toBe("main")
    store.set("swing-50k")
    expect(saved.get("openport.account")).toBe("swing-50k")
    const blocked = createAccountStore(() => { throw new Error("denied") })
    expect(blocked.get()).toBe("main")
    blocked.set("swing-50k")
    expect(blocked.get()).toBe("swing-50k")
  })

  it("scopes every trading request, and nothing else", async () => {
    const fetcher = vi.fn(async () => new Response(JSON.stringify(portfolio), { status: 200 }))
    vi.stubGlobal("fetch", fetcher)
    await api.portfolio()
    expect(fetcher).toHaveBeenLastCalledWith("/api/portfolio", expect.anything())
    activeAccount.set("swing-50k")
    await api.portfolio()
    expect(fetcher).toHaveBeenLastCalledWith("/api/portfolio?account=swing-50k", expect.anything())
    await api.orders("open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders?status=open&account=swing-50k", expect.anything())
    await api.cancelOrder("7", "open")
    expect(fetcher).toHaveBeenLastCalledWith("/api/orders/7?account=swing-50k", expect.objectContaining({ method: "DELETE" }))
    await api.plans()
    expect(fetcher).toHaveBeenLastCalledWith("/api/plans", expect.anything())
    await api.status()
    expect(fetcher).toHaveBeenLastCalledWith("/api/status", expect.anything())
  })

  it("decides the trading status the terminal shows", () => {
    expect(liveState(withAccounts, null, "open").trading).toBe(trading)
    expect(liveState(withAccounts, null, "open", 0, "swing-50k").trading).toBe(swing.trading)
    expect(liveState(withAccounts, null, "open", 0, "gone").trading).toBeUndefined()
    // Older servers list no accounts: only the main one exists.
    expect(liveState(status, null, "open").accounts).toEqual([])
  })
})

describe("switching accounts", () => {
  it("lists every account and offers a new one", () => {
    const html = render(<AccountSwitcher />)
    expect(html).toContain('<option value="main" selected="">Main</option>')
    expect(html).toContain('<option value="swing-50k">Swing 50k · End-of-day 50K · eod-50k</option>')
    expect(html).toContain("New account…")
  })

  it("creates an account on an evaluation or practice plan, never a funded one", () => {
    const html = render(<NewAccountDialog trading={trading} onClose={() => {}} onCreated={() => {}} />)
    expect(html).toContain('value="Account 3"')
    expect(html).toContain("Practice")
    expect(html).toContain("Intraday 100K")
    expect(html).not.toContain("funded-intraday-100k")
    expect(html).toContain("Choose a plan</button>")
  })
})


it("uses live account sources while viewing a replay and sends lifecycle methods", async () => {
  const fetcher = vi.fn(async () => new Response("{}", { status: 200 }))
  vi.stubGlobal("fetch", fetcher)
  dataSource.set("replay")
  await api.liveAccounts()
  expect(fetcher).toHaveBeenLastCalledWith("/api/accounts?archived=true", expect.anything())
  dataSource.set("live")
  activeAccount.set("unrelated")
  await api.updateAccount("swing-50k", { name: "Renamed", archived: true }, "open")
  expect(fetcher).toHaveBeenLastCalledWith("/api/accounts/swing-50k", expect.objectContaining({ method: "PATCH", body: JSON.stringify({ name: "Renamed", archived: true }) }))
  await api.deleteAccount("swing-50k", "open")
  expect(fetcher).toHaveBeenLastCalledWith("/api/accounts/swing-50k", expect.objectContaining({ method: "DELETE" }))
})
