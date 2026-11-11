import "@fontsource/geist-sans/400.css"
import "@fontsource/geist-sans/500.css"
import "@fontsource/geist-sans/600.css"
import "@fontsource/geist-mono/400.css"
import "./index.css"

import { QueryClientProvider } from "@tanstack/react-query"
import { StrictMode } from "react"
import { createRoot } from "react-dom/client"
import { App } from "./App"
import { LiveProvider } from "./api/live"
import { createQueryClient } from "./api/query-client"
import { ErrorBoundary } from "./components/ErrorBoundary"
import { watchLinkedToken } from "./lib/write-token"

const queryClient = createQueryClient()

watchLinkedToken(window)

const root = document.getElementById("root")
if (!root) throw new Error("missing #root")

createRoot(root).render(
  <StrictMode>
    <QueryClientProvider client={queryClient}>
      {/* Pages have their own boundaries; this one catches the rest, so the root never goes blank. */}
      <ErrorBoundary label="The terminal">
        <LiveProvider>
          <App />
        </LiveProvider>
      </ErrorBoundary>
    </QueryClientProvider>
  </StrictMode>,
)
