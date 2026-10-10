import { useSyncExternalStore } from "react"

const key = "openport.write-token"
/** Memory remains usable when storage is denied (including access to the getter). */
export function createTokenStore(storage: () => Pick<Storage, "getItem" | "setItem" | "removeItem">) {
  let memory: string | undefined
  let sandbox: string | undefined
  const listeners = new Set<() => void>()
  return {
    get() {
      if (memory !== undefined) return memory
      try { memory = storage().getItem(key) ?? "" } catch { memory = "" }
      return memory
    },
    sandboxAccount() {
      if (sandbox === undefined) {
        try { sandbox = storage().getItem(`${key}.sandbox`) ?? "" } catch { sandbox = "" }
      }
      return sandbox
    },
    set(token: string, account = "") {
      memory = token.trim()
      sandbox = memory ? account : ""
      let persisted = true
      try {
        if (memory) storage().setItem(key, memory)
        else storage().removeItem(key)
        if (sandbox) storage().setItem(`${key}.sandbox`, sandbox)
        else storage().removeItem(`${key}.sandbox`)
      } catch {
        persisted = false
        try { storage().removeItem(key); storage().removeItem(`${key}.sandbox`) } catch { /* Memory still works. */ }
      }
      listeners.forEach((listener) => listener())
      return persisted
    },
    subscribe(listener: () => void) { listeners.add(listener); return () => { listeners.delete(listener) } },
  }
}
export const writeToken = createTokenStore(() => window.sessionStorage)

export const isSandboxToken = (token: string) => !!token && token === writeToken.get() && !!writeToken.sandboxAccount()

type TokenStore = ReturnType<typeof createTokenStore>
/** A link carrying the write token after "#token=", which openportd prints when it
 * keeps its token in a file (as the Docker image does), saves it for this tab. The
 * fragment never reaches the server, and the address bar is left without it. */
export function adoptLinkedToken(location: Pick<Location, "hash" | "pathname" | "search">,
                                 history: Pick<History, "replaceState">, store: TokenStore = writeToken) {
  const match = /^#token=([^&#]*)$/.exec(location.hash)
  if (!match) return false
  let token = ""
  try { token = decodeURIComponent(match[1] ?? "").trim() } catch { /* a malformed link saves nothing */ }
  history.replaceState(null, "", location.pathname + location.search)
  if (!token) return false
  store.set(token)
  return true
}
/** Adopts a linked token now and on every later hash change: opening the link in a tab
 * already showing the terminal changes only the hash, without a reload. Register this
 * before the router subscribes, so the router sees the cleared address, not the token. */
export function watchLinkedToken(target: Pick<Window, "location" | "history" | "addEventListener">,
                                 store: TokenStore = writeToken) {
  adoptLinkedToken(target.location, target.history, store)
  target.addEventListener("hashchange", () => { adoptLinkedToken(target.location, target.history, store) })
}
export function useWriteToken() {
  return useSyncExternalStore(writeToken.subscribe, writeToken.get, () => "")
}
