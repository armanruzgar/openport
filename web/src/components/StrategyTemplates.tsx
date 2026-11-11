import { useQuery } from "@tanstack/react-query"
import { useState } from "react"
import { api } from "../api/client"
import { useLive } from "../api/live"
import type { Chain, Expiry } from "../api/types"
import { matchingPayload } from "../lib/payload"
import { netQuote, templateTag, type StrategyTemplate, type TemplateSetup, type TemplateTarget } from "../lib/strategy"
import { loadStrategyPresets, saveStrategyPresets } from "../lib/strategy-presets"
import { Dialog } from "./Dialog"
import { deliversShares } from "../lib/trading"
import { netText } from "./StrategyTicket"

const kinds: { value: StrategyTemplate["kind"]; label: string }[] = [
  { value: "vertical", label: "Vertical spread" }, { value: "condor", label: "Iron condor" },
  { value: "iron-butterfly", label: "Iron butterfly" }, { value: "strangle", label: "Strangle" },
  { value: "straddle", label: "Straddle" }, { value: "butterfly", label: "Long butterfly" },
  { value: "calendar", label: "Calendar" }, { value: "diagonal", label: "Diagonal" },
]
const targetLabels: Record<TemplateTarget["mode"], string> = {
  atm: "At the money", delta: "Delta (absolute, 0–100)", points: "Offset from forward (points)",
  moves: "Expected moves from forward", strike: "Centre strike",
}
function initial(kind: StrategyTemplate["kind"], farExpiry: string): StrategyTemplate {
  switch (kind) {
    case "vertical": return { kind, type: "put", direction: "credit", target: { mode: "delta", value: 15 }, width: 5 }
    case "condor": return { kind, target: { mode: "delta", value: 15 }, width: 5 }
    case "iron-butterfly": return { kind, width: 5 }
    case "strangle": return { kind, side: "sell", delta: 15 }
    case "straddle": return { kind, side: "sell" }
    case "butterfly": return { kind, type: "call", target: { mode: "atm" }, width: 5 }
    case "calendar": case "diagonal": return { kind, type: "call", target: { mode: "atm" }, farExpiry, offset: kind === "diagonal" ? 5 : 0 }
  }
}

