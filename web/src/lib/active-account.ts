import type { DestinationChangeReason } from "./destination-change"
import { useSyncExternalStore } from "react"

const key = "openport.account"
export const MAIN_ACCOUNT = "main"

/** The paper account the terminal acts on, remembered per browser; the main one when storage is unavailable. */
export function createAccountStore(storage: () => Pick<Storage, "getItem" | "setItem">) {
  let memory: string | undefined
  const listeners = new Set<(reason: DestinationChangeReason) => void>()
  return {
    get() {
      if (memory !== undefined) return memory
      try { memory = storage().getItem(key) || MAIN_ACCOUNT } catch { memory = MAIN_ACCOUNT }
      return memory
    },
    set(id: string, reason: DestinationChangeReason = "automatic") {
      memory = id || MAIN_ACCOUNT
      try { storage().setItem(key, memory) } catch { /* Keep the in-memory choice. */ }
      listeners.forEach((listener) => listener(reason))
    },
    subscribe(listener: (reason: DestinationChangeReason) => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const activeAccount = createAccountStore(() => window.localStorage)
export function useActiveAccount() {
  return useSyncExternalStore(activeAccount.subscribe, activeAccount.get, () => MAIN_ACCOUNT)
}
