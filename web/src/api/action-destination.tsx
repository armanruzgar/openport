import { createContext, useContext, useRef, type ReactNode } from "react"
import { captureDestination, type Destination } from "./destination"

export const ActionDestination = createContext<Destination | null>(null)
export function useActionDestination(scope?: number) {
  const boundary = useContext(ActionDestination)
  // An explicit scope belongs to a persistent control, even inside a settings dialog.
  const inherited = scope === undefined ? boundary : null
  const captured = useRef({ scope, destination: inherited ?? captureDestination() })
  if (captured.current.scope !== scope) captured.current = { scope, destination: inherited ?? captureDestination() }
  return inherited ?? captured.current.destination
}
export function ActionBoundary({ children }: { children: ReactNode }) {
  const destination = useActionDestination()
  return <ActionDestination value={destination}>{children}</ActionDestination>
}
