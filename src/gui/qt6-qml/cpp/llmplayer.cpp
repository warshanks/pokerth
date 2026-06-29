/*****************************************************************************
 * PokerTH - The open source texas holdem engine                             *
 * Copyright (C) 2006-2025 Felix Hammer, Florian Thauer, Lothar May          *
 *****************************************************************************/

#include "llmplayer.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStringList>
#include <QProcessEnvironment>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QHash>
#include <QUrl>
#include <QDebug>

namespace {
// Parse a minimal .env file: one KEY=VALUE per line, '#' comments and blank lines
// ignored, an optional leading "export ", and optional surrounding single/double
// quotes on the value. Returns key -> value.
QHash<QString, QString> parseDotEnv(const QString &path)
{
	QHash<QString, QString> out;
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return out;
	QTextStream in(&f);
	while (!in.atEnd()) {
		QString line = in.readLine().trimmed();
		if (line.isEmpty() || line.startsWith('#'))
			continue;
		if (line.startsWith(QLatin1String("export ")))
			line = line.mid(7).trimmed();
		const int eq = line.indexOf('=');
		if (eq <= 0)
			continue;
		const QString key = line.left(eq).trimmed();
		QString val = line.mid(eq + 1).trimmed();
		if (val.size() >= 2 &&
		    ((val.startsWith('"') && val.endsWith('"')) ||
		     (val.startsWith('\'') && val.endsWith('\'')))) {
			val = val.mid(1, val.size() - 2);
		}
		if (!key.isEmpty())
			out.insert(key, val);
	}
	return out;
}

// Locate a .env: explicit POKERTH_LLM_ENV, else ./.env, else ~/.pokerth_llm.env.
QString findDotEnv()
{
	const QByteArray explicitPath = qgetenv("POKERTH_LLM_ENV");
	if (!explicitPath.isEmpty())
		return QString::fromLocal8Bit(explicitPath);
	if (QFileInfo::exists(QStringLiteral(".env")))
		return QStringLiteral(".env");
	const QString homeFile = QDir::home().filePath(QStringLiteral(".pokerth_llm.env"));
	if (QFileInfo::exists(homeFile))
		return homeFile;
	return QString();
}
} // namespace

LlmPlayer::LlmPlayer(QObject *parent)
	: QObject(parent)
{
	const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	const QString dotenvPath = findDotEnv();
	const QHash<QString, QString> dotenv =
	    dotenvPath.isEmpty() ? QHash<QString, QString>() : parseDotEnv(dotenvPath);

	// Resolve a setting: a real environment variable always wins; otherwise fall
	// back to the .env file, then the built-in default.
	auto cfg = [&](const char *key, const QString &def = QString()) -> QString {
		if (env.contains(QString::fromLatin1(key)))
			return env.value(QString::fromLatin1(key)).trimmed();
		if (dotenv.contains(QString::fromLatin1(key)))
			return dotenv.value(QString::fromLatin1(key)).trimmed();
		return def;
	};

	m_endpoint = cfg("POKERTH_LLM_ENDPOINT");
	m_model    = cfg("POKERTH_LLM_MODEL", "gpt-3.5-turbo");
	m_apiKey   = cfg("POKERTH_LLM_API_KEY");

	const QString enableFlag = cfg("POKERTH_LLM_ENABLE", "0");
	const bool wantEnabled = (enableFlag == "1" || enableFlag.compare("true", Qt::CaseInsensitive) == 0);
	m_enabled = wantEnabled && !m_endpoint.isEmpty();

	bool ok = false;
	const double temp = cfg("POKERTH_LLM_TEMPERATURE").toDouble(&ok);
	if (ok) m_temperature = temp;
	const int tmo = cfg("POKERTH_LLM_TIMEOUT_MS").toInt(&ok);
	if (ok && tmo > 0) m_timeoutMs = tmo;
	const int mt = cfg("POKERTH_LLM_MAX_TOKENS").toInt(&ok);
	if (ok && mt > 0) m_maxTokens = mt;
	const int ctx = cfg("POKERTH_LLM_CONTEXT").toInt(&ok);
	if (ok && ctx > 0) { m_contextSize = ctx; m_contextPinned = true; }
	const int rt = cfg("POKERTH_LLM_REASONING_TURNS").toInt(&ok);
	if (ok && rt >= 0) m_reasoningTurns = rt;
	// Scope of reasoning hydration: "turns" (rolling window of N), "hand" (reset
	// each hand), or "game" (reset each game/tournament).
	const QString scope = cfg("POKERTH_LLM_REASONING_SCOPE", "turns").trimmed().toLower();
	if (scope == "hand")       m_reasoningScope = ScopeHand;
	else if (scope == "game")  m_reasoningScope = ScopeGame;
	else                       m_reasoningScope = ScopeTurns;

	m_jsonMode = (cfg("POKERTH_LLM_JSON_MODE", "1") != "0");

	m_logPath = cfg("POKERTH_LLM_LOG");
	if (m_logPath.isEmpty())
		m_logPath = QDir::home().filePath("pokerth_llm_eval.jsonl");

	m_nam = new QNetworkAccessManager(this);

	if (!dotenvPath.isEmpty())
		qInfo() << "[LLM] loaded config from" << dotenvPath;

	if (wantEnabled && m_endpoint.isEmpty())
		qWarning() << "[LLM] POKERTH_LLM_ENABLE set but POKERTH_LLM_ENDPOINT is empty - autopilot disabled";
	if (m_enabled)
		qInfo() << "[LLM] autopilot ENABLED endpoint=" << m_endpoint
		        << "model=" << m_model << "log=" << m_logPath
		        << "reasoning_scope=" << (m_reasoningScope == ScopeHand ? "hand"
		                                  : m_reasoningScope == ScopeGame ? "game" : "turns")
		        << "reasoning_turns=" << m_reasoningTurns;

	// Learn the loaded context window from the server (unless set explicitly).
	if (m_enabled && m_contextSize == 0)
		fetchContextSize();
}

