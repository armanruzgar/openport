import { notificationHistory, useNotificationHistory } from "../lib/notification-history"
import { useOutsideClose } from "../lib/outside-close"
import { useEffect, useRef, useState } from "react"
import { useLive } from "../api/live"
import { useAccount, useRisk, useFills, useTrades } from "../api/trading"
import { alertStore, describeAlert, directionFor, dividendMessage, deliveryMessage, fillMessage, newestFill, newFills, priceAlertMessage, reached, unordered, useAlertSettings, type PriceAlert } from "../lib/alerts"
import { useActiveAccount } from "../lib/active-account"
import { ruleAlerts, ruleAlertStore } from "../lib/rule-alerts"
import { isNum } from "../lib/format"
import { notificationPermission, notify, requestNotifications, toasts, useToasts } from "../lib/notify"
import { Dialog } from "./Dialog"
import { NotificationSettings } from "./NotificationSettings"
import { ServerAlerts, ServerAlertWatcher } from "./ServerAlerts"

const setAt = new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", hour: "numeric", minute: "2-digit", timeZone: "America/New_York" })

/** Fires price and fill alerts from the live feed; mounted once for the whole terminal. */
export function AlertWatcher() {
  const settings = useAlertSettings()
  const { underlyings, source } = useLive()
  useEffect(() => {
    if (source !== "live" || !settings.prices.length) return
    const spot = (symbol: string) => underlyings.find((u) => u.symbol === symbol)?.spot
    const due = settings.prices.filter((alert) => reached(alert, spot(alert.symbol)))
    if (!due.length) return
    // Claim the due alerts in storage: one another tab already fired is gone from there.
    let claimed: PriceAlert[] = []
    alertStore.update((current) => {
      claimed = current.prices.filter((alert) => due.some((d) => d.id === alert.id))
      return { ...current, prices: current.prices.filter((alert) => !claimed.includes(alert)) }
    })
    for (const alert of claimed) {
      const { title, body } = priceAlertMessage(alert, spot(alert.symbol) as number)
      notify(title, body, settings.sound, { kind: "alert", scope: "browser", id: `price:${alert.id}` })
    }
  }, [settings, underlyings, source])
  // Account alerts fire in replays too; external forwarding follows the server opt-in.
  return <>
    <ServerAlertWatcher sound={settings.sound} />
    <FillWatcher sound={settings.sound} enabled={source === "live" && settings.fills} />
    <DeliveryWatcher sound={settings.sound} />
    <RuleWatcher sound={settings.sound} />
  </>
}

function RuleWatcher({ sound }: { sound: boolean }) {
  const account = useAccount().data
  const risk = useRisk().data
  const id = useActiveAccount()
  const { source } = useLive()
  useEffect(() => {
    if (!account || !risk || account.account_version !== risk.account_version) return
    const attempt = account.evaluation.attempt, day = account.evaluation.day
    const alerts = ruleAlertStore.fresh(source === "live" ? id : `${source}:${id}`, attempt, day, ruleAlerts(account, risk))
    for (const alert of alerts) notify(alert.title, alert.body, sound, { kind: "rule", scope: `${source}:${id}`, id: `rule:${attempt}:${day}:${alert.id}`, quiet: source !== "live" })
  }, [account, risk, id, sound, source])
  return null
}

/** Assignments, exercises at expiry and dividends arrive without an order, often
 * overnight: each new one is announced once, whatever the fill setting. */
function DeliveryWatcher({ sound }: { sound: boolean }) {
  const { accountScope, trading, source, account } = useLive()
  const data = useTrades("current").data
  const seen = useRef<{ scope: number; fill: number; dividends: number } | null>(null)
  useEffect(() => {
    if (!data || !trading?.enabled) return
    const fills = data.stock_fills ?? []
    const dividends = data.dividends ?? []
    const newest = fills.reduce((max, f) => Math.max(max, Number(f.id) || 0), 0)
    const last = seen.current
    seen.current = { scope: accountScope, fill: newest, dividends: dividends.length }
    // The first list for an account is history.
    if (last?.scope !== accountScope) return
    for (const fill of fills.filter((f) => Number(f.id) > last.fill && unordered(f))) {
      const { title, body } = deliveryMessage(fill)
      notify(title, body, sound, { kind: "assignment", scope: `${source}:${account}`, id: `delivery:${fill.id}:${fill.time}`, quiet: source !== "live" })
    }
    for (const dividend of dividends.slice(last.dividends)) {
      const { title, body } = dividendMessage(dividend)
      notify(title, body, sound, { kind: "dividend", scope: `${source}:${account}`, id: `dividend:${dividend.symbol}:${dividend.ex_date}:${dividend.time}`, quiet: source !== "live" })
    }
  }, [data, accountScope, trading?.enabled, sound, source, account])
  return null
}

