# LLM eval harness (autonomous player)

This builds an autonomous "LLM player" into the Qt6/QML client. When enabled, the
LLM takes the **hero seat (seat 0)** of a local single‑player game and plays full
tournaments against PokerTH's built‑in heuristic bots, with no human input. Every
decision is logged to a JSONL file so you can score how well a model plays.

The model itself is **not** run on the device — the client calls an
OpenAI‑compatible *chat‑completions* endpoint over your LAN (Ollama / llama.cpp /
vLLM / etc.). This is what makes it viable on a Raspberry Pi 5: the Pi only runs
the game and makes HTTP requests.

## How it works

* When it's the hero's turn, the engine calls `meInAction()` →
  `GameHandler::onMeInAction()`. If autopilot is enabled, instead of waiting for a
  click, `GameHandler` builds a JSON snapshot of the table (hole cards, board,
  pot, stacks, positions and the **exact legal action menu**) and hands it to
  `LlmPlayer::requestDecision()`.
* The snapshot also carries **accumulated context** so decisions aren't stateless:
  `hand_history` (the betting action this hand, by street), `recent_hands`
  (compact summaries of the last 8 finished hands — final board, pot, winners,
  shown cards — *including hands the hero folded*, so it can build reads), and
  `opponent_stats` (per-opponent actions-seen / aggressive% / fold%). These are
  fed from the engine's log callbacks, which fire for the whole hand even after
  the hero is out of it. The model also explains each move via a `reasoning`
  field, shown in the in-game action log and recorded in the JSONL.
* `LlmPlayer` POSTs the snapshot to the chat‑completions endpoint
  (`QNetworkAccessManager`, asynchronous — the UI never freezes), parses the
  reply, **validates and clamps** it to a legal move, logs everything, and emits
  `decisionReady(action, amount)`.
* `GameHandler::onLlmDecision()` applies the move through the *same* functions a
  human click uses (`fold()/call()/raise()/allIn()`), so all the betting/side‑pot
  rules are reused unchanged.
* On any failure (network error, timeout, garbled output) the harness falls back
  to a safe legal move (check if free, otherwise fold) and records the reason — an
  unreachable or slow LAN server degrades gracefully instead of hanging.
* Between hands the next hand auto‑starts, so a whole tournament plays itself.

Source: `src/gui/qt6-qml/cpp/llmplayer.{h,cpp}` plus the hooks in
`gamehandler.cpp` (`onMeInAction`, `onLlmDecision`, `buildLlmObservation`) and the
delay reductions in `qmlguiinterface.cpp`.

## Configuration (environment variables)

| Variable | Default | Meaning |
| --- | --- | --- |
| `POKERTH_LLM_ENABLE` | `0` | `1` turns the autopilot on (also shrinks animation/think delays). |
| `POKERTH_LLM_ENDPOINT` | — | Full chat‑completions URL, e.g. `http://192.168.1.50:11434/v1/chat/completions`. Required. |
| `POKERTH_LLM_MODEL` | `gpt-3.5-turbo` | Model name passed to the server. |
| `POKERTH_LLM_API_KEY` | — | Optional bearer token (omit for most LAN servers). |
| `POKERTH_LLM_TEMPERATURE` | `0.7` | Sampling temperature. |
| `POKERTH_LLM_TIMEOUT_MS` | `30000` | Per‑request transfer timeout. |
| `POKERTH_LLM_JSON_MODE` | `1` | Send `response_format: json_object`. Set `0` if your server rejects it. |
| `POKERTH_LLM_MAX_TOKENS` | `1024` | Max output tokens per decision. |
| `POKERTH_LLM_CONTEXT` | auto | Context window in tokens for the on-screen `%`. Auto-detected from llama.cpp `/props` (`n_ctx`); set to override or for non-llama servers. |
| `POKERTH_LLM_LOG` | `~/pokerth_llm_eval.jsonl` | JSONL decision log path. |

Each decision's token usage (`usage.prompt_tokens/completion_tokens/total_tokens`,
plus `context_size`) is recorded in the JSONL and shown live in the chance panel
(`ctx 1,234 / 131,072 (1%)`). `analyze_llm_eval.py` summarises prompt-token
min/median/mean/peak and the peak as a % of the window — useful for watching the
session history (`recent_hands`, etc.) grow against the context limit.

Autopilot only engages when `POKERTH_LLM_ENABLE=1` **and** an endpoint is set.
With it off, the client behaves exactly as before.

### Using a `.env` file

Instead of `export`ing these each time, put them in a `.env` file. At startup the
client loads the first of: `$POKERTH_LLM_ENV` (explicit path), `./.env` (the
directory you launch it from), then `~/.pokerth_llm.env`. A real environment
variable always overrides the file. Lines are `KEY=VALUE`; `#` comments, blank
lines, a leading `export `, and surrounding quotes are all accepted.

```bash
cp .env.example .env      # then edit .env
./pokerth_qml-client      # no exports needed
```

It logs `[LLM] loaded config from <path>` at startup so you can confirm which file
was used. `.env` is gitignored (it may hold an API key); `.env.example` is the
tracked template.

## Running

Build the QML client (the autopilot lives only in `pokerth_qml-client`):

```bash
BUILD_TARGET=pokerth_qml-client ./build_macos.sh    # macOS
# or a normal CMake build of the pokerth_qml-client target on Linux / RPi5
```

Then, e.g. against an Ollama server on the LAN:

```bash
export POKERTH_LLM_ENABLE=1
export POKERTH_LLM_ENDPOINT=http://192.168.1.50:11434/v1/chat/completions
export POKERTH_LLM_MODEL=llama3.1:8b
./pokerth_qml-client
```

Start a local game from the UI (Local Game). The hero seat plays itself; watch the
table or tail the log:

```bash
tail -f ~/pokerth_llm_eval.jsonl | python3 -m json.tool --json-lines
```

## Scoring

Each JSONL line is one decision:

```json
{"ts":"...","model":"llama3.1:8b","observation":{...,"hero":{"stack":1490,...}},
 "raw":"{\"action\":\"raise\",\"amount\":40,...}","action":"raise","amount":40,
 "status":"ok","latency_ms":820,"http_status":200}
```

`observation.hero.stack` is captured at every decision, so the log already gives
the model's chip trajectory over the session plus action mix, fallback rate and
latency. Summarise a run with:

```bash
python3 analyze_llm_eval.py ~/pokerth_llm_eval.jsonl
```

## v1 limitations / next steps

* **Opponents are the built‑in bots** (a fixed, reproducible benchmark). Model‑vs‑
  model needs lifting the hardcoded "seat 0 is human" rule — a later phase.
* **Per‑decision outcome logging** (the chip result of *this* decision) isn't
  written yet; the stack trajectory and the `recent_hands` results in each
  observation are good proxies. A dedicated per-hand result record could be added.
* **Recent-hand depth is a constant** (`kLlmMaxRecentHands = 8` in `gamehandler.h`).
  Bump it there if you want a longer memory (at some token/latency cost).
* **Windowed only.** Runs need a display (delays are shrunk, not removed). A
  headless `GuiInterface` for throughput sweeps is a later phase.
