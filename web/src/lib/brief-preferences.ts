import { useSyncExternalStore } from "react"

const key = "openport.brief-levels"
const event = "openport:brief-levels"
// The choice this page could not store; it outranks a stale stored one.
let fallback: boolean | undefined
export function loadBriefLevels() {
  if (fallback !== undefined) return fallback
  try { return localStorage.getItem(key) === "true" } catch { return false }
}
export function saveBriefLevels(value: boolean) {
  // Private browsing can deny storage, and full storage refuses writes.
  try { localStorage.setItem(key, String(value)); fallback = undefined } catch { fallback = value }
  window.dispatchEvent(new Event(event))
}
function subscribe(callback: () => void) {
  window.addEventListener(event, callback)
  window.addEventListener("storage", callback)
  return () => { window.removeEventListener(event, callback); window.removeEventListener("storage", callback) }
}
export const useBriefLevels = () => useSyncExternalStore(subscribe, loadBriefLevels, loadBriefLevels)