/** Parameters select listed contracts; the existing ticket remains the order review and submission step. */
export function StrategyTemplates({ near, expiries, onApply, onClose, onStockStrategy }: {
  near: Chain; expiries: readonly Expiry[]; onApply: (setup: TemplateSetup) => void; onClose: () => void
  onStockStrategy?: (kind: "covered-call" | "collar") => void
}) {
  const live = useLive()
  const version = live.version(near.symbol)
  const later = expiries.filter((e) => Date.parse(e.expiry_time) > Date.parse(near.expiry.expiry_time))
  const [template, setTemplate] = useState<StrategyTemplate>(() => initial("vertical", later[0]?.id ?? ""))
  const [presets, setPresets] = useState(() => loadStrategyPresets(near.symbol))
  const [presetName, setPresetName] = useState("")
  const [storageMessage, setStorageMessage] = useState("")
  const farId = "farExpiry" in template ? template.farExpiry : null
  const farQuery = useQuery({
    queryKey: ["chain", near.symbol, farId, 0, version],
    queryFn: ({ signal }) => api.chain(near.symbol, farId!, 0, signal),
    enabled: farId != null && later.some((e) => e.id === farId),
    placeholderData: (previous) => matchingPayload(previous, near.symbol, farId),
  })
  const far = matchingPayload(farQuery.data, near.symbol, farId)
  const selection = useQuery({
    queryKey: ["template", live.accountScope, near.symbol, near.expiry.id, near.version, near.strikes[0]?.strike, near.strikes.at(-1)?.strike, template, far?.version],
    queryFn: ({ signal }) => api.buildTemplate(template, near, signal),
    enabled: farId == null || far != null,
    retry: false,
  })
  // A calendar or diagonal needs a listed later expiry: none chosen, or one no longer
  // listed (an old preset's), loads nothing, so say what is missing rather than wait.
  const farListed = farId != null && later.some((e) => e.id === farId)
  const setup = selection.data ?? { reason: selection.error ? selection.error.message
    : farId != null && !farListed ? (later.length ? "Choose a later expiry for the far leg."
      : `No expiry after ${near.expiry.expiry} is listed, so this template has no far leg.`)
    : farId && !far ? `Loading the far chain (${farId})…` : "Selecting listed contracts…" }
  const modes: TemplateTarget["mode"][] = template.kind === "vertical" ? ["delta", "points", "moves"]
    : template.kind === "condor" ? ["delta", "moves"] : template.kind === "butterfly" ? ["atm", "strike"] : ["atm", "delta"]
  const save = () => {
    const name = presetName.trim()
    if (!name) return
    const next = [...presets.filter((p) => p.name !== name), { name, template }]
    setPresets(next)
    setStorageMessage(saveStrategyPresets(near.symbol, next) ? "Preset saved in this browser." : "Storage unavailable; preset remains available while this menu is open.")
  }
  return <Dialog title="Strategy templates" onClose={onClose}>
    <div className="space-y-4">
      {onStockStrategy && deliversShares(near.symbol) && <section aria-label="Strategies with shares" className="space-y-2">
        <p className="text-xs text-muted">Buy 100 shares per contract, then send the options as a separate order. These two steps are not atomic.</p>
        <div className="flex gap-2">
          <button type="button" className="trade-button" onClick={() => onStockStrategy("covered-call")}>Covered call</button>
          <button type="button" className="trade-button" onClick={() => onStockStrategy("collar")}>Collar</button>
        </div>
      </section>}
      {live.source === "replay" && live.replay?.demo && <p className="text-xs text-warn">Demo market · simulated prices</p>}
      <p className="text-xs text-muted">{near.symbol} · Near expiry {near.expiry.expiry} {near.expiry.settlement}. Strikes snap to the loaded chain. Review the resulting widths and prices before sending.</p>
      {presets.length > 0 && <label className="trade-label">Saved preset
        <select className="trade-input" value="" onChange={(e) => {
          const saved = presets.find((p) => p.name === e.target.value)
          if (!saved) return
          // A preset's far expiry may have expired since: start from the first later one instead.
          const recalled = saved.template
          const farExpiry = "farExpiry" in recalled ? recalled.farExpiry : null
          setTemplate("farExpiry" in recalled && !later.some((option) => option.id === farExpiry) ? { ...recalled, farExpiry: later[0]?.id ?? "" } : recalled)
          setPresetName(saved.name); setStorageMessage("")
        }}><option value="">Choose a preset for {near.symbol}</option>{presets.map((p) => <option key={p.name} value={p.name}>{p.name}</option>)}</select>
      </label>}
      <div className="grid grid-cols-2 gap-3">
        <label className="trade-label col-span-2">Template
          <select className="trade-input" value={template.kind} onChange={(e) => setTemplate(initial(e.target.value as StrategyTemplate["kind"], later[0]?.id ?? ""))}>
            {kinds.map((kind) => <option key={kind.value} value={kind.value}>{kind.label}</option>)}
          </select>
        </label>
        {"type" in template && <label className="trade-label">Option type
          <select className="trade-input" value={template.type} onChange={(e) => setTemplate({ ...template, type: e.target.value as "call" | "put" })}>
            <option value="call">Call</option><option value="put">Put</option>
          </select>
        </label>}
        {template.kind === "vertical" && <label className="trade-label">Premium
          <select className="trade-input" value={template.direction} onChange={(e) => setTemplate({ ...template, direction: e.target.value as "credit" | "debit" })}>
            <option value="credit">Credit</option><option value="debit">Debit</option>
          </select>
        </label>}
        {"side" in template && <label className="trade-label">Position
          <select className="trade-input" value={template.side} onChange={(e) => setTemplate({ ...template, side: e.target.value as "buy" | "sell" })}>
            <option value="sell">Short</option><option value="buy">Long</option>
          </select>
        </label>}
        {"target" in template && <>
          <label className="trade-label col-span-2">{template.kind === "vertical" ? "Short-leg target" : "Strike selection"}
            <select className="trade-input" value={template.target.mode} onChange={(e) => {
              const mode = e.target.value as TemplateTarget["mode"]
              setTemplate({ ...template, target: mode === "atm" ? { mode } : { mode, value: mode === "delta" ? 15 : mode === "moves" ? 1 : mode === "strike" ? near.expiry.forward ?? 0 : 0 } })
            }}>{modes.map((mode) => <option key={mode} value={mode}>{targetLabels[mode]}</option>)}</select>
          </label>
          {template.target.mode !== "atm" && <label className="trade-label col-span-2">{targetLabels[template.target.mode]}
            <input className="trade-input" type="number" step="any" value={Number.isFinite(template.target.value) ? template.target.value : ""}
              onChange={(e) => { if (template.target.mode !== "atm") setTemplate({ ...template, target: { mode: template.target.mode, value: e.target.valueAsNumber } }) }} />
          </label>}
        </>}
        {template.kind === "strangle" && <label className="trade-label">Target delta
          <input className="trade-input" type="number" min="1" max="49" step="any" value={Number.isFinite(template.delta) ? template.delta : ""} onChange={(e) => setTemplate({ ...template, delta: e.target.valueAsNumber })} />
        </label>}
        {"width" in template && <label className="trade-label">Width (points)
          <input className="trade-input" type="number" min="0" step="any" value={Number.isFinite(template.width) ? template.width : ""} onChange={(e) => setTemplate({ ...template, width: e.target.valueAsNumber })} />
        </label>}
        {"farExpiry" in template && <label className="trade-label col-span-2">Far expiry
          <select className="trade-input" value={template.farExpiry} onChange={(e) => setTemplate({ ...template, farExpiry: e.target.value })}>
            <option value="">Choose a later expiry</option>{later.map((e) => <option key={e.id} value={e.id}>{e.expiry} {e.settlement}</option>)}
          </select>
        </label>}
        {template.kind === "diagonal" && <label className="trade-label col-span-2">Far strike offset from near (points, signed)
          <input className="trade-input" type="number" step="any" value={Number.isFinite(template.offset) ? template.offset : ""} onChange={(e) => setTemplate({ ...template, offset: e.target.valueAsNumber })} />
        </label>}
      </div>
      <p className="text-[11px] text-muted">Delta uses the chain’s own Greeks. Positive forward offsets go out of the money; condors use both sides. Calendars and diagonals sell near and buy far.</p>
      {"reason" in setup ? <p role="status" className="text-sm text-warn">{farId && farQuery.isError ? `Far chain unavailable: ${String(farQuery.error)}` : setup.reason}</p>
        : <div aria-label="Template preview" className="space-y-1 rounded-md border border-border p-3 text-xs tabular">
          {setup.legs.map((leg) => <div key={leg.symbol}>{leg.side === "buy" ? "Buy" : "Sell"} {leg.ratio} × {leg.strike} {leg.type} · {leg.expiry}</div>)}
          <div>Net mid: {netText(netQuote(setup.legs).mid)} · Width: {setup.widths.join(" / ")} points</div>
        </div>}
      <p className="break-words text-[11px] text-muted">Template tag: {templateTag(template)}</p>
      <div className="flex items-end gap-2">
        <label className="trade-label min-w-0 flex-1">Preset name<input className="trade-input" value={presetName} onChange={(e) => setPresetName(e.target.value)} placeholder="0DTE put spread 10Δ 5 wide" /></label>
        <button type="button" className="trade-button" disabled={!presetName.trim() || "reason" in setup} onClick={save}>Save preset</button>
      </div>
      {storageMessage && <p role="status" className="text-xs text-muted">{storageMessage}</p>}
      <button type="button" className="trade-button w-full border-accent" disabled={"reason" in setup} onClick={() => { if (!("reason" in setup)) onApply(setup) }}>Review in ticket</button>
    </div>
  </Dialog>
}
