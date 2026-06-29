/*****************************************************************************
 * PokerTH - The open source texas holdem engine                             *
 * Copyright (C) 2006-2025 Felix Hammer, Florian Thauer, Lothar May          *
 *****************************************************************************/

#ifndef LLMPLAYER_H
#define LLMPLAYER_H

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;

// Autonomous "LLM player" for the local eval harness.
//
// GameHandler builds a JSON snapshot of the table (hole cards, board, pot,
// stacks, positions and the LEGAL action menu) and hands it to requestDecision().
// LlmPlayer asks an OpenAI-compatible chat-completions endpoint for a poker
// decision, validates+clamps the answer to a legal move, appends the full
// decision (state, raw model output, chosen action, latency) to a JSONL log for
// offline model scoring, and finally emits decisionReady() with a ready-to-apply
// action. On any failure (network error, timeout, garbled output) it falls back
// to a safe legal move (check if free, otherwise fold) so an eval run never hangs.
//
// Configured via environment variables, which may also be supplied from a .env
// file (a real env var always overrides the file). The .env is looked up in this
// order: $POKERTH_LLM_ENV (explicit path), ./.env (launch dir), ~/.pokerth_llm.env.
//   POKERTH_LLM_ENABLE       "1" to turn the autopilot on (default off)
//   POKERTH_LLM_ENDPOINT     full chat-completions URL of the LAN server, e.g.
//                            http://192.168.1.50:11434/v1/chat/completions
//   POKERTH_LLM_MODEL        model name passed to the server (default gpt-3.5-turbo)
//   POKERTH_LLM_API_KEY      optional bearer token (omit for most LAN servers)
//   POKERTH_LLM_TEMPERATURE  sampling temperature (default 0.7)
//   POKERTH_LLM_TIMEOUT_MS   per-request transfer timeout (default 30000)
//   POKERTH_LLM_LOG          JSONL decision-log path (default ~/pokerth_llm_eval.jsonl)
class LlmPlayer : public QObject
{
	Q_OBJECT
public:
	explicit LlmPlayer(QObject *parent = nullptr);
	~LlmPlayer() override;

	// True when the autopilot is configured (enable flag set AND an endpoint given).
	bool enabled() const { return m_enabled; }
	QString model() const { return m_model; }
	// Loaded context window in tokens (from llama.cpp /props or POKERTH_LLM_CONTEXT);
	// 0 if unknown.
	int contextSize() const { return m_contextSize; }

	// Kick off ONE asynchronous decision for the given observation. Exactly one
	// decisionReady() follows (possibly a fallback move) per call. The caller is
	// expected to serialise calls (one decision in flight at a time).
	void requestDecision(const QJsonObject &observation);

	// Clear the replayed reasoning conversation (call when a new game starts).
	void resetConversation();

signals:
	// A legal, ready-to-apply decision. action is one of:
	//   "fold", "check", "call", "bet", "raise", "allin".
	// For "bet"/"raise", amount is the additional-chips value to pass straight to
	// GameHandler::raise(); 0 for every other action. reasoning is the model's
	// short self-explanation (may be empty), for display/logging only.
	void decisionReady(const QString &action, int amount, const QString &reasoning);

	// Token usage of the latest decision request, for context-length tracking.
	// contextSize is the loaded window (0 if unknown).
	void contextUsage(int promptTokens, int completionTokens, int totalTokens, int contextSize);

private slots:
	void onReplyFinished();
	void onPropsFinished();

private:
	// Best-effort GET of llama.cpp /props to learn the context window (n_ctx).
	void fetchContextSize();
	QString buildSystemPrompt() const;
	QString buildUserPrompt(const QJsonObject &obs) const;
	// Compact one-line situation recap used as the user turn in replayed history.
	QString compactRecap(const QJsonObject &obs) const;
	// Parse the model's reply text and map+clamp it against the legal options in
	// obs. Fills outAction/outAmount with a legal move; sets status to "ok" or a
	// short diagnostic ("raise_clamped_max", "unknown_action:foo", ...).
	void decideFromText(const QJsonObject &obs, const QString &content,
	                    QString &outAction, int &outAmount, QString &outReasoning,
	                    QString &status) const;
	// Safe legal fallback: check if free, otherwise fold.
	void fallback(const QJsonObject &obs, QString &outAction, int &outAmount) const;
	void logDecision(const QJsonObject &obs, const QString &rawContent,
	                 const QString &action, int amount, const QString &reasoning,
	                 const QString &status, qint64 latencyMs, int httpStatus,
	                 const QJsonObject &usage) const;

	bool m_enabled = false;
	QString m_endpoint;
	QString m_model;
	QString m_apiKey;
	double m_temperature = 0.7;
	int m_timeoutMs = 30000;
	// Output token budget. Generous by default so reasoning models have room to
	// think AND still emit the JSON answer. POKERTH_LLM_MAX_TOKENS overrides.
	int m_maxTokens = 8192;
	// Send {"response_format":{"type":"json_object"}} (OpenAI/vLLM/llama.cpp). Set
	// POKERTH_LLM_JSON_MODE=0 for servers that reject the field.
	bool m_jsonMode = true;
	QString m_logPath;

	QNetworkAccessManager *m_nam = nullptr;
	// Observation tied to the in-flight reply, used for validation + logging when
	// the reply arrives. Safe because callers keep one request in flight.
	QJsonObject m_pendingObs;
	qint64 m_requestStartMs = 0;

	// Context-length tracking.
	int m_contextSize = 0;        // loaded n_ctx (tokens), 0 = unknown
	int m_lastPromptTokens = 0;
	int m_peakPromptTokens = 0;

	// Reasoning hydration: replay the last N turns as a conversation so the model
	// sees its own prior chain-of-thought. 0 = off (POKERTH_LLM_REASONING_TURNS).
	int m_reasoningTurns = 0;
	QString m_pendingRecap;       // recap of the in-flight turn (stored on reply)
	QStringList m_priorRecaps;    // user-side recap per remembered turn
	QStringList m_priorAssistant; // assistant reply (reasoning + answer) per turn
};

#endif // LLMPLAYER_H
