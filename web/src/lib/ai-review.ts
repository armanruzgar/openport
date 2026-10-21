import type { JournalTrade } from "./journal"

export interface AiReviewSettings { enabled: boolean; endpoint: string; key: string }
/** No automatic requests. The key stays in component memory; fetch sends no cookies. */
export async function requestAiReview(settings: AiReviewSettings, trade: JournalTrade, flags: string[]): Promise<string> {
  if (!settings.enabled || !settings.endpoint || !settings.key) throw new Error("AI review is off. Configure an endpoint and your own key first.")
  const url = new URL(settings.endpoint)
  if (url.protocol !== "https:" && !(url.protocol === "http:" && ["localhost", "127.0.0.1", "[::1]"].includes(url.hostname)))
    throw new Error("Use HTTPS, or HTTP on localhost.")
  if (url.username || url.password) throw new Error("Put the key in the key field, not the URL.")
  const response = await fetch(url, { method: "POST", credentials: "omit", redirect: "error", signal: AbortSignal.timeout(30_000),
    headers: { "Content-Type": "application/json", Authorization: `Bearer ${settings.key}` },
    body: JSON.stringify({ trade, flags, paper_trade: true, instruction: "Review this paper trade for practice. Explain uncertainty; do not invent market observations." }) })
  if (!response.ok) throw new Error(`AI review endpoint returned HTTP ${response.status}`)
  const result: unknown = await response.json()
  if (!result || typeof result !== "object" || !("review" in result) || typeof result.review !== "string") throw new Error("Expected a JSON object with a review string.")
  return result.review
}
