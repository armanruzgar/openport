import type { FeedState, ProviderInfo, UnderlyingSnapshot } from "../api/types"
import { isNum } from "./format"

export function providerLabel(provider: ProviderInfo, state?: FeedState | null): string {
  if (provider.simulated) return "simulated prices"
  const delayed = isNum(provider.delay_seconds) && provider.delay_seconds > 0
    ? `${provider.delay_seconds / 60}-min delay` : "delayed"
  if (provider.realtime_plan_dependent) {
    if (state === "live") return "real-time"
    if (state === "delayed") return delayed
    return "timing unconfirmed"
  }
  return provider.realtime ? "real-time" : delayed
}

/** "fallback · cboe" while an underlying's backup feed supplies it, else null. */
export function fallbackLabel(provider: ProviderInfo | null | undefined, underlying: UnderlyingSnapshot | null | undefined): string | null {
  if (!provider?.fallback || !underlying?.fallback) return null
  return `fallback · ${underlying.feed ?? provider.fallback.backup}`
}

/** Why the backup supplies an underlying, for a tooltip. */
export function fallbackTitle(provider: ProviderInfo): string {
  const f = provider.fallback
  if (!f) return ""
  return `${f.primary} has been in error or silent for ${f.after_seconds} s, so ${f.backup} supplies this underlying until ${f.primary} has been fresh for as long; its data is reported as delayed`
}
