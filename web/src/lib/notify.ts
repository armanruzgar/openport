import { notificationHistory, type NotificationKind } from "./notification-history"
import { useSyncExternalStore } from "react"

export type Permission = NotificationPermission | "unsupported"
export function notificationPermission(): Permission {
  return typeof Notification === "undefined" ? "unsupported" : Notification.permission
}
export async function requestNotifications(): Promise<Permission> {
  if (typeof Notification === "undefined") return "unsupported"
  if (Notification.permission !== "default") return Notification.permission
  try { return await Notification.requestPermission() } catch { return Notification.permission }
}

export interface Toast { id: number; title: string; body: string }
/** Messages shown inside the terminal, each for a few seconds. */
export function createToasts(timeout = 8000) {
  let toasts: Toast[] = []
  let next = 1
  const listeners = new Set<() => void>()
  const emit = () => listeners.forEach((listener) => listener())
  const dismiss = (id: number) => {
    toasts = toasts.filter((t) => t.id !== id)
    emit()
  }
  return {
    get: () => toasts,
    push(title: string, body: string) {
      const toast = { id: next++, title, body }
      toasts = [...toasts.slice(-3), toast]
      emit()
      if (timeout > 0) setTimeout(() => dismiss(toast.id), timeout)
    },
    dismiss,
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const toasts = createToasts()
const none: Toast[] = []
export function useToasts() {
  return useSyncExternalStore(toasts.subscribe, toasts.get, () => none)
}

let audio: AudioContext | null = null
/** A short two-note chime, made on the fly. */
function chime() {
  try {
    audio ??= new AudioContext()
    const start = audio.currentTime
    for (const [offset, frequency] of [[0, 880], [0.14, 1175]] as const) {
      const tone = audio.createOscillator()
      const gain = audio.createGain()
      tone.frequency.value = frequency
      gain.gain.setValueAtTime(0.0001, start + offset)
      gain.gain.exponentialRampToValueAtTime(0.12, start + offset + 0.02)
      gain.gain.exponentialRampToValueAtTime(0.0001, start + offset + 0.25)
      tone.connect(gain).connect(audio.destination)
      tone.start(start + offset)
      tone.stop(start + offset + 0.26)
    }
  } catch { /* No audio in this browser. */ }
}

/** Show an alert in the terminal, as a browser notification when allowed, and with a chime. */
export function notify(title: string, body: string, sound: boolean, context?: { kind: NotificationKind; scope: string; id?: string; quiet?: boolean }) {
  notificationHistory.add({ id: context?.id ?? crypto.randomUUID(), title, body, kind: context?.kind ?? "alert", scope: context?.scope ?? "browser" })
  if (context?.quiet) return
  toasts.push(title, body)
  if (notificationPermission() === "granted") {
    try { new Notification(title, { body }) } catch { /* Some browsers only notify from a service worker. */ }
  }
  if (sound) chime()
}
