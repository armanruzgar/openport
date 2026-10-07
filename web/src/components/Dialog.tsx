import { ActionBoundary } from "../api/action-destination"
import { useEffect, useId, useRef, type ReactNode } from "react"

/** Native modal supplies focus trapping and makes the rest of the app inert. */
export function Dialog({ title, onClose, children }: { title: string; onClose: () => void; children: ReactNode }) {
  const ref = useRef<HTMLDialogElement>(null)
  const close = useRef(onClose)
  close.current = onClose
  const titleId = useId()
  useEffect(() => {
    const previous = document.activeElement
    const dialog = ref.current
    dialog?.showModal()
    return () => {
      dialog?.close()
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus()
    }
  }, [])
  return (
    <dialog ref={ref} aria-labelledby={titleId} onCancel={(event) => { event.preventDefault(); close.current() }}
      onKeyDown={(event) => event.stopPropagation()}
      className="m-auto max-h-[90dvh] w-[calc(100%-1.5rem)] max-w-xl overflow-y-auto rounded-lg border border-border bg-panel p-0 text-foreground shadow-chart backdrop:bg-black/50">
      <header className="flex items-center justify-between gap-3 border-b border-border px-4 py-3">
        <h2 id={titleId} className="font-medium">{title}</h2>
        <button type="button" className="trade-button" onClick={onClose} aria-label={`Close ${title}`}>Close</button>
      </header>
      <div className="space-y-4 p-4"><ActionBoundary>{children}</ActionBoundary></div>
    </dialog>
  )
}