LlmPlayer::~LlmPlayer() = default;

void LlmPlayer::fetchContextSize()
{
	// llama.cpp serves GET /props with default_generation_settings.n_ctx. Derive
	// the URL from the chat endpoint (replace the /v1/... path, else use host root).
	QString propsUrl = m_endpoint;
	const int idx = propsUrl.indexOf(QStringLiteral("/v1/"));
	if (idx >= 0) {
		propsUrl = propsUrl.left(idx) + QStringLiteral("/props");
	} else {
		const QUrl u(m_endpoint);
		propsUrl = u.scheme() + QStringLiteral("://") + u.host()
		           + (u.port() > 0 ? QStringLiteral(":%1").arg(u.port()) : QString())
		           + QStringLiteral("/props");
	}

	QNetworkRequest req{QUrl(propsUrl)};
	if (!m_apiKey.isEmpty())
		req.setRawHeader("Authorization", QByteArray("Bearer ") + m_apiKey.toUtf8());
	req.setTransferTimeout(m_timeoutMs);
	QNetworkReply *reply = m_nam->get(req);
	connect(reply, &QNetworkReply::finished, this, &LlmPlayer::onPropsFinished);
}

void LlmPlayer::onPropsFinished()
{
	QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
	if (!reply) return;
	reply->deleteLater();
	if (reply->error() != QNetworkReply::NoError) {
		qInfo() << "[LLM] /props fetch failed:" << reply->errorString()
		        << "(context-length % will be unavailable; set POKERTH_LLM_CONTEXT to override)";
		return;
	}
	QJsonParseError pe;
	const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &pe);
	if (pe.error != QJsonParseError::NoError || !doc.isObject()) return;
	const int n = doc.object().value("default_generation_settings").toObject()
	              .value("n_ctx").toInt();
	if (n > 0) {
		m_contextSize = n;
		qInfo() << "[LLM] context window n_ctx=" << m_contextSize;
	}
}

