import { useActionApi } from "../api/action-client"
import { useState } from "react"
import { useQueryClient } from "@tanstack/react-query"

import { useLive } from "../api/live"
import type { NotificationChannel, NotificationEvent } from "../api/types"
import type { WriteMode } from "../api/trading-types"
import { timestampET } from "../lib/freshness"

const events: { id: NotificationEvent; label: string }[] = [
  { id: "fill", label: "Fills" }, { id: "order_rejected", label: "Rejected orders" },
  { id: "floor", label: "Near the floor" }, { id: "rule_trip", label: "Rule trips" },
  { id: "assignment", label: "Assignments" }, { id: "exercise", label: "Exercises" },
  { id: "playbook_ready", label: "Playbooks ready" }, { id: "feed_stalled", label: "Stalled feed" },
  { id: "alert", label: "Account alerts" },
]

function Channel({ channel, mode }: { channel: NotificationChannel; mode: WriteMode }) {
  const api = useActionApi()
  const client = useQueryClient()
  const [enabled, setEnabled] = useState(channel.enabled)
  const [selected, setSelected] = useState(channel.events)
  const [distance, setDistance] = useState(channel.floor_distance)
  const [pending, setPending] = useState(false)
  const [message, setMessage] = useState("")
  const [error, setError] = useState(false)
  const valid = /^\d+(\.\d{1,6})?$/.test(distance) && Number.isFinite(Number(distance))
  const changed = enabled !== channel.enabled || distance !== channel.floor_distance ||
    [...selected].sort().join() !== [...channel.events].sort().join()
  async function action(test: boolean) {
    setPending(true); setMessage(""); setError(false)
    try {
      if (test) {
        await api.testNotification(channel.id, mode)
        setMessage("Test queued. Delivery status updates after the server sends it.")
      } else {
        await api.configureNotification(channel.id, { enabled, events: selected, floor_distance: distance }, mode)
        setMessage("Filters saved for this server session.")
      }
      void client.invalidateQueries({ queryKey: ["status", "live"] })
    } catch {
      setError(true)
      setMessage(test ? "Could not queue the test. Check your admin token and channel status." : "Could not save filters. Check your admin token and floor distance.")
    } finally { setPending(false) }
  }
  return <div className="space-y-2 rounded-md border border-border p-3">
    <div className="flex items-center justify-between gap-2 text-sm">
      <span className="font-medium">{channel.id} <span className="font-normal text-muted">· {channel.type}</span></span>
      <label className="flex items-center gap-2 text-xs">Enabled
        <input type="checkbox" role="switch" aria-label={`Enable ${channel.id}`} checked={enabled}
          disabled={pending || mode === "disabled"} className="h-4 w-4 accent-[var(--accent)]" onChange={(event) => setEnabled(event.target.checked)} />
      </label>
    </div>
    <fieldset disabled={pending || mode === "disabled"}>
      <legend className="sr-only">Events for {channel.id}</legend>
      <div className="grid grid-cols-2 gap-2 text-xs">
        {events.map((event) => <label key={event.id} className="flex items-center gap-2">
          <input type="checkbox" className="accent-[var(--accent)]" checked={selected.includes(event.id)}
            onChange={(change) => setSelected(change.target.checked ? [...selected, event.id] : selected.filter((value) => value !== event.id))} />
          {event.label}
        </label>)}
      </div>
      <label className="trade-label mt-2">Floor distance ($)
        <input className="trade-input" inputMode="decimal" value={distance} aria-invalid={!valid}
          onChange={(event) => setDistance(event.target.value)} />
      </label>
    </fieldset>
    <p className="text-xs text-muted">Last delivery: {channel.last_delivery ? timestampET(channel.last_delivery) : "none"}.
      {` ${channel.delivered} delivered · ${channel.failures} failed attempts · ${channel.dropped} dropped.`}</p>
    {channel.last_error && <p className="text-xs text-warn">Last error: {channel.last_error}</p>}
    <div className="flex gap-2">
      <button type="button" className="trade-button" disabled={pending || !changed || !valid || mode === "disabled"} onClick={() => void action(false)}>Save filters</button>
      <button type="button" className="trade-button" disabled={pending || !channel.enabled || mode === "disabled"} onClick={() => void action(true)}>Test {channel.id}</button>
    </div>
    {message && <p role={error ? "alert" : "status"} className={`text-xs ${error ? "text-warn" : "text-muted"}`}>{message}</p>}
  </div>
}

export function NotificationSettings() {
  const { status, source, trading } = useLive()
  const notifications = status?.notifications
  const mode = source === "live" && notifications?.enabled ? trading?.write ?? "disabled" : "disabled"
  return <section className="space-y-2">
    <h3 className="text-xs font-medium uppercase tracking-wide text-muted">Notifications</h3>
    <p className="text-xs text-muted">Send paper trading alerts to Discord, Telegram, ntfy or a webhook while this browser is closed.</p>
    <p className="text-xs text-muted">{notifications?.include_simulated
      ? "Demo and replay forwarding is on. Alerts from these runs are forwarded to enabled channels matching their event filters."
      : "Demo and replay forwarding is off. Alerts from these runs are not forwarded, even when a channel is enabled. Set include_simulated to true in the server notification config to opt in."} Backtests never send.</p>
    {source !== "live" && <p className="text-sm text-muted">Switch to live to manage notifications.</p>}
    {!notifications?.channels.length ? <p className="text-sm text-muted">No channels configured. Add them with the server's --notify-config file or OPENPORT_NOTIFY_JSON environment variable.</p>
      : <>
        {notifications.channels.map((channel) => <Channel key={`${channel.id}:${channel.enabled}:${channel.events.join()}:${channel.floor_distance}`} channel={channel} mode={mode} />)}
        <p className="text-xs text-muted">Queue: {notifications.queue_depth}/{notifications.queue_capacity} · {notifications.dropped} dropped.
          Filters apply until restart. Save lasting changes in the server config. Destinations and credentials are managed there and never shown here.</p>
      </>}
  </section>
}