/** Only fills after the first list it sees, so history and account switches stay quiet. */
function FillWatcher({ sound, enabled }: { sound: boolean; enabled: boolean }) {
  const { accountScope, source, account } = useLive()
  const fills = useFills().data?.fills
  const seen = useRef<{ scope: number; last: number } | null>(null)
  useEffect(() => {
    if (!fills) return
    if (seen.current?.scope !== accountScope) {
      seen.current = { scope: accountScope, last: newestFill(fills) }
      return
    }
    const fresh = newFills(fills, seen.current.last)
    if (!fresh.length) return
    seen.current.last = newestFill(fills)
    for (const fill of fresh) {
      const { title, body } = fillMessage(fill)
      notify(title, body, sound, { kind: "fill", scope: `${source}:${account}`, id: `fill:${fill.id}:${fill.time}`, quiet: !enabled || fresh.indexOf(fill) < fresh.length - 3 })
    }
    if (enabled && fresh.length > 3) toasts.push("More fills", `${fresh.length - 3} earlier fills are on the Orders page.`)
  }, [fills, accountScope, sound, source, account, enabled])
  return null
}

export function AlertsDialog({ initial, onClose }: { initial?: { symbol: string; level: number | null }; onClose: () => void }) {
  const settings = useAlertSettings()
  const { underlyings, status } = useLive()
  const symbols = underlyings.map((u) => u.symbol)
  const [symbol, setSymbol] = useState(initial?.symbol ?? symbols[0] ?? "")
  const [level, setLevel] = useState(initial?.level != null && Number.isFinite(initial.level) ? initial.level.toFixed(2) : "")
  const [permission, setPermission] = useState(notificationPermission)
  const spot = underlyings.find((u) => u.symbol === symbol)?.spot
  const value = Number(level)
  // The direction comes from the price now, so an alert waits for one.
  const valid = symbol !== "" && /^\d+(\.\d+)?$/.test(level.trim()) && value > 0 && isNum(spot)
  const direction = directionFor(value, isNum(spot) ? spot : null)
  const delay = status?.provider.delay_seconds
  function add() {
    if (!valid) return
    alertStore.update((current) => ({ ...current, prices: [...current.prices,
      { id: crypto.randomUUID(), symbol, direction, level: value, created: new Date().toISOString() }] }))
    setLevel("")
  }
  return <Dialog title="Alerts" onClose={onClose}>
    <section className="space-y-2">
      <h3 className="text-xs font-medium uppercase tracking-wide text-muted">Price alerts</h3>
      <form className="flex flex-wrap items-end gap-2" onSubmit={(event) => { event.preventDefault(); add() }}>
        <label className="trade-label">Underlying
          <select className="trade-input" value={symbol} onChange={(e) => setSymbol(e.target.value)}>
            {symbols.map((s) => <option key={s} value={s}>{s}</option>)}
          </select></label>
        <label className="trade-label">Level
          <input className="trade-input" inputMode="decimal" value={level} placeholder={isNum(spot) ? spot.toFixed(2) : undefined}
            onChange={(e) => setLevel(e.target.value)} /></label>
        <button type="submit" className="trade-button" disabled={!valid}>Add alert</button>
      </form>
      {symbol !== "" && !isNum(spot) && <p className="text-xs text-muted">Alerts need a price for {symbol} to tell rising from falling.</p>}
      {valid && <p className="text-xs text-muted">Alerts once when {describeAlert({ symbol, direction, level: value })}{isNum(spot) ? `, from ${spot.toFixed(2)} now` : ""}.</p>}
      {settings.prices.length ? <ul className="divide-y divide-border/60 text-sm">
        {settings.prices.map((alert) => <li key={alert.id} className="flex items-center justify-between gap-2 py-1.5">
          <span>{describeAlert(alert)} <span className="text-xs text-muted">· set {setAt.format(Date.parse(alert.created))} ET</span></span>
          <button type="button" className="trade-button" aria-label={`Remove alert: ${describeAlert(alert)}`}
            onClick={() => alertStore.update((current) => ({ ...current, prices: current.prices.filter((a) => a.id !== alert.id) }))}>Remove</button>
        </li>)}
      </ul> : <p className="text-sm text-muted">No price alerts.</p>}
    </section>
    <section className="space-y-2">
      <h3 className="text-xs font-medium uppercase tracking-wide text-muted">Browser notifications</h3>
      <label className="flex items-center justify-between gap-2 text-sm">Notify on each fill
        <input type="checkbox" role="switch" aria-label="Fill alerts" checked={settings.fills} className="h-4 w-4 accent-[var(--accent)]"
          onChange={(e) => alertStore.update((current) => ({ ...current, fills: e.target.checked }))} /></label>
      <label className="flex items-center justify-between gap-2 text-sm">Play a sound
        <input type="checkbox" role="switch" aria-label="Alert sound" checked={settings.sound} className="h-4 w-4 accent-[var(--accent)]"
          onChange={(e) => alertStore.update((current) => ({ ...current, sound: e.target.checked }))} /></label>
      <p className="text-xs text-muted">{permission === "granted" ? "Browser notifications are on, so alerts reach you in other tabs too."
        : permission === "denied" ? "This browser blocks notifications from openport, so alerts show on this page only. Allow them in the site settings to change that."
        : permission === "unsupported" ? "This browser has no notifications, so alerts show on this page only."
        : <>Alerts show on this page. <button type="button" className="text-accent hover:underline"
            onClick={() => void requestNotifications().then(setPermission)}>Allow browser notifications</button> to see them in other tabs too.</>}</p>
      <p className="text-[11px] text-faint">Browser price alerts are kept in this browser and run while openport is open, on the feed's prices{delay ? `, which arrive ${Math.round(delay / 60)} minutes late` : ""}. A price alert fires once, then clears.</p>
    </section>
    <ServerAlerts />
    <NotificationSettings />
  </Dialog>
}

