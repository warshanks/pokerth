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
| `POKERTH_LLM_LOG` | `~/pokerth_llm_eval.jsonl` | JSONL decision log path. |

Autopilot only engages when `POKERTH_LLM_ENABLE=1` **and** an endpoint is set.
With it off, the client behaves exactly as before.

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
* **Per‑hand outcome logging** (chip delta per hand, tournament placement) isn't
  written yet; the stack trajectory is a good proxy. Hooking
  `GameHandler::onShowdown()` / `logPlayerWinsMsg()` would add exact results.
* **Windowed only.** Runs need a display (delays are shrunk, not removed). A
  headless `GuiInterface` for throughput sweeps is a later phase.
