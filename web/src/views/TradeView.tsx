import { StagedOrders } from "../components/Playbooks"
import type { View } from "../lib/route"
import { ChainView } from "./ChainView"
import { ExposureView } from "./ExposureView"
import { SmileView } from "./SmileView"
import { VolatilityScannerView } from "./VolatilityScannerView"

const tabs: { view: View; label: string; hint: string }[] = [
  { view: "chain", label: "Chain", hint: "Quotes, Greeks and the order ticket" },
  { view: "smile", label: "Volatility", hint: "Smile, SVI/SSVI fits and term structure" },
  { view: "exposure", label: "Exposure", hint: "Dealer gamma and vanna exposure" },
  { view: "vol-scanner", label: "Vol scanner", hint: "Compare all subscribed underlyings" },
]

/** The trading terminal: the chain with its ticket, plus market analytics beside it. */
export function TradeView({ symbol, view, expiry, onNavigate }: {
  symbol: string; view: View; expiry: string | null; onNavigate: (patch: { view?: View; expiry?: string | null; symbol?: string }) => void
}) {
  return (
    <div className="min-w-0 space-y-3">
      <StagedOrders />
      <div role="tablist" aria-label="Trade views" className="flex flex-wrap gap-1 border-b border-border">
        {tabs.map((tab) => (
          <button key={tab.view} type="button" role="tab" aria-selected={view === tab.view} title={tab.hint}
            onClick={() => onNavigate({ view: tab.view })}
            className={`-mb-px border-b-2 px-3 py-1.5 text-sm transition-colors ${view === tab.view ? "border-accent text-foreground" : "border-transparent text-muted hover:text-foreground"}`}>
            {tab.label}
          </button>
        ))}
      </div>
      {view === "smile" ? <SmileView symbol={symbol} />
        : view === "vol-scanner" ? <VolatilityScannerView onSymbol={next => onNavigate({ symbol: next, view: "smile", expiry: null })} />
        : view === "exposure" ? <ExposureView symbol={symbol} />
        : <ChainView symbol={symbol} expiry={expiry} onExpiry={(id) => onNavigate({ expiry: id })} />}
    </div>
  )
}
