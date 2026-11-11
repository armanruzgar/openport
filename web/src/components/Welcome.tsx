import { useSyncExternalStore } from "react"
import { useLive } from "../api/live"
import { joinList } from "../lib/format"
import { providerLabel } from "../lib/provider"
import type { View } from "../lib/route"
import { formatMoney } from "../lib/trading"
import { useDemoOffer } from "./DemoPrompt"
import { Dialog } from "./Dialog"
import { TradingError } from "./TradingControls"

const key = "openport.welcome"
/** Whether the welcome is showing: once per browser, and again from the footer. */
export function createWelcomeStore(storage: () => Pick<Storage, "getItem" | "setItem">) {
  let open: boolean | undefined
  const listeners = new Set<() => void>()
  const emit = () => listeners.forEach((listener) => listener())
  return {
    get() {
      // Without storage it would return on every visit, so it waits to be asked for.
      if (open === undefined) {
        try { open = storage().getItem(key) !== "seen" } catch { open = false }
      }
      return open
    },
    close() {
      open = false
      try { storage().setItem(key, "seen") } catch { /* Closed for this page. */ }
      emit()
    },
    show() {
      open = true
      emit()
    },
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const welcome = createWelcomeStore(() => window.localStorage)
export function useWelcome() {
  return useSyncExternalStore(welcome.subscribe, welcome.get, () => false)
}

/** What OpenPort is, where its data comes from, and where to start. */
export function Welcome({ onNavigate }: { onNavigate: (view: View) => void }) {
  const open = useWelcome()
  const { status, trading } = useLive()
  const offer = useDemoOffer()
  if (!open || !status) return null
  const provider = status.provider
  const symbols = status.underlyings.map((u) => u.symbol)
  const go = (view: View) => {
    welcome.close()
    onNavigate(view)
  }
  return <Dialog title="Welcome to OpenPort" onClose={() => welcome.close()}>
    <p className="text-sm">An options analytics terminal and a paper-trading simulator. Orders fill against the
      quotes your market data displays, up to their size, and nothing reaches an exchange.</p>
    <dl className="grid grid-cols-[7rem_1fr] gap-x-3 gap-y-1.5 text-sm">
      <dt className="text-muted">Market data</dt>
      <dd>{provider.name}, {providerLabel(provider, status.feed.state)}{symbols.length ? ` · ${symbols.join(", ")}` : ""}</dd>
      <dt className="text-muted">Account</dt>
      <dd>{trading?.enabled
        ? `${trading.plan ?? "Practice"}, starting with ${formatMoney(trading.initial_cash)}`
        : trading?.reason ?? "Paper trading is off on this server"}</dd>
    </dl>
    <ul className="list-disc space-y-1 pl-5 text-sm">
      <li><span className="font-medium">Trade</span>: pick an expiry on the chain and click a bid or an ask to open the
        ticket. Strategy mode builds spreads of up to four legs.</li>
      {trading?.enabled && <>
        <li><span className="font-medium">Positions and Orders</span>: close, roll, change or cancel, with risk limits
          and a kill switch.</li>
        <li><span className="font-medium">Dashboard and Rules</span>: start an evaluation against a profit target and a
          trailing drawdown, and see how it is judged.</li>
        <li><span className="font-medium">Journal</span>: the P&L calendar and reports, with a note and tags on each trade.</li>
      </>}
      <li>The bell sets price and fill alerts. Number keys switch pages; the arrow keys step through expiries.</li>
    </ul>
    {offer.offered && <p className="text-sm">{offer.reason}. In the meantime the demo market plays a simulated day
      in {joinList(offer.symbols)} options, with generated prices and its own paper account. The Replay page has scenario goals, repeatable seeds, start times and saved runs.</p>}
    <p className="text-xs text-muted">Simulated fills, no order routing, not investment advice.</p>
    <TradingError error={offer.controls.error} />
    <div className="flex flex-wrap gap-2">
      {offer.offered && <button type="button" className="trade-button border-accent text-foreground" disabled={offer.controls.pending}
        onClick={() => offer.start(() => go("replay"), undefined, () => welcome.close())}>
        {offer.controls.pending ? "Starting…" : "Try the demo"}</button>}
      <button type="button" className="trade-button" onClick={() => go("replay")}>Practice library</button>
      <button type="button" className={`trade-button ${offer.offered ? "" : "border-accent text-foreground"}`} onClick={() => go("chain")}>Start on the chain</button>
      {trading?.enabled && <button type="button" className="trade-button" onClick={() => go("dashboard")}>See the account</button>}
    </div>
  </Dialog>
}
