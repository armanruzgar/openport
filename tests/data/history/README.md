# Historical API fixtures

These are hand-written response shapes with generated values, not downloaded
market data. Prices, sizes and identifiers are simulated test inputs. No provider
key or licensed response is stored here.

Theta files use the v3 NDJSON shapes for lists, option history, stock quotes and
index prices. `databento.json` saves DBN field values as readable JSON; the test
loads them into the vendored SDK's record structs before invoking its callback.
`massive_*.json` contains contract reference, quotes and trades response shapes;
SIP and exchange timestamps are explicit, including a late trade across windows.
All paths exercise the production converters without network access.