void LlmPlayer::requestDecision(const QJsonObject &observation)
{
	m_pendingObs = observation;
	m_pendingRecap = compactRecap(observation);
	m_requestStartMs = QDateTime::currentMSecsSinceEpoch();

	QJsonArray messages;
	messages.append(QJsonObject{{"role", "system"}, {"content", buildSystemPrompt()}});

	// Reasoning hydration: replay recent turns as a real conversation so the model
	// sees its own prior chain-of-thought (its reasoning is carried in the assistant
	// content). Disabled when POKERTH_LLM_REASONING_TURNS == 0.
	for (int i = 0; i < m_priorRecaps.size(); ++i) {
		messages.append(QJsonObject{{"role", "user"},      {"content", m_priorRecaps[i]}});
		messages.append(QJsonObject{{"role", "assistant"}, {"content", m_priorAssistant[i]}});
	}

	messages.append(QJsonObject{{"role", "user"},   {"content", buildUserPrompt(observation)}});

	QJsonObject body;
	body["model"]       = m_model;
	body["messages"]    = messages;
	body["temperature"] = m_temperature;
	body["max_tokens"]  = m_maxTokens;
	// Ask OpenAI-compatible servers for strict JSON. Can be disabled for servers
	// that reject the field; we also defensively extract JSON from prose.
	if (m_jsonMode)
		body["response_format"] = QJsonObject{{"type", "json_object"}};

	QNetworkRequest req{QUrl(m_endpoint)};
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	if (!m_apiKey.isEmpty())
		req.setRawHeader("Authorization", QByteArray("Bearer ") + m_apiKey.toUtf8());
	req.setTransferTimeout(m_timeoutMs);

	QNetworkReply *reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(reply, &QNetworkReply::finished, this, &LlmPlayer::onReplyFinished);
}

void LlmPlayer::resetConversation()
{
	m_priorRecaps.clear();
	m_priorAssistant.clear();
	m_priorTokensEst.clear();
}

void LlmPlayer::onHandStart()
{
	// "hand" scope: the reasoning chain is retained within a hand, then cleared.
	if (m_reasoningScope == ScopeHand)
		resetConversation();
}

void LlmPlayer::refreshContextSize()
{
	// Re-read the server's n_ctx so the context badge can't go stale if you change
	// it server-side between games. Skipped if pinned via POKERTH_LLM_CONTEXT.
	if (m_enabled && !m_contextPinned)
		fetchContextSize();
}

QString LlmPlayer::compactRecap(const QJsonObject &obs) const
{
	const QJsonObject hero = obs.value("hero").toObject();
	QStringList hole, board;
	for (const auto &c : hero.value("hole_cards").toArray()) hole << c.toString();
	for (const auto &c : obs.value("board").toArray()) board << c.toString();
	return QStringLiteral("[%1] hole %2 board %3 pot %4 to_call %5 — your move?")
	       .arg(obs.value("round").toString(),
	            hole.isEmpty() ? QStringLiteral("?") : hole.join(' '),
	            board.isEmpty() ? QStringLiteral("-") : board.join(' '))
	       .arg(obs.value("pot").toInt())
	       .arg(obs.value("to_call").toInt());
}

