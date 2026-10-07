import { useQueryClient } from "@tanstack/react-query"
import { useCallback, useEffect, useState } from "react"
import { marketTime, useLive } from "./api/live"
import { useAccount } from "./api/trading"
import type { Summary } from "./api/types"
import { AlertWatcher, Toasts } from "./components/Alerts"
import { WatchOnlyNotice, WriteAccess } from "./components/TradingControls"
import { AccountStorageBanner } from "./components/AccountStorageBanner"
import { Header } from "./components/Header"
import { DemoPrompt } from "./components/DemoPrompt"
import { CircuitBreakerBanner } from "./components/CircuitBreakerBanner"
import { UpdateNotice } from "./components/UpdateNotice"
import { ReplayBanner } from "./components/ReplayBanner"
import { Welcome, welcome } from "./components/Welcome"
import { Sidebar, viewLabels } from "./components/Sidebar"
import { Empty } from "./components/ui"
import { payoutsVisible } from "./lib/payouts"
import { accountViews, navigableViews, useRoute } from "./lib/route"
import { matchingPayload } from "./lib/payload"
import { defaultExpiry } from "./lib/expiry"
import { BriefView } from "./views/BriefView"
import { DashboardView } from "./views/DashboardView"
import { EngineView } from "./views/EngineView"
import { BacktestView } from "./views/BacktestView"
import { PlaybooksView } from "./views/PlaybooksView"
import { JournalView } from "./views/JournalView"
import { OrdersView } from "./views/OrdersView"
import { PayoutsView } from "./views/PayoutsView"
import { PositionsView } from "./views/PositionsView"
import { ReplayView } from "./views/ReplayView"
import { RulesView } from "./views/RulesView"
import { TradeView } from "./views/TradeView"

export function App() {
  const [route, navigate] = useRoute()
  const live = useLive()
  const queryClient = useQueryClient()
  const [menu, setMenu] = useState(false)
  const closeMenu = useCallback(() => setMenu(false), [])
  const storage = live.accounts.find((account) => account.id === live.account)
  const symbols = (live.tick?.underlyings ?? live.status?.underlyings ?? []).map((u) => u.symbol)
  const symbol = route.symbol && symbols.includes(route.symbol) ? route.symbol : (symbols[0] ?? null)
  const version = symbol ? live.version(symbol) : 0
  const payouts = payoutsVisible(useAccount().data)
  // Account pages need a paper-trading server; analytics-only servers land on Trade.
  const view = accountViews.includes(route.view) && !live.trading ? "chain"
    : route.view === "payouts" && !payouts ? "dashboard" : route.view
  const pages = navigableViews(live.trading != null, payouts)

  useEffect(() => { document.title = `${viewLabels[view]} · OpenPort` }, [view])

  // Number keys switch between the sidebar's pages; left/right step through expiries on the chain.
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      const target = event.target instanceof Element ? event.target : null
      if (event.defaultPrevented || event.metaKey || event.ctrlKey || event.altKey || target?.closest(
        'input, textarea, select, button, [contenteditable]:not([contenteditable="false"]), [role="radiogroup"], [role="tablist"], [role="textbox"], [role="combobox"], [role="slider"], [role="spinbutton"]',
      )) return
      const next = pages[Number(event.key) - 1]
      if (next) {
        event.preventDefault()
        navigate({ view: next })
        return
      }
      if ((event.key === "ArrowLeft" || event.key === "ArrowRight") && symbol && view === "chain") {
        const summary = matchingPayload(queryClient.getQueryData<Summary>(["summary", symbol, version]), symbol)
        const ids = summary?.expiries.map((e) => e.id) ?? []
        if (ids.length === 0) return
        const current = route.expiry ?? defaultExpiry(summary?.expiries ?? [], marketTime(live, symbol, summary?.as_of))
        const at = Math.max(0, ids.indexOf(current ?? ""))
        const step = ids[Math.min(ids.length - 1, Math.max(0, at + (event.key === "ArrowRight" ? 1 : -1)))]
        if (step) navigate({ expiry: step })
        event.preventDefault()
      }
    }
    window.addEventListener("keydown", onKey)
    return () => window.removeEventListener("keydown", onKey)
  }, [navigate, queryClient, route.expiry, view, symbol, version, pages, live])

  const waiting = live.status?.feed.message || "Connecting to openportd…"
  const content = view === "dashboard" ? <DashboardView />
    : view === "brief" ? <BriefView symbol={symbol} />
    : view === "positions" ? <PositionsView />
    : view === "orders" ? <OrdersView />
    : view === "backtest" ? <BacktestView />
    : view === "playbooks" ? <PlaybooksView />
    : view === "journal" ? <JournalView />
    : view === "rules" ? <RulesView />
    : view === "payouts" ? <PayoutsView />
    : view === "engine" ? <EngineView />
    : view === "replay" ? <ReplayView onNavigate={(next) => navigate({ view: next })} />
    : !symbol ? (
      <Empty>
        <div className="text-center">
          <div>Waiting for the first snapshot…</div>
          {!live.status && <div className="mt-3"><WriteAccess /></div>}
          <div className="mt-1 text-xs text-faint">{waiting}</div>
        </div>
      </Empty>
    ) : <TradeView symbol={symbol} view={view} expiry={route.expiry} onNavigate={navigate} />

  return (
    <div className="flex min-h-full">
      <Sidebar view={view} onView={(next) => navigate({ view: next })} open={menu} onClose={closeMenu} />
      <div className="flex min-w-0 flex-1 flex-col">
        <ReplayBanner />
        <DemoPrompt onNavigate={(next) => navigate({ view: next })} />
        <Header symbol={symbol} onSymbol={(s) => navigate({ symbol: s, expiry: null })} onMenu={() => setMenu(true)} />
        <CircuitBreakerBanner />
        <AccountStorageBanner damaged={storage?.damaged} journal={storage?.journal_size} reason={live.trading?.reason} />
        <UpdateNotice />
        <WatchOnlyNotice trading={live.trading} sandboxes={live.status?.sandboxes} onCreated={(id) => {
          live.switchSource("live", "user")
          live.switchAccount(id, "user")
          void queryClient.invalidateQueries()
        }} />
        <main className="min-w-0 flex-1 p-3 lg:p-5">{content}</main>
        <AlertWatcher />
        <Toasts />
        <Welcome onNavigate={(next) => navigate({ view: next })} />
        <footer className="border-t border-border px-4 py-2 text-[11px] text-faint">
          OpenPort · open-source options analytics and a paper-trading simulator on your own market data · simulated fills, no order routing · not investment advice ·{" "}
          <a className="underline decoration-dotted hover:text-muted" href="https://github.com/38st/openport">
            source
          </a>{" "}·{" "}
          <button type="button" className="underline decoration-dotted hover:text-muted" onClick={() => welcome.show()}>
            welcome
          </button>
        </footer>
      </div>
    </div>
  )
}
