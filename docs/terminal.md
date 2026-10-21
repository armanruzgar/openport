# Terminal controls and saved layouts

All trades, fills, exercises and assignments in OpenPort are simulated. The
terminal uses the configured feed's prices and timestamps, including its delay.
A chart overlay or expected move is a derived estimate, not an executable quote.

## Keyboard and touch

Press **?** or the **?** button for the shortcut dialog. **Ctrl/Cmd+K** opens
symbol search over the server's subscribed underlyings. Arrow through the search
results and press Enter to jump. The terminal does not add runtime subscriptions.
Number keys select the sidebar pages in their displayed order; left/right outside
the chain retain the existing previous/next-expiry behavior.

Tab into a chain quote. Up/down moves through strikes, left/right chooses calls
or puts, and Home/End jumps to the first/last strike. **B** prefills a buy at the
ask; **S** prefills a sell at the bid. Enter on a quote opens the ticket for that
cell: buy at the ask, sell at the bid.
These actions use the latest loaded quote and retain the server's trading checks.
In strategy mode they add or remove a leg using the same rules as clicking quotes.

Inside a ticket, Enter from a non-editable control moves focus to its order
review; buttons and toggles keep Enter for their own action, and Enter in a field
submits the form like its submit button. Escape closes the ticket and keeps any
strategy legs already picked; native modal dialogs trap focus and restore it when closed.
On Positions, focus a position row and press **F** to open that contract's
existing close ticket. It does not flatten the entire account or bypass review.
Shortcuts ignore editable inputs, composition and held-key repeats.

Below 640px the chain shows one side, switched with **Calls / Puts**, and the
compact strike, IV, bid and ask columns. Desktop column preferences remain saved.
Tickets use a bottom sheet with the same form, review and trading checks; larger
touch controls and internal panel scrolling keep the page usable at 375px.

## Preferences and workspaces

Each symbol remembers its strike window, Quotes/Greeks selection, liquidity
columns, single/strategy ticket mode and stacked-expiry selection in localStorage.
The chart remembers its interval, visibility and study settings. Storage errors
fall back to defaults or keep the current in-memory choice; private-browser
restrictions do not block trading controls.

**Workspaces** saves a named snapshot of the current symbol, hash route (including
expiry), chain preferences and chart settings. Saving an existing name replaces
it. Restore or delete a workspace from the same dialog. Up to 20 workspaces are
kept locally. Drawings remain associated with their symbol, independently of
workspace selection. Neither workspace restoration nor a pop-out changes the
paper account or submits an order. A pop-out opens in the same origin with this
tab as its opener, so the browser copies the tab's session write token into it;
the token never goes in the pop-out's address. Restoring a workspace, or changing
the ticket mode in another tab, clears the open ticket and picked legs, as the
Single / Strategy toggle does.

**Stack expiries** adds collapsible sections below the selected chain. Only opened
sections and expiries needed by strategy legs are fetched. Each chain renders a
small window of rows, with overscan and native table spacer rows. Unchanged visible
rows are memoized across quote updates. The server still returns the requested
chain; windowing reduces browser rendering work, not response size.

## Watchlist and notifications

Expand **Watchlist** to see the subscribed symbols, last underlying price, change
against the prior available daily candle, nearest-active-expiry ATM IV, IV rank
and derived one-standard-deviation expected move. Missing history and analytics
remain missing. Parity spot and proxy IV history are marked. Expected move is
`spot × ATM IV × sqrt(days / 365)`; the selected expiry and market-data timestamp
are shown. It is not a forecast or a guarantee of price bounds.

The bell opens the notification drawer under the header. It retains up to 200
recent events across accounts: observed fills, assignments/exercises, dividends,
rule breaches and browser/server alert firings. It shows the current account and
source plus browser price alerts. **Mark all read** clears unread badges;
**Clear** removes that scope's local history. Toasts remain available, and fills
are recorded even when fill toasts are disabled. Receipt times are explicitly
browser receipt times, not invented market timestamps.

The current server API has notification channel configuration and test actions,
but no notification-history endpoint. This drawer therefore records events the
browser observes after its initial account snapshot and persists them locally;
it is not an audit log of events that happened while the browser was closed.
Use Orders and Journal for the server's retained trade records.

## Chart studies and annotations

SMA and EMA use loaded closing prices and a configurable 2–200-bar period (default
20). EMA starts at the first complete SMA. Bollinger bands use that SMA plus/minus
two population standard deviations. Studies have a full-period warm-up and are
calculated before the visible window, so panning does not restart the average.
They are derived from the loaded history, which may be shorter than a full session.

The VWAP calculation uses typical price `(high + low + close) / 3`, weighted by
bar volume and reset by New York calendar date. A missing volume makes that
session unavailable. **The current candle endpoint supplies OHLC without volume,
so VWAP is unavailable and the chart explicitly says so.** No volume is fabricated
or substituted with option-chain volume. Enabling a real VWAP display requires a
future volume-bearing underlying candle source/API.

**Draw level** places a horizontal annotation with one chart tap. **Draw trendline**
uses two taps on different bars. **Cancel drawing** exits drawing mode. The
**Drawings** section also accepts numeric prices and actual loaded bar timestamps,
so drawing does not require a pointer. It lists saved annotations and their remove
actions. A horizontal line's **Create price alert** opens the existing alert form
at that level; adding the alert still requires the form's confirmation.

Up to 100 drawings are stored per symbol. Trendlines retain their actual candle
timestamps across intervals and are shown when both anchors are in the loaded
history. Annotations are user input, not market observations.

## Multiple monitors

The chain, chart, positions and orders offer **Open in new window** actions using
ordinary hash routes. Pop-ups depend on browser permission; the links also support
normal link navigation. Each window runs its own terminal connection and refreshes
its own data. At widths of at least 1536px, Positions and Orders can be shown beside
one another with **Positions + orders**; narrower screens show the selected page.