void LlmPlayer::onReplyFinished()
{
	QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
	if (!reply) return;
	reply->deleteLater();

	const qint64 latency = QDateTime::currentMSecsSinceEpoch() - m_requestStartMs;
	const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

	QString action;
	int amount = 0;
	QString status;
	QString content;
	QString reasoning;
	QString reasoningContent;   // full chain-of-thought (separate field), for hydration
	QJsonObject usage;

	if (reply->error() != QNetworkReply::NoError) {
		status = QStringLiteral("http_error: ") + reply->errorString();
		fallback(m_pendingObs, action, amount);
	} else {
		const QByteArray data = reply->readAll();
		QJsonParseError pe;
		const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
		if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
			status = QStringLiteral("bad_response_json");
			fallback(m_pendingObs, action, amount);
		} else {
			usage = doc.object().value("usage").toObject();
			const QJsonObject msg = doc.object()
			        .value("choices").toArray().at(0).toObject()
			        .value("message").toObject();
			content = msg.value("content").toString();
			reasoningContent = msg.value("reasoning_content").toString();
			if (content.trimmed().isEmpty()) {
				status = QStringLiteral("empty_content");
				fallback(m_pendingObs, action, amount);
			} else {
				decideFromText(m_pendingObs, content, action, amount, reasoning, status);
			}
		}
	}

	// Reasoning hydration: remember this turn so future requests can replay it as a
	// conversation. The assistant message carries the chain-of-thought inline (in
	// <think> tags) so it survives regardless of the server's history-stripping.
	if (hydrationEnabled() && !content.trimmed().isEmpty()) {
		QString assistantMsg;
		if (!reasoningContent.trimmed().isEmpty())
			assistantMsg = QStringLiteral("<think>\n%1\n</think>\n\n%2").arg(reasoningContent, content);
		else
			assistantMsg = content;
		m_priorRecaps.append(m_pendingRecap);
		m_priorAssistant.append(assistantMsg);
		m_priorTokensEst.append((m_pendingRecap.size() + assistantMsg.size()) / 4);

		auto dropOldest = [&]() {
			m_priorRecaps.removeFirst();
			m_priorAssistant.removeFirst();
			m_priorTokensEst.removeFirst();
		};
		// Rolling-window cap for "turns" scope.
		if (m_reasoningScope == ScopeTurns && m_reasoningTurns > 0)
			while (m_priorRecaps.size() > m_reasoningTurns) dropOldest();
		// Token-budget safety (all scopes): keep the replayed history under ~half the
		// context window so a long "game" scope can't overflow n_ctx.
		if (m_contextSize > 0) {
			int sum = 0;
			for (int t : m_priorTokensEst) sum += t;
			const int budget = m_contextSize / 2;
			while (m_priorRecaps.size() > 1 && sum > budget) { sum -= m_priorTokensEst.first(); dropOldest(); }
		} else {
			while (m_priorRecaps.size() > 64) dropOldest();   // unknown ctx: hard fallback
		}
	}

	const int promptTokens     = usage.value("prompt_tokens").toInt();
	const int completionTokens = usage.value("completion_tokens").toInt();
	const int totalTokens      = usage.value("total_tokens").toInt();
	if (promptTokens > 0) {
		m_lastPromptTokens = promptTokens;
		if (promptTokens > m_peakPromptTokens) m_peakPromptTokens = promptTokens;
	}

	logDecision(m_pendingObs, content, action, amount, reasoning, status, latency, httpStatus, usage);
	qInfo() << "[LLM] decision=" << action << "amount=" << amount
	        << "status=" << status << "latency_ms=" << latency
	        << "prompt_tokens=" << promptTokens << "/" << m_contextSize
	        << "peak=" << m_peakPromptTokens;
	emit decisionReady(action, amount, reasoning);
	emit contextUsage(promptTokens, completionTokens, totalTokens, m_contextSize);
	emit thinkingReady(reasoningContent);
}