export function AlertsButton() {
  const { source, account } = useLive()
  const scope = `${source}:${account}`
  const list = useNotificationHistory(scope)
  const [open, setOpen] = useState(false)
  const [settings, setSettings] = useState(false)
  const ref = useRef<HTMLDivElement>(null)
  useOutsideClose(ref, open, () => setOpen(false))
  useEffect(() => {
    const reload = (event: StorageEvent) => { if (event.key === "openport.notification-history") notificationHistory.reload() }
    window.addEventListener("storage", reload)
    return () => window.removeEventListener("storage", reload)
  }, [])
  const unread = list.filter((entry) => !entry.read).length
  return <div ref={ref} className="sm:relative">
    <button type="button" aria-label={`Notifications, ${unread} unread`} title="Notifications" aria-expanded={open} aria-controls="notification-center"
      onClick={() => setOpen(!open)} className="relative flex h-8 w-8 items-center justify-center rounded-md border border-border text-muted hover:bg-raised hover:text-foreground">
      <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <path d="M6 16v-5a6 6 0 0 1 12 0v5l1.5 2h-15L6 16Z" /><path d="M10 20.5a2 2 0 0 0 4 0" />
      </svg>
      {unread > 0 && <span className="absolute -right-1.5 -top-1.5 min-w-4 rounded-full bg-accent px-1 text-[9px] font-medium leading-4 text-background">{unread}</span>}
    </button>
    {open && <section id="notification-center" aria-label="Notification history" className="notification-drawer absolute inset-x-3 top-full z-30 mt-2 max-w-[calc(100vw-1.5rem)] sm:left-auto sm:right-0 sm:w-96 rounded-lg border border-border bg-panel p-3 shadow-chart"
      onKeyDown={(event) => { if (event.key === "Escape") { event.stopPropagation(); setOpen(false); ref.current?.querySelector("button")?.focus() } }}>
      <h2 className="text-sm font-medium text-foreground">Notifications · {unread} unread</h2>
      <p className="my-2 text-[11px] text-muted">Observed in this browser · {source} / {account}. Times below are receipt times. Recent 200 events across accounts.</p>
      <div className="mb-2 flex flex-wrap gap-2">
        <button className="trade-button" onClick={() => notificationHistory.markAllRead(scope)} disabled={!unread}>Mark all read</button>
        <button className="trade-button" onClick={() => notificationHistory.clear(scope)} disabled={!list.length}>Clear</button>
        <button className="trade-button" onClick={() => { setOpen(false); setSettings(true) }}>Alert settings</button>
      </div>
      <ol className="max-h-[55dvh] overflow-y-auto">{[...list].reverse().map((entry) => <li key={entry.id} className={`border-t border-border py-3 ${entry.read ? "text-muted" : "text-foreground"}`}>
        <div className="text-xs font-medium">{!entry.read && <span aria-label="Unread">● </span>}{entry.title}</div>
        <p className="mt-1 break-words text-xs">{entry.body}</p>
        <time className="mt-1 block text-[10px] text-faint" dateTime={entry.received}>Received {new Date(entry.received).toLocaleString()}</time>
      </li>)}</ol>
      {!list.length && <p className="py-3 text-xs text-muted">No notifications observed yet.</p>}
    </section>}
    {settings && <AlertsDialog onClose={() => setSettings(false)} />}
  </div>
}

/** Alerts inside the terminal, newest last, each for a few seconds. */
export function Toasts() {
  const list = useToasts()
  return <div className="pointer-events-none fixed bottom-4 right-4 z-50 flex w-80 max-w-[calc(100%-2rem)] flex-col gap-2" role="status" aria-live="polite">
    {list.map((toast) => <div key={toast.id} className="pointer-events-auto rounded-lg border border-border bg-panel p-3 text-sm shadow-chart">
      <div className="flex items-start justify-between gap-2">
        <span className="font-medium">{toast.title}</span>
        <button type="button" className="text-xs text-muted hover:text-foreground" aria-label={`Dismiss ${toast.title}`} onClick={() => toasts.dismiss(toast.id)}>×</button>
      </div>
      <p className="mt-0.5 text-xs text-muted">{toast.body}</p>
    </div>)}
  </div>
}
