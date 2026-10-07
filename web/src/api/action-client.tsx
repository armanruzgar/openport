import { useActionDestination } from "./action-destination"
import { useEffect, useRef, useSyncExternalStore } from "react"
import { api } from "./client"
import { DestinationChangedError, destinationLabel, withDestination, type Destination } from "./destination"
import { Dialog } from "../components/Dialog"

type Confirmation = { error: DestinationChangedError; resolve: (destination: Destination | null) => void }
let pending: Confirmation[] = []
const listeners = new Set<() => void>()
function emit() { listeners.forEach(listener => listener()) }
function settle(item: Confirmation, destination: Destination | null) {
  pending = pending.filter(value => value !== item)
  item.resolve(destination)
  emit()
}
export function DestinationConfirmation() {
  const items = useSyncExternalStore(listener => { listeners.add(listener); return () => { listeners.delete(listener) } }, () => pending)
  const item = items[0]
  if (!item) return null
  return <Dialog title="Confirm changed destination" onClose={() => settle(item, null)}>
    <p role="alert">{item.error.message}</p>
    <button className="trade-button" onClick={() => settle(item, item.error.current)}>Send to {destinationLabel(item.error.current)}</button>
    <button className="trade-button" onClick={() => settle(item, null)}>Cancel</button>
  </Dialog>
}

/** Bind at action/view mount, not at submit. Only account mutations check the guard.
 * Persistent account controls can supply accountScope to rebind on switches.
 * Dialogs/tickets omit it so their opening destination stays fixed.
 * A confirmation authorizes only this invocation; previews never open confirmations.
 */
export function useActionApi(scope?: number): typeof api {
  const destination = useActionDestination(scope)
  const mounted = useRef(true)
  const owned = useRef(new Set<Confirmation>())
  useEffect(() => {
    mounted.current = true
    const requests = owned.current
    return () => { mounted.current = false; requests.forEach(item => settle(item, null)); requests.clear() }
  }, [])
  return new Proxy(api, {
    get(target, key: keyof typeof api) {
      const method = target[key] as (...args: unknown[]) => unknown
      return (...args: unknown[]) => {
        const invoke = (expected: Destination) => withDestination(expected, () => method(...args))
        const result = invoke(destination)
        if (!(result instanceof Promise)) return result
        return result.catch(async (error: unknown) => {
          if (!mounted.current || !(error instanceof DestinationChangedError) || String(key).startsWith("preview") || key === "whatIf") throw error
          const approved = await new Promise<Destination | null>(resolve => {
            const item: Confirmation = { error, resolve: value => { owned.current.delete(item); resolve(value) } }
            owned.current.add(item)
            pending = [...pending, item]
            emit()
          })
          if (!approved || !mounted.current) throw error
          // Recheck at send: another change while confirming cannot redirect the write.
          return invoke(approved)
        })
      }
    },
  })
}