void LlmPlayer::decideFromText(const QJsonObject &obs, const QString &content,
                               QString &outAction, int &outAmount, QString &outReasoning,
                               QString &status) const
{
	// Reasoning models emit a chain-of-thought block before the answer. Strip it
	// (everything up to the last </think>) so its braces don't confuse the JSON
	// extraction; the answer JSON follows. Harmless if no such block is present.
	QString body = content;
	const int thinkEnd = body.lastIndexOf(QStringLiteral("</think>"), -1, Qt::CaseInsensitive);
	if (thinkEnd >= 0)
		body = body.mid(thinkEnd + 8);

	// Models occasionally wrap the JSON in markdown fences or prose; grab the
	// outermost {...} block of what remains.
	const int b = body.indexOf('{');
	const int e = body.lastIndexOf('}');
	QJsonObject d;
	if (b >= 0 && e > b) {
		QJsonParseError pe;
		const QJsonDocument doc = QJsonDocument::fromJson(body.mid(b, e - b + 1).toUtf8(), &pe);
		if (pe.error == QJsonParseError::NoError && doc.isObject())
			d = doc.object();
	}
	if (d.isEmpty()) {
		status = QStringLiteral("unparseable_decision");
		fallback(obs, outAction, outAmount);
		return;
	}

	outReasoning = d.value("reasoning").toString().trimmed();

	const QString a = d.value("action").toString().trimmed().toLower();
	int amt = 0;
	if (d.value("amount").isDouble())
		amt = static_cast<int>(d.value("amount").toDouble());
	else
		amt = d.value("amount").toString().toInt();

	const int toCall    = obs.value("to_call").toInt();
	const int minRaise  = obs.value("min_raise").toInt();
	const int maxRaise  = obs.value("max_raise").toInt();
	const bool canCheck = obs.value("can_check").toBool();
	const bool canRaise = obs.value("can_raise").toBool();

	status = QStringLiteral("ok");

	if (a == "fold") {
		outAction = QStringLiteral("fold");
		outAmount = 0;
	} else if (a == "check") {
		if (canCheck) {
			outAction = QStringLiteral("check");
		} else {
			outAction = QStringLiteral("call");
			status = QStringLiteral("coerced_check_to_call");
		}
		outAmount = 0;
	} else if (a == "call") {
		outAction = (toCall == 0) ? QStringLiteral("check") : QStringLiteral("call");
		outAmount = 0;
	} else if (a == "bet" || a == "raise") {
		if (!canRaise) {
			outAction = (toCall == 0) ? QStringLiteral("check") : QStringLiteral("call");
			outAmount = 0;
			status = QStringLiteral("raise_not_allowed_degraded");
		} else {
			int clamped = amt;
			if (clamped < minRaise) { clamped = minRaise; status = QStringLiteral("raise_clamped_min"); }
			if (clamped > maxRaise) { clamped = maxRaise; status = QStringLiteral("raise_clamped_max"); }
			outAction = (clamped >= maxRaise) ? QStringLiteral("allin") : QStringLiteral("raise");
			outAmount = clamped;
		}
	} else if (a == "allin" || a == "all-in" || a == "all_in") {
		if (canRaise) {
			outAction = QStringLiteral("allin");
			outAmount = maxRaise;
		} else {
			outAction = (toCall == 0) ? QStringLiteral("check") : QStringLiteral("call");
			outAmount = 0;
			status = QStringLiteral("allin_degraded");
		}
	} else {
		status = QStringLiteral("unknown_action:") + a;
		fallback(obs, outAction, outAmount);
	}
}

void LlmPlayer::fallback(const QJsonObject &obs, QString &outAction, int &outAmount) const
{
	const bool canCheck = obs.value("can_check").toBool() || obs.value("to_call").toInt() == 0;
	outAction = canCheck ? QStringLiteral("check") : QStringLiteral("fold");
	outAmount = 0;
}

QString LlmPlayer::buildSystemPrompt() const
{
	return QStringLiteral(
		"You are an expert No-Limit Texas Hold'em poker player. You are playing a "
		"tournament autonomously. On each turn you are given the full table state "
		"and the exact list of legal actions, and you must choose one.\n\n"
		"All chip amounts are integers. \"to_call\" is how many chips you must add "
		"to call. For a bet or raise, \"amount\" is the TOTAL additional chips you "
		"put in this turn (it must be between \"min_raise\" and \"max_raise\", where "
		"\"max_raise\" means all-in).\n\n"
		"You are also given the betting action so far this hand, your current hand "
		"odds (probability of finishing with each category), summaries of recent "
		"finished hands this session (including ones you folded), and per-opponent "
		"tendencies. Use them to read opponents, learn from outcomes, and inform "
		"your decision.\n\n"
		"Respond with ONLY a single JSON object, no prose, no markdown, in exactly "
		"this form:\n"
		"{\"action\": \"<fold|check|call|bet|raise|allin>\", \"amount\": <int>, "
		"\"reasoning\": \"<short>\"}\n"
		"Use \"amount\": 0 for fold/check/call/allin. Only choose an action that is "
		"present in \"legal_actions\".");
}

QString LlmPlayer::buildUserPrompt(const QJsonObject &obs) const
{
	// A readable summary followed by the raw JSON. The summary helps weaker models;
	// the JSON guarantees nothing is lost.
	const auto joinCards = [](const QJsonArray &arr) {
		QStringList s;
		for (const auto &c : arr) s << c.toString();
		return s.join(' ');
	};

	const QJsonObject hero = obs.value("hero").toObject();
	const QString holeStr = joinCards(hero.value("hole_cards").toArray());
	const QString boardStr = joinCards(obs.value("board").toArray());

	QStringList lines;
	lines << QStringLiteral("Betting round: %1").arg(obs.value("round").toString());
	lines << QStringLiteral("Your hole cards: %1").arg(holeStr.isEmpty() ? QStringLiteral("(hidden)") : holeStr);
	lines << QStringLiteral("Board: %1").arg(boardStr.isEmpty() ? QStringLiteral("(none yet)") : boardStr);
	lines << QStringLiteral("Pot: %1").arg(obs.value("pot").toInt());
	lines << QStringLiteral("Your stack: %1   Your position: %2")
	         .arg(hero.value("stack").toInt()).arg(hero.value("position").toString());
	lines << QStringLiteral("To call: %1   Min raise: %2   Max raise (all-in): %3")
	         .arg(obs.value("to_call").toInt())
	         .arg(obs.value("min_raise").toInt())
	         .arg(obs.value("max_raise").toInt());
	{
		QStringList opp;
		for (const auto &pv : obs.value("players").toArray()) {
			const QJsonObject p = pv.toObject();
			if (p.value("is_hero").toBool()) continue;
			if (p.value("name").toString().isEmpty()) continue;
			opp << QStringLiteral("  seat %1 %2 [%3]: stack %4, bet %5, last %6%7")
			       .arg(p.value("seat").toInt())
			       .arg(p.value("name").toString())
			       .arg(p.value("position").toString())
			       .arg(p.value("stack").toInt())
			       .arg(p.value("bet_this_round").toInt())
			       .arg(p.value("last_action").toString())
			       .arg(p.value("in_hand").toBool() ? QString() : QStringLiteral(" (folded/out)"));
		}
		if (!opp.isEmpty()) {
			lines << QStringLiteral("Opponents:");
			lines << opp.join('\n');
		}
	}

	// Betting history of the current hand.
	{
		const QJsonArray hist = obs.value("hand_history").toArray();
		if (!hist.isEmpty()) {
			lines << QStringLiteral("Action this hand:");
			QString curStreet;
			for (const auto &av : hist) {
				const QJsonObject a = av.toObject();
				const QString street = a.value("street").toString();
				if (street != curStreet) {
					lines << QStringLiteral("  [%1]").arg(street);
					curStreet = street;
				}
				QString l = QStringLiteral("    %1 %2").arg(a.value("name").toString(),
				                                            a.value("action").toString());
				if (a.contains("amount"))
					l += QStringLiteral(" %1").arg(a.value("amount").toInt());
				lines << l;
			}
		}
	}

	// Your current hand odds: chance of finishing with each category by the river,
	// given only the visible cards (a fair, no-leak equity read).
	{
		const QJsonArray odds = obs.value("hand_odds").toArray();
		QStringList parts;
		for (const auto &ov : odds) {
			const QJsonObject o = ov.toObject();
			if (!o.value("possible").toBool()) continue;       // skip impossible categories
			const int pct = o.value("pct").toInt();
			if (pct <= 0) continue;
			parts << QStringLiteral("%1 %2%").arg(o.value("label").toString()).arg(pct);
		}
		if (!parts.isEmpty())
			lines << QStringLiteral("Your hand odds (by river): ") + parts.join(", ");
	}

	// Recent finished hands (incl. ones you folded), so you can read opponents.
	{
		const QJsonArray recent = obs.value("recent_hands").toArray();
		if (!recent.isEmpty()) {
			lines << QStringLiteral("Recent hands:");
			for (const auto &hv : recent) {
				const QJsonObject h = hv.toObject();
				const QString board = joinCards(h.value("board").toArray());
				QStringList winners;
				for (const auto &w : h.value("winners").toArray()) winners << w.toString();
				QStringList shown;
				for (const auto &sv : h.value("shown").toArray()) {
					const QJsonObject s = sv.toObject();
					shown << QStringLiteral("%1 %2").arg(s.value("name").toString(),
					                                     joinCards(s.value("cards").toArray()));
				}
				QString l = QStringLiteral("  #%1 board[%2] pot %3 won by %4")
				            .arg(h.value("hand_id").toInt())
				            .arg(board.isEmpty() ? QStringLiteral("preflop") : board)
				            .arg(h.value("pot").toInt())
				            .arg(winners.isEmpty() ? QStringLiteral("?") : winners.join(", "));
				if (!shown.isEmpty())
					l += QStringLiteral("; showdown: ") + shown.join(", ");
				lines << l;

				// How the hand was played, by street (the betting narrative).
				const QJsonArray acts = h.value("actions").toArray();
				if (!acts.isEmpty()) {
					QStringList segs, streetActs;
					QString cur;
					for (const auto &av : acts) {
						const QJsonObject a = av.toObject();
						const QString st = a.value("street").toString();
						if (st != cur) {
							if (!streetActs.isEmpty())
								segs << QStringLiteral("%1: %2").arg(cur, streetActs.join(", "));
							streetActs.clear();
							cur = st;
						}
						QString s = QStringLiteral("%1 %2").arg(a.value("name").toString(),
						                                        a.value("action").toString());
						if (a.contains("amount")) s += QStringLiteral(" %1").arg(a.value("amount").toInt());
						streetActs << s;
					}
					if (!streetActs.isEmpty())
						segs << QStringLiteral("%1: %2").arg(cur, streetActs.join(", "));
					if (!segs.isEmpty())
						lines << QStringLiteral("      ") + segs.join(QStringLiteral(" | "));
				}
			}
		}
	}

	// Opponent tendencies distilled from observed actions.
	{
		const QJsonArray stats = obs.value("opponent_stats").toArray();
		if (!stats.isEmpty()) {
			lines << QStringLiteral("Opponent reads (from observed actions):");
			for (const auto &sv : stats) {
				const QJsonObject s = sv.toObject();
				lines << QStringLiteral("  %1: %2 actions seen, %3% aggressive, %4% fold")
				         .arg(s.value("name").toString())
				         .arg(s.value("actions_seen").toInt())
				         .arg(s.value("aggressive_pct").toInt())
				         .arg(s.value("fold_pct").toInt());
			}
		}
	}

	{
		QStringList legal;
		for (const auto &c : obs.value("legal_actions").toArray()) legal << c.toString();
		lines << QStringLiteral("Legal actions: %1").arg(legal.join(", "));
	}
	lines << QString();
	// Dump the core state as JSON as a "nothing lost" backup, but drop the verbose
	// context arrays already rendered above (kept readable-only to save tokens).
	QJsonObject core = obs;
	core.remove(QStringLiteral("hand_history"));
	core.remove(QStringLiteral("recent_hands"));
	core.remove(QStringLiteral("opponent_stats"));
	core.remove(QStringLiteral("hand_odds"));
	lines << QStringLiteral("Core state JSON:");
	lines << QString::fromUtf8(QJsonDocument(core).toJson(QJsonDocument::Compact));
	lines << QString();
	lines << QStringLiteral("Respond with only the JSON decision object.");
	return lines.join('\n');
}

void LlmPlayer::logDecision(const QJsonObject &obs, const QString &rawContent,
                            const QString &action, int amount, const QString &reasoning,
                            const QString &status, qint64 latencyMs, int httpStatus,
                            const QJsonObject &usage) const
{
	if (m_logPath.isEmpty()) return;

	QJsonObject rec;
	rec["ts"]          = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
	rec["model"]       = m_model;
	rec["observation"] = obs;
	rec["raw"]         = rawContent;
	rec["action"]      = action;
	rec["amount"]      = amount;
	rec["reasoning"]   = reasoning;
	rec["status"]      = status;
	rec["latency_ms"]  = static_cast<double>(latencyMs);
	rec["http_status"] = httpStatus;
	// Token usage for context-length tracking (llama.cpp returns this).
	if (!usage.isEmpty()) rec["usage"] = usage;
	rec["context_size"] = m_contextSize;
	rec["reasoning_turns"] = m_reasoningTurns;   // hydration depth, for A/B runs

	QFile f(m_logPath);
	if (f.open(QIODevice::Append | QIODevice::Text)) {
		f.write(QJsonDocument(rec).toJson(QJsonDocument::Compact));
		f.write("\n");
		f.close();
	} else {
		qWarning() << "[LLM] could not append to log" << m_logPath;
	}
}
