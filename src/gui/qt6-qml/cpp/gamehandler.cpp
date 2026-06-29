/*****************************************************************************
 * PokerTH - The open source texas holdem engine                             *
 * Copyright (C) 2006-2025 Felix Hammer, Florian Thauer, Lothar May          *
 *****************************************************************************/

#include "gamehandler.h"
#include "chatemotes.h"
#include "llmplayer.h"
#include <session.h>
#include <game.h>
#include <handinterface.h>
#include <playerinterface.h>
#include <boardinterface.h>
#include <berointerface.h>
#include <cardsvalue.h>
#include <playerdata.h>
#include <game_defs.h>
#include <gamedata.h>
#include <configfile.h>
#include <soundevents.h>
#include <QString>
#include <QTimer>
#include <QDebug>
#include <QUrl>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QEvent>
#include <QJsonObject>
#include <QJsonArray>
#include <QLocale>
#include <algorithm>
#include <list>

namespace {
// Karten-Code (0-51) → Kurzform mit Unicode-Farbsymbol, z. B. "K♥". Identisch zu
// QmlGuiInterface::fmtCard, damit Showdown-Karten genau wie die Board-Karten im
// Spielverlauf aussehen.  0-12 Karo(♦), 13-25 Herz(♥), 26-38 Pik(♠), 39-51 Kreuz(♣).
QString logCard(int code)
{
    if (code < 0 || code > 51)
        return QStringLiteral("?");
    static const char *ranks[] = {"2","3","4","5","6","7","8","9","10","J","Q","K","A"};
    static const QChar suits[] = { QChar(0x2666), QChar(0x2665), QChar(0x2660), QChar(0x2663) };
    return QString::fromLatin1(ranks[code % 13]) + QString(suits[code / 13]);
}

// Karten-Code (0-51) → ASCII-Kurzform für den LLM, z. B. "Ah", "Td", "2c".
// Rang = code%13 (2..A, T=Zehn), Farbe = code/13 (0=d 1=h 2=s 3=c, wie logCard).
QString cardToStr(int code)
{
    if (code < 0 || code > 51)
        return QStringLiteral("??");
    static const char *ranks[] = {"2","3","4","5","6","7","8","9","T","J","Q","K","A"};
    static const char suits[] = {'d', 'h', 's', 'c'};
    return QString::fromLatin1(ranks[code % 13]) + QChar(suits[code / 13]);
}

// getMyButton(): 0=keiner, 1=Dealer, 2=Small Blind, 3=Big Blind.
QString buttonName(int button)
{
    switch (button) {
    case 1: return QStringLiteral("dealer");
    case 2: return QStringLiteral("small_blind");
    case 3: return QStringLiteral("big_blind");
    default: return QStringLiteral("none");
    }
}

QString actionName(int action)
{
    switch (action) {
    case PLAYER_ACTION_FOLD:  return QStringLiteral("fold");
    case PLAYER_ACTION_CHECK: return QStringLiteral("check");
    case PLAYER_ACTION_CALL:  return QStringLiteral("call");
    case PLAYER_ACTION_BET:   return QStringLiteral("bet");
    case PLAYER_ACTION_RAISE: return QStringLiteral("raise");
    case PLAYER_ACTION_ALLIN: return QStringLiteral("allin");
    default:                  return QStringLiteral("none");
    }
}

QString roundName(int round)
{
    static const char *names[] = {"preflop", "flop", "turn", "river", "post_river"};
    return (round >= 0 && round <= 4) ? QString::fromLatin1(names[round]) : QStringLiteral("unknown");
}

// Spielverlauf-Zeile als HTML einfärben – Farben/Stil 1:1 wie der Qt-Widgets-
// Client (Default-Tischstil): normal #F0F0F0, Gewinner Hauptpot #FFFF00, Side-Pot
// #FFFFCC, Sit-out/Board #FF6633.
QString formatLogLine(const QString &text, int type)
{
    const QString esc = text.toHtmlEscaped();
    switch (type) {
    case GameHandler::LogHeader:
        return QStringLiteral("<span style=\"color:#F0F0F0; font-weight:bold;\">") + esc + QStringLiteral("</span>");
    case GameHandler::LogWinnerMain:
        return QStringLiteral("<span style=\"color:#FFFF00;\">") + esc + QStringLiteral("</span>");
    case GameHandler::LogWinnerSide:
        return QStringLiteral("<span style=\"color:#FFFFCC;\">") + esc + QStringLiteral("</span>");
    case GameHandler::LogSitOut:
        return QStringLiteral("<i><span style=\"color:#FF6633;\">") + esc + QStringLiteral("</span></i>");
    case GameHandler::LogBoard:
        return QStringLiteral("<span style=\"color:#FF6633;\">") + esc + QStringLiteral("</span>");
    case GameHandler::LogGameWin:
        return QStringLiteral("<b><i><span style=\"color:#F0F0F0;\">") + esc + QStringLiteral("</span></i></b>");
    default:
        return QStringLiteral("<span style=\"color:#F0F0F0;\">") + esc + QStringLiteral("</span>");
    }
}

// Avatar-Pfad → QML-Bildquelle. getMyAvatar() liefert (wie im Widgets-Client)
// einen lokalen Dateipfad; existiert die Datei, als file://-URL zurückgeben.
QString resolveAvatarSource(const std::string &raw)
{
    if (raw.empty())
        return QString();
    const QString path = QString::fromStdString(raw);
    if (!QFileInfo::exists(path))
        return QString();
    return QUrl::fromLocalFile(path).toString();
}

// ASCII-Smileys → Unicode-Emoji (identisch zum Lobby-Chat). Eingabe ist bereits
// HTML-escaped ('>' = "&gt;"); RichText rendert die Emoji über die Systemschrift.
QString chatCheckForEmotes(const QString &input)
{
    QString result = input;
    auto emo = [](char32_t cp) -> QString { return QString::fromUcs4(&cp, 1); };

    result.replace(QLatin1String("0:-)"),    emo(0x1F607)); // 😇
    result.replace(QLatin1String("X-("),     emo(0x1F620)); // 😠
    result.replace(QLatin1String("B-)"),     emo(0x1F60E)); // 😎
    result.replace(QLatin1String("8-)"),     emo(0x1F60E)); // 😎
    result.replace(QLatin1String(":'("),     emo(0x1F622)); // 😢
    result.replace(QLatin1String("&gt;:-)"), emo(0x1F608)); // 😈
    result.replace(QLatin1String(":-["),     emo(0x1F633)); // 😳
    result.replace(QLatin1String(":-*"),     emo(0x1F617)); // 😗
    result.replace(QLatin1String(":-))" ),   emo(0x1F602)); // 😂
    result.replace(QLatin1String(":))" ),    emo(0x1F602)); // 😂
    result.replace(QLatin1String(":-|"),     emo(0x1F610)); // 😐
    result.replace(QLatin1String(":-P"),     emo(0x1F61B)); // 😛
    result.replace(QLatin1String(":-p"),     emo(0x1F61B)); // 😛
    result.replace(QLatin1String(":-("),     emo(0x1F61E)); // 😞
    result.replace(QLatin1String(":("),      emo(0x1F61E)); // 😞
    result.replace(QLatin1String(":-&"),     emo(0x1F912)); // 🤒
    result.replace(QLatin1String(":-D"),     emo(0x1F603)); // 😃
    result.replace(QLatin1String(":D"),      emo(0x1F603)); // 😃
    result.replace(QLatin1String(":-!"),     emo(0x1F60F)); // 😏
    result.replace(QLatin1String(":-0"),     emo(0x1F62E)); // 😮
    result.replace(QLatin1String(":-O"),     emo(0x1F62E)); // 😮
    result.replace(QLatin1String(":-o"),     emo(0x1F62E)); // 😮
    result.replace(QLatin1String(":-/"),     emo(0x1F615)); // 😕
    if (!result.contains(QLatin1String("http://")) && !result.contains(QLatin1String("https://")))
        result.replace(QLatin1String(":/"), emo(0x1F615));  // 😕
    result.replace(QLatin1String(";-)"),     emo(0x1F609)); // 😉
    result.replace(QLatin1String(";)"),      emo(0x1F609)); // 😉
    result.replace(QLatin1String(":-S"),     emo(0x1F61F)); // 😟
    result.replace(QLatin1String(":-s"),     emo(0x1F61F)); // 😟
    result.replace(QLatin1String(":-)"),     emo(0x1F60A)); // 😊
    result.replace(QLatin1String(":)"),      emo(0x1F60A)); // 😊
    return enlargeEmojis(result);
}
} // namespace

GameHandler::GameHandler(QObject *parent)
    : QObject(parent), m_phaseText("Preflop")
{
    // Initialize empty player list (10 seats)
    for (int i = 0; i < 10; ++i) {
        QVariantMap p;
        p["name"]    = QString("");
        p["stack"]   = 0;
        p["bet"]     = 0;
        p["active"]  = false;
        p["myTurn"]  = false;
        p["seatId"]  = i;
        p["button"]  = 0;
        p["action"]  = 0;
        p["card0"]   = -1;
        p["card1"]   = -1;
        m_players.append(p);
    }
    // Initialize empty board cards (5 slots, -1 = not dealt)
    for (int i = 0; i < 5; ++i)
        m_boardCards.append(-1);

    m_timeoutBeepTimer = new QTimer(this);
    m_timeoutBeepTimer->setSingleShot(true);
    connect(m_timeoutBeepTimer, &QTimer::timeout, this, [this]() {
        playYourTurnTimeoutSound();
    });

    // AFK-Reset: echte Nutzeraktivität (Maus/Tastatur) hält den serverseitigen
    // Inaktivitäts-Timeout zurück. WICHTIG: Spielaktionen (fold/call/raise)
    // zählen serverseitig NICHT als Aktivität (Type_MyActionRequestMessage ist
    // von IsClientActivity ausgenommen, damit Auto-Check/Fold den AFK-Timeout
    // nicht aushebelt) – nur eine Type_ResetTimeoutMessage setzt den In-Game-
    // Timer (21 min) zurück. Ohne dies wurde der QML-Client trotz aktiven
    // Spielens nach ~21 min vom Server gekickt (wie der Widgets-Client per
    // eventFilter). App-weiter Filter, ratenbegrenzt.
    m_afkResetTimer.start();
    if (qApp)
        qApp->installEventFilter(this);

    // LLM eval harness: autonomous player for the hero seat (configured via env;
    // a no-op unless POKERTH_LLM_ENABLE=1 and an endpoint are set).
    m_llm = new LlmPlayer(this);
    connect(m_llm, &LlmPlayer::decisionReady, this, &GameHandler::onLlmDecision);
    connect(m_llm, &LlmPlayer::contextUsage, this, &GameHandler::onContextUsage);
    connect(m_llm, &LlmPlayer::thinkingReady, this, &GameHandler::onThinking);

    // Per-session finished-hand memory depth (POKERTH_LLM_HISTORY_HANDS).
    const int hh = qEnvironmentVariableIntValue("POKERTH_LLM_HISTORY_HANDS");
    if (hh > 0) m_llmMaxRecentHands = hh;
}

bool GameHandler::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type t = event->type();
    if (t == QEvent::MouseButtonPress || t == QEvent::KeyPress) {
        if (m_session && m_session->isNetworkClientRunning()
            && m_afkResetTimer.elapsed() >= kAfkResetIntervalMs) {
            m_session->resetNetworkTimeout();
            m_afkResetTimer.restart();
        }
    }
    return QObject::eventFilter(watched, event);
}

GameHandler::~GameHandler()
{
    delete m_soundEventHandler;
    m_soundEventHandler = nullptr;
}

void GameHandler::setConfig(ConfigFile *config)
{
    m_config = config;
    ensureSoundEventHandler();
}

void GameHandler::setSession(boost::shared_ptr<Session> session)
{
    m_session = session;
}

void GameHandler::setGame(boost::shared_ptr<Game> game)
{
    ensureSoundEventHandler();
    if (m_soundEventHandler)
        m_soundEventHandler->newGameStarts();

    m_localGameExitRequested = false;
    m_game = game;
    m_leftPlayers.clear();
    // Ausstehende Busted-Player-Timer aus dem vorigen Spiel verwerfen.
    qDeleteAll(m_bustedLocalTimers);
    m_bustedLocalTimers.clear();
    // Reset state for new game
    m_pot = 0;
    m_gameId = m_game ? m_game->getMyGameID() : 0;
    m_phaseText = "Preflop";
    m_handNumber = 0;
    m_myTurn = false;
    m_callAmount = 0;
    m_minRaiseAmount = 0;
    m_maxRaiseAmount = 0;
    m_boardCardCount = 0;
    m_boardCards = QVariantList{-1, -1, -1, -1, -1};
    m_winnerSeatIds.clear();
    m_winningHandText.clear();
    m_showdownActive = false;
    m_gameLog.clear();
    emit gameLogChanged();
    m_chatLog.clear();
    emit chatLogChanged();
    for (int i = 0; i < 10; ++i) {
        m_lastSeenAction[i] = 0;
        m_actionToken[i] = -1;
    }

    // Re-build player list (seats may differ between games)
    refreshPlayerData();

    emit potChanged();
    emit gameIdChanged();
    emit phaseTextChanged();
    emit handNumberChanged();
    emit myTurnChanged();
    emit callAmountChanged();
    emit minRaiseAmountChanged();
    emit maxRaiseAmountChanged();
    emit boardCardCountChanged();
    emit boardCardsChanged();
    emit winnerSeatIdsChanged();
    emit winningHandTextChanged();
}

// ─── private helpers ────────────────────────────────────────────────────────

void GameHandler::ensureSoundEventHandler()
{
    if (!m_soundEventHandler && m_config)
        m_soundEventHandler = new SoundEvents(m_config);
}

void GameHandler::playYourTurnTimeoutSound()
{
    ensureSoundEventHandler();
    if (m_soundEventHandler)
        m_soundEventHandler->playSound("yourturn", 0);
}

void GameHandler::appendGameLog(const QString &message, int type)
{
    if (message.isEmpty()) return;
    m_gameLog.append(formatLogLine(message, type));
    // Begrenzen, damit der Verlauf nicht unbegrenzt wächst.
    const int kMaxLines = 400;
    if (m_gameLog.size() > kMaxLines)
        m_gameLog.erase(m_gameLog.begin(), m_gameLog.begin() + (m_gameLog.size() - kMaxLines));
    emit gameLogChanged();
}

void GameHandler::appendChat(const QString &playerName, const QString &message)
{
    if (message.isEmpty()) return;

    // Emoji-Reaktionen (Konvention des Web-Clients): "/emoji 🎉" bzw. legacy
    // "[R]🎉". Nicht in den Chat-Verlauf aufnehmen, sondern als Reaktions-
    // Animation am Sitz des Absenders abspielen. Auf dem getrimmten Text
    // prüfen (Clients können Whitespace anhängen); Längen-Limit großzügiger
    // als im Web-Client (22 statt 18), damit auch ZWJ-/Variation-Selector-
    // Sequenzen durchgehen – normale Nachrichten matchen trotzdem nicht.
    const QString trimmedMsg = message.trimmed();
    QString reactionEmoji;
    if (trimmedMsg.startsWith(QStringLiteral("/emoji ")) && trimmedMsg.size() < 22)
        reactionEmoji = trimmedMsg.mid(7).trimmed();
    else if (trimmedMsg.startsWith(QStringLiteral("[R]")) && trimmedMsg.size() < 14)
        reactionEmoji = trimmedMsg.mid(3).trimmed();
    if (!reactionEmoji.isEmpty()) {
        qDebug() << "[REACT] incoming reaction from" << playerName << ":" << reactionEmoji;
        emit reactionReceived(playerName, reactionEmoji);
        return;
    }

    // Formatierung analog zum Lobby-Chat: /me-Aktion, Emojis, Erwähnung.
    const QString myNick = m_config ? QString::fromStdString(m_config->readConfigString("MyName")) : QString();
    const bool isAction = message.startsWith(QStringLiteral("/me "));
    const QString rawDisplay = isAction ? message.mid(4) : message;

    QString escapedMsg = rawDisplay.toHtmlEscaped();
    static const QRegularExpression urlRe(QStringLiteral("(https?://\\S+)"));
    escapedMsg.replace(urlRe, QStringLiteral("<a href=\"\\1\">\\1</a>"));

    const bool isMention = !myNick.isEmpty() && rawDisplay.contains(myNick, Qt::CaseInsensitive);
    const QString color = isMention ? QStringLiteral("#E3C800") : QStringLiteral("#e6e6e6");
    QString styledMsg = QStringLiteral("<span style=\"color:") + color
                        + (isMention ? QStringLiteral("; font-weight:bold") : QString())
                        + QStringLiteral(";\">") + escapedMsg + QStringLiteral("</span>");
    if (!m_config || !m_config->readConfigInt("DisableChatEmoticons"))
        styledMsg = chatCheckForEmotes(styledMsg);

    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    const QString name = playerName.toHtmlEscaped();
    QString line;
    if (isAction)
        line = QStringLiteral("[") + ts + QStringLiteral("] <i>* ") + name + QStringLiteral(" ") + styledMsg + QStringLiteral(" *</i>");
    else
        line = QStringLiteral("[") + ts + QStringLiteral("] <b>") + name + QStringLiteral(":</b> ") + styledMsg;

    m_chatLog.append(line);
    const int kMaxLines = 400;
    if (m_chatLog.size() > kMaxLines)
        m_chatLog.erase(m_chatLog.begin(), m_chatLog.begin() + (m_chatLog.size() - kMaxLines));
    emit chatLogChanged();
}

void GameHandler::sendChat(const QString &message)
{
    if (!m_session || message.trimmed().isEmpty()) return;
    // Auf 128 Bytes UTF-8 begrenzen (wie der Lobby-Chat).
    QString text = message;
    while (!text.isEmpty() && text.toUtf8().size() > 128)
        text.chop(1);
    if (text.isEmpty()) return;
    m_session->sendGameChatMessage(text.toStdString());
}

bool GameHandler::localGameCallbacksBlocked() const
{
    if (!m_localGameExitRequested) return false;
    if (!m_session) return true;
    return !m_session->isNetworkClientRunning();
}

void GameHandler::refreshPlayerData()
{
    // Build a fresh 10-slot list
    QVariantList newPlayers;
    for (int i = 0; i < 10; ++i) {
        QVariantMap p;
        p["name"]   = QString("");
        p["stack"]  = 0;
        p["bet"]    = 0;
        p["active"] = false;
        p["myTurn"] = false;
        p["seatId"] = i;
        p["button"] = 0;
        p["action"] = 0;
        p["card0"]  = -1;
        p["card1"]  = -1;
        newPlayers.append(p);
    }

    // Lazy-init m_game for local games: session creates the game internally
    if (!m_game && m_session && !m_localGameExitRequested) {
        auto g = m_session->getCurrentGame();
        if (g) m_game = g;
    }

    // Eindeutiges Token der aktuellen Setzrunde (Hand-Nr. × 8 + Runde). Eine Aktion
    // wird nur angezeigt, solange dieses Token unverändert ist → zu Rundenbeginn
    // (auch über Hände hinweg) verschwinden alle Aktions-Anzeigen automatisch.
    int currentToken = -1;
    if (m_game) {
        auto hand = m_game->getCurrentHand();
        if (hand) {
            currentToken = hand->getMyID() * 8 + static_cast<int>(hand->getCurrentRound());
            // Showdown gilt nur in der Post-River-Phase. In jeder aktiven Setzrunde
            // (Preflop–River) zurücksetzen, damit ein stehengebliebenes Flag (z. B.
            // wenn onNextRoundCleanGui im Netzwerkspiel nicht feuert) nicht die
            // Action-Badges der Folgehände ausblendet.
            if (hand->getCurrentRound() != GAME_STATE_POST_RIVER)
                m_showdownActive = false;
        }
    }

    // Neue Setzrunde → die letzte Aggression (bet/raise) gilt nicht mehr.
    if (currentToken != m_aggressorToken) {
        m_aggressorToken = currentToken;
        m_lastAggressorSeq = 0;
    }

    int humanCount = 0;
    if (m_game) {
        PlayerList seats = m_game->getSeatsList();

        // Vorab-Durchlauf: Aktionswechsel erfassen, jeder Aktion eine fortlaufende
        // Sequenznummer geben und die jüngste Aggression (bet/raise) der Runde
        // merken – unabhängig von der Sitzreihenfolge.
        for (auto it = seats->begin(); it != seats->end(); ++it) {
            int id = (*it)->getMyID();
            if (id < 0 || id >= 10) continue;
            int act = (*it)->getMyAction();
            int curSet = (*it)->getMySet();
            // Frische Aktion = Aktionstyp ODER Einsatz hat sich geändert. So zählt
            // auch ein erneutes Callen nach einer Erhöhung (Typ bleibt CALL, Einsatz
            // steigt) als neue Aktion → das zuvor geleerte Badge erscheint wieder.
            if (act != m_lastSeenAction[id] || curSet != m_lastSeenSet[id]) {
                m_lastSeenAction[id] = act;
                m_lastSeenSet[id] = curSet;
                m_actionToken[id] = currentToken;
                m_actionSeq[id] = ++m_actionCounter;
                // Aggression (= alle anderen müssen erneut reagieren): bet/raise
                // immer; ein All-In nur, wenn sein Einsatz ÜBER dem aktuellen
                // Höchsteinsatz der übrigen Spieler liegt (echtes Erhöhen – ein
                // All-In-Call auf/unter dem Höchsteinsatz löst nicht aus).
                bool aggressive = (act == PLAYER_ACTION_BET || act == PLAYER_ACTION_RAISE);
                if (act == PLAYER_ACTION_ALLIN) {
                    int maxOtherSet = 0;
                    for (auto jt = seats->begin(); jt != seats->end(); ++jt) {
                        if (jt == it) continue;
                        int s = (*jt)->getMySet();
                        if (s > maxOtherSet) maxOtherSet = s;
                    }
                    aggressive = (curSet > maxOtherSet);
                }
                if (aggressive)
                    m_lastAggressorSeq = m_actionSeq[id];
            }
        }

        for (auto it = seats->begin(); it != seats->end(); ++it) {
            int id = (*it)->getMyID();
            // Spieler, der das Spiel verlassen hat: Sitz als leer behandeln.
            if (m_leftPlayers.contains((*it)->getMyUniqueID()))
                continue;
            if (!(*it)->getMyName().empty() && (*it)->getMyType() == PLAYER_TYPE_HUMAN)
                ++humanCount;
            if (id >= 0 && id < 10) {
                int cards[2] = {-1, -1};
                (*it)->getMyCards(cards);
                const bool cardsKnown = cards[0] >= 0 && cards[1] >= 0;
                // Gegnerkarten nur im echten Showdown anzeigen – und nur für die
                // Spieler, die laut Engine aufdecken müssen (wie im Widgets-Client:
                // nicht gefoldet UND checkIfINeedToShowCards()). Das Showdown-Flag
                // verhindert, dass die noch veraltete playerNeedToShowCards-Liste
                // während der River-Setzrunde der nächsten Hand fälschlich aufdeckt.
                const bool showdownReveal = m_showdownActive
                                            && (*it)->getMyAction() != PLAYER_ACTION_FOLD
                                            && (*it)->checkIfINeedToShowCards();
                // All-In-Aufdeckung: Karten sind nach AllInShowCardsMessage für alle
                // nicht-gefoldeten Spieler sichtbar (bis zur nächsten Hand).
                const bool allInReveal = m_allInRevealed
                                         && (*it)->getMyAction() != PLAYER_ACTION_FOLD;
                const bool faceUp = cardsKnown && (id == 0 || showdownReveal || allInReveal);
                if (id != 0 && m_allInRevealed) {
                    qDebug() << "[ALLIN] refreshPD seat" << id
                             << "cardsKnown=" << cardsKnown
                             << "cards=" << cards[0] << "/" << cards[1]
                             << "action=" << (int)(*it)->getMyAction()
                             << "allInReveal=" << allInReveal
                             << "faceUp=" << faceUp;
                }

                // Im Showdown werden ALLE Aktions-Badges entfernt (auch All-In und
                // Fold) – jetzt zählen nur noch aufgedeckte Karten, Gewinner-Hand
                // und Sieger.
                // Sonst: All-In bleibt die ganze Hand über sichtbar (die Engine
                // behält PLAYER_ACTION_ALLIN über alle Runden bei und setzt es erst
                // zur nächsten Hand zurück). Übrige Aktionen verschwinden zu Runden-
                // beginn (Token-Logik) und sobald ein anderer Spieler bet/raise
                // gesetzt hat (Sequenz < letzte Aggression).
                int act = (*it)->getMyAction();
                const bool sameRound = (currentToken >= 0 && m_actionToken[id] == currentToken);
                int displayAction;
                if (m_showdownActive || allInReveal) {
                    // Showdown und All-In-Runout (Karten aufgedeckt): Badge entfernen
                    // damit die aufgedeckten Karten nicht verdeckt werden.
                    displayAction = 0;
                } else if (act == PLAYER_ACTION_ALLIN) {
                    displayAction = act;
                } else if (act == PLAYER_ACTION_FOLD) {
                    // "Fold" bleibt für die Runde stehen – eine spätere bet/raise
                    // eines anderen Spielers entfernt es nicht (nur nicht-gefoldete
                    // Spieler müssen erneut handeln).
                    displayAction = sameRound ? act : 0;
                } else if (sameRound && m_actionSeq[id] >= m_lastAggressorSeq) {
                    displayAction = act;
                } else {
                    displayAction = 0;
                }

                QVariantMap p;
                p["name"]   = QString::fromStdString((*it)->getMyName());
                p["stack"]  = (*it)->getMyCash();
                p["bet"]    = (*it)->getMySet();
                p["active"] = (*it)->getMyActiveStatus();
                p["myTurn"] = (*it)->getMyTurn();
                p["seatId"] = id;
                // Dealer/Small-/Big-Blind nur für aktive Spieler: ausgeschiedene
                // (0 Coins, raus) behalten sonst ihr altes BB/SB/D-Icon bis zur
                // nächsten Hand. All-In-Spieler bleiben aktiv und behalten es korrekt.
                p["button"] = (*it)->getMyActiveStatus() ? (*it)->getMyButton() : BUTTON_NONE;
                p["action"] = displayAction;
                // Gefoldete Spieler bleiben die ganze Hand über gefoldet → Karten
                // durchscheinend darstellen (wie im Qt-Widgets-Client).
                p["folded"] = ((*it)->getMyAction() == PLAYER_ACTION_FOLD);
                // Avatar (gesetzter Spieler-Avatar); Sitz 0 notfalls aus der Config.
                std::string avatarRaw = (*it)->getMyAvatar();
                if (avatarRaw.empty() && id == 0 && m_config)
                    avatarRaw = m_config->readConfigString("MyAvatar");
                p["avatar"] = resolveAvatarSource(avatarRaw);
                p["card0"]  = faceUp ? cards[0] : -1;
                p["card1"]  = faceUp ? cards[1] : -1;
                if (id == 0) {
                    // qDebug() << "[DBG] seat0 cards:" << cards[0] << cards[1]
                    //          << "faceUp:" << faceUp;
                }
                newPlayers[id] = p;
            }
        }
    }

    // Chat-Icon nur, wenn außer mir noch ein menschlicher Spieler dabei ist.
    const bool newHasHumanOpponents = humanCount > 1;
    if (newHasHumanOpponents != m_hasHumanOpponents) {
        m_hasHumanOpponents = newHasHumanOpponents;
        emit hasHumanOpponentsChanged();
    }

    m_players = newPlayers;
    emit playersChanged();

    // Lokales Spiel: Spieler mit 0 Coins nach 10 Sekunden ausblenden.
    checkBustedLocalPlayers();
}

void GameHandler::refreshPotData()
{
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (!hand) return;
    auto board = hand->getBoard();
    if (!board) return;

    int newPot = board->getPot();
    if (newPot != m_pot) {
        m_pot = newPot;
        emit potChanged();
    }

    int newTotalPot = board->getPot() + board->getSets();
    if (newTotalPot != m_totalPot) {
        m_totalPot = newTotalPot;
        emit totalPotChanged();
    }
}

void GameHandler::computeCallAndRaiseAmounts()
{
    int newCallAmount = 0;
    int newMinRaise = 0;
    int newMaxRaise = 0;
    bool newCanAct = false;
    int dbgMyAction = -1;   // [ACTDBG] zuletzt gelesene Engine-Aktion (für Log)
    int dbgPrevId   = -99;  // [ACTDBG] getPreviousPlayerID() (für Log)
    int dbgHandId   = -1;   // [ACTDBG] aktuelle Hand-ID (für Log)

    if (m_game) {
        auto hand = m_game->getCurrentHand();
        if (hand) {
            auto bero = hand->getCurrentBeRo();
            auto seats = hand->getSeatsList();
            if (bero && seats && !seats->empty()) {
                auto humanPlayer = seats->front();
                const int highestSet = bero->getHighestSet();
                const int humanSet = humanPlayer->getMySet();
                const int humanCash = humanPlayer->getMyCash();

                // Buttons aktiv (für Vor-Auswahl ODER echten Zug), wenn ich in
                // der Hand und nicht all-in bin UND entweder gerade am Zug bin
                // (m_myTurn) ODER NICHT der zuletzt handelnde Spieler war.
                //
                // Das spiegelt exakt gameTableImpl::updateMyButtonsState() der
                // Widgets-Referenz: dort sind die Buttons „checkable" (Vorwahl)
                // solange getPreviousPlayerID() != 0 (= ich war nicht der letzte
                // Akteur). getMyAction() == NONE taugt NICHT als Kriterium: nach
                // meiner Aktion (CALL/CHECK) und einer anschließenden Erhöhung
                // eines Gegners muss ich erneut handeln können – getMyAction()
                // ist dann aber bereits != NONE, sodass die Vorwahl fälschlich
                // gesperrt blieb, bis ich am Zug bin. previousPlayerID wird beim
                // Rundenwechsel/Deal auf -1 gesetzt (Vorwahl bleibt offen, kein
                // Flackern) und nach jeder Aktion auf den Akteur – nach MEINER
                // Aktion also auf 0, was die Buttons sauber abschaltet, bis ein
                // Gegner handelt.
                const int myAction = humanPlayer->getMyAction();
                const int prevPlayerId = hand->getPreviousPlayerID();
                dbgMyAction = myAction;
                dbgPrevId   = prevPlayerId;
                dbgHandId   = hand->getMyID();
                const bool baseEligible =
                            myAction != PLAYER_ACTION_FOLD
                            && myAction != PLAYER_ACTION_ALLIN
                            && humanCash > 0
                            && humanPlayer->isSessionActive();
                newCanAct = baseEligible
                            && (m_myTurn || prevPlayerId != 0)
                            && !m_showdownActive;

                if (humanCash + humanSet <= highestSet) {
                    newCallAmount = humanCash;
                } else {
                    newCallAmount = highestSet - humanSet;
                }
                if (newCallAmount < 0) {
                    newCallAmount = 0;
                }

                const bool buttonsDisabled =
                    humanPlayer->getMyAction() == PLAYER_ACTION_ALLIN ||
                    humanPlayer->getMyAction() == PLAYER_ACTION_FOLD ||
                    humanCash == 0 ||
                    (humanSet == highestSet && humanPlayer->getMyAction() != PLAYER_ACTION_NONE) ||
                    !humanPlayer->isSessionActive();

                if (!buttonsDisabled && !bero->getFullBetRule()) {
                    int minimum = 0;
                    bool canBetRaise = false;

                    if (hand->getCurrentRound() == 0) {
                        if (humanCash + humanSet > highestSet) {
                            minimum = highestSet - humanSet + bero->getMinimumRaise();
                            canBetRaise = true;
                        }
                    } else {
                        if (highestSet == 0) {
                            minimum = hand->getSmallBlind() * 2;
                            canBetRaise = true;
                        } else if (highestSet > humanSet && humanCash + humanSet > highestSet) {
                            minimum = highestSet - humanSet + bero->getMinimumRaise();
                            canBetRaise = true;
                        }
                    }

                    if (canBetRaise) {
                        if (minimum < 0) {
                            minimum = 0;
                        }
                        newMaxRaise = humanCash;
                        newMinRaise = std::min(minimum, newMaxRaise);
                    }
                }
            }
        }
    }

    // VERDACHT: myTurn=true aber Engine zeigt uns bereits als Fold/AllIn → stale Daten?
    if (m_myTurn && (dbgMyAction == PLAYER_ACTION_FOLD || dbgMyAction == PLAYER_ACTION_ALLIN)) {
        qDebug() << "[ACTDBG] SUSPECT: myTurn=true but myAction=" << dbgMyAction
                 << "(FOLD=1,ALLIN=6) handId=" << dbgHandId
                 << "prevId=" << dbgPrevId << "newCanAct=" << newCanAct;
    }
    if (newCanAct != m_canAct) {
        m_canAct = newCanAct;
        qDebug() << "[ACTDBG] canAct=" << m_canAct << "prevPlayerId=" << dbgPrevId
                 << "myAction=" << dbgMyAction
                 << "(NONE=0,FOLD=1,CHK=2,CALL=3,BET=4,RAISE=5,ALLIN=6)"
                 << "handId=" << dbgHandId
                 << "myTurn=" << m_myTurn << "tSeat=" << m_timeoutSeatId;
        emit canActChanged();
    }
    if (newCallAmount != m_callAmount) {
        m_callAmount = newCallAmount;
        emit callAmountChanged();
    }
    if (newMinRaise != m_minRaiseAmount) {
        m_minRaiseAmount = newMinRaise;
        emit minRaiseAmountChanged();
    }
    if (newMaxRaise != m_maxRaiseAmount) {
        m_maxRaiseAmount = newMaxRaise;
        emit maxRaiseAmountChanged();
    }
}

bool GameHandler::humanCanAct() const
{
    if (!m_game) return false;
    auto hand = m_game->getCurrentHand();
    if (!hand) return false;
    auto seats = hand->getSeatsList();
    if (!seats || seats->empty()) return false;
    auto human = seats->front();
    if (!human) return false;
    const int a = human->getMyAction();
    return a != PLAYER_ACTION_FOLD
        && a != PLAYER_ACTION_ALLIN
        && human->getMyCash() > 0
        && human->isSessionActive();
}

void GameHandler::doActionDone()
{
    if (!m_session) return;
    if (localGameCallbacksBlocked()) return;

    if (m_myTurn) {
        m_myTurn = false;
        emit myTurnChanged();
    }
    // Mein Aktionsfenster schließen: solange m_timeoutSeatId == 0 gälte ich über
    // isMyTurnToAct() weiter als „am Zug" → eine zweite Aktion könnte durchrutschen.
    // Jetzt, da gehandelt, sofort beenden (stopTimeoutAnimation folgt ohnehin).
    if (m_timeoutSeatId == 0) {
        m_timeoutSeatId = -1;
        emit timeoutChanged();
    }
    qDebug() << "[ACTDBG] doActionDone sent, net="
             << (m_session && m_session->isNetworkClientRunning());

    // Ich habe gehandelt → Buttons sofort inaktiv schalten. canAct leitet das
    // aus getMyAction() (!= NONE) ab; die Aktion ist zu diesem Zeitpunkt bereits
    // auf dem Spieler gesetzt (fold/call/raise), daher liefert das Recompute den
    // korrekten Wert (inaktiv bis zur nächsten Runde bzw. bis ich erneut dran bin).
    computeCallAndRaiseAmounts();

    if (m_session->isNetworkClientRunning()) {
        // Network game: send action to server
        m_session->sendClientPlayerAction();
    } else {
        // Local game: advance game loop (equivalent to Qt5 nextPlayerAnimation -> switchRounds)
        // LLM eval harness: collapse the pause for fast unattended runs.
        const int advanceDelay = (m_llm && m_llm->enabled()) ? 40 : 300;
        boost::shared_ptr<Game> game = m_game;
        QTimer::singleShot(advanceDelay, this, [game]() {
            if (game && game->getCurrentHand())
                game->getCurrentHand()->switchRounds();
        });
    }
}

// ─── slots called from QmlGuiInterface ──────────────────────────────────────

void GameHandler::onRefreshSet()
{
    if (localGameCallbacksBlocked()) return;
    qDebug() << "[ACTDBG] >> onRefreshSet myTurn=" << m_myTurn;
    refreshPlayerData();
    computeCallAndRaiseAmounts();
}

void GameHandler::onRefreshAction(int playerId, int playerAction)
{
    if (localGameCallbacksBlocked()) return;
    qDebug() << "[ACTDBG] >> onRefreshAction id=" << playerId << "act=" << playerAction << "myTurn=" << m_myTurn;
    refreshPlayerData();
    computeCallAndRaiseAmounts();

    // Entspricht gametableimpl::refreshAction: nur bei spezifischer Aktion
    // (nicht bei globalem Refresh) den Aktionssound abspielen.
    if (playerId < 0 || playerAction <= 0 || playerAction > 6)
        return;
    // Echte Spieler-Aktion (CHECK/CALL/FOLD/BET/RAISE/ALLIN): Signal an QML
    emit refreshActionTriggered();
    if (!m_config || m_config->readConfigInt("PlayGameActions") == 0)
        return;

    static const char *kActionSounds[] = {
        "", "fold", "check", "call", "bet", "raise", "allin"
    };

    ensureSoundEventHandler();
    if (m_soundEventHandler)
        m_soundEventHandler->playSound(kActionSounds[playerAction], playerId);
}

void GameHandler::onRefreshCash()
{
    if (localGameCallbacksBlocked()) return;
    qDebug() << "[ACTDBG] >> onRefreshCash myTurn=" << m_myTurn;
    refreshPlayerData();
    computeCallAndRaiseAmounts();
}

void GameHandler::onRefreshPlayerName()
{
    if (localGameCallbacksBlocked()) return;
    refreshPlayerData();
}

void GameHandler::onRefreshPot()
{
    if (localGameCallbacksBlocked()) return;
    qDebug() << "[ACTDBG] >> onRefreshPot myTurn=" << m_myTurn;
    refreshPotData();
    refreshPlayerData();
    computeCallAndRaiseAmounts();
}

void GameHandler::onRefreshGameLabels(int gameState)
{
    if (localGameCallbacksBlocked()) return;
    QString newPhase;
    switch (gameState) {
    case 0:  newPhase = "Preflop"; break;
    case 1:  newPhase = "Flop";    break;
    case 2:  newPhase = "Turn";    break;
    case 3:  newPhase = "River";   break;
    default: newPhase = "";        break;
    }

    const bool phaseChanged = (newPhase != m_phaseText);
    if (phaseChanged) {
        // Vor Phasenwechsel myTurn zurücksetzen: onNextRoundCleanGui kommt via
        // QueuedConnection u.U. erst nach dieser synchronen Methode. Ohne Reset
        // wäre m_myTurn noch true → QML myTurnNow=true → Buttons enabled mit
        // veralteten Werten aus der vorherigen Runde.
        if (m_myTurn) {
            m_myTurn = false;
            emit myTurnChanged();
        }
        m_phaseText = newPhase;
        emit phaseTextChanged();
    }

    if (m_game) {
        auto hand = m_game->getCurrentHand();
        if (hand) {
            int newHandNum = hand->getMyID();
            if (newHandNum != m_handNumber) {
                m_handNumber = newHandNum;
                emit handNumberChanged();
            }
        }
    }

    computeCallAndRaiseAmounts();
    // Nach computeCallAndRaiseAmounts() sind alle Werte der neuen Runde korrekt
    // (switchRounds() ist bereits abgeschlossen). QML kann die Vorauswahl nun
    // freischalten – unabhängig davon, ob callAmountChanged gefeuert hat.
    if (phaseChanged)
        emit roundValuesReady();
}

void GameHandler::onMeInAction()
{
    qDebug() << "[ACTDBG] onMeInAction() blocked=" << localGameCallbacksBlocked()
             << "myTurn=" << m_myTurn << "tSeat=" << m_timeoutSeatId;
    if (localGameCallbacksBlocked()) return;
    refreshPlayerData();
    computeCallAndRaiseAmounts();
    if (!m_myTurn) {
        m_myTurn = true;
        emit myTurnChanged();
    }
    qDebug() << "[ACTDBG] meInAction myTurn=" << m_myTurn << "tSeat=" << m_timeoutSeatId;
    if (m_game) {
        auto dh = m_game->getCurrentHand();
        if (dh) {
            auto db = dh->getCurrentBeRo();
            auto ds = dh->getSeatsList();
            if (db && ds && !ds->empty()) {
                auto hp = ds->front();
                qDebug() << "[ACTDBG]   amounts call=" << m_callAmount
                         << "minRaise=" << m_minRaiseAmount << "maxRaise=" << m_maxRaiseAmount
                         << "| mySet=" << hp->getMySet() << "highestSet=" << db->getHighestSet()
                         << "cash=" << hp->getMyCash() << "myButton=" << hp->getMyButton()
                         << "(1=D,2=SB,3=BB) myAction=" << hp->getMyAction()
                         << "round=" << dh->getCurrentRound()
                         << "fullBetRule=" << db->getFullBetRule()
                         << "minRaiseEngine=" << db->getMinimumRaise();
                qDebug() << "[BBDBG] onMeInAction BB-check:"
                         << "bbPosId=" << (int)db->getBigBlindPositionId()
                         << "sbPosId=" << (int)db->getSmallBlindPositionId()
                         << "p0UniqueId=" << hp->getMyUniqueID()
                         << "prevPlayerId=" << dh->getPreviousPlayerID()
                         << "firstRound=" << db->getFirstRound()
                         << "isP0BB=" << (hp->getMyUniqueID() == db->getBigBlindPositionId());
            }
        }
    }
    // LLM eval harness: if the autonomous player drives the hero seat, ask it for
    // a decision instead of waiting for a human click. The engine is blocked
    // waiting on seat 0, so the async request runs on the Qt event loop and the
    // resulting onLlmDecision() submits the move exactly like a button press.
    if (llmAutopilotActive() && !m_llmRequestInFlight) {
        // Decide on the hero's actual turn, with full context in the observation.
        const QJsonObject obs = buildLlmObservation();
        if (!obs.isEmpty()) {
            m_llmRequestInFlight = true;
            m_llm->requestDecision(obs);
        }
    }

    // Maßgeblicher „ich bin am Zug"-Punkt (wie meInAction im Widgets-Client):
    // hier – und nur hier – die vorgemerkte/automatische Aktion auslösen,
    // IMMER (auch wenn m_myTurn oben schon true war, z.B. via Action-Timer).
    emit meInActionTriggered();
}

void GameHandler::onDisableMyButtons()
{
    if (localGameCallbacksBlocked()) return;
    int dbgAct = -1;
    if (m_game) {
        auto hand = m_game->getCurrentHand();
        if (hand) {
            auto seats = hand->getSeatsList();
            if (seats && !seats->empty())
                dbgAct = seats->front()->getMyAction();
        }
    }
    qDebug() << "[ACTDBG] onDisableMyButtons myTurn=" << m_myTurn
             << "p0Action=" << dbgAct
             << "(NONE=0,FOLD=1,CHK=2,CALL=3,BET=4,RAISE=5,ALLIN=6)";
    if (m_myTurn) {
        m_myTurn = false;
        emit myTurnChanged();
    }
}

void GameHandler::onStartTimeoutAnimation(int playerNum, int timeoutSec)
{
    // Log VOR dem Guard, damit blockierte Aufrufe sichtbar sind
    if (playerNum == 0)
        qDebug() << "[ACTDBG] onStartTimeout(0) blocked=" << localGameCallbacksBlocked()
                 << "myTurn=" << m_myTurn << "humanCanAct=" << humanCanAct()
                 << "tSeat=" << m_timeoutSeatId << "timeoutSec=" << timeoutSec;
    if (localGameCallbacksBlocked()) return;

    // Fortschrittsbalken (Ersatz fürs Action-Badge) für den gerade aktiven Sitz.
    if (m_timeoutSeatId != playerNum || m_timeoutSec != timeoutSec) {
        m_timeoutSeatId = playerNum;
        m_timeoutSec = timeoutSec;
        emit timeoutChanged();
    }

    // Der Server zählt jetzt die Aktionszeit für DIESEN Sitz herunter. Ist es
    // mein Sitz (0) und kann ich agieren, ist es definitiv mein Zug. m_myTurn
    // hier setzen (nicht erst in onMeInAction): startTimeoutAnimation kommt
    // VOR meInAction und markiert exakt das Fenster, in dem der Server auf
    // meine Aktion wartet. Sonst gab es ein Fenster, in dem die Buttons bereits
    // aktiv waren (canAct), aber m_myTurn noch false war → Klicks/Vorwahlen
    // wurden als „kein Zug" verworfen (fold/call/raise prüfen m_myTurn) und
    // liefen in den Timeout (Server-Auto-Check).
    if (playerNum == 0 && !m_myTurn && humanCanAct()) {
        m_myTurn = true;
        emit myTurnChanged();
    }
    if (playerNum == 0)
        qDebug() << "[ACTDBG] startTimeout seat0 myTurn=" << m_myTurn
                 << "humanCanAct=" << humanCanAct() << "tSeat=" << m_timeoutSeatId;

    // Wie im Widgets-Client: Ton erst nach 3 Sekunden Vorlauf – nur für mich
    // und nur, wenn ich noch am Zug bin (eine vorgemerkte Aktion kann oben
    // bereits synchron ausgeführt worden sein → dann kein Beep).
    if (playerNum == 0 && m_myTurn && timeoutSec >= 4)
        m_timeoutBeepTimer->start((timeoutSec - 3) * 1000);
}

void GameHandler::onStopTimeoutAnimation(int playerNum)
{
    if (m_timeoutSeatId == playerNum) {
        m_timeoutSeatId = -1;
        emit timeoutChanged();
    }
    m_timeoutBeepTimer->stop();

    // Mein Aktionsfenster ist vorbei (gehandelt oder Zug abgelaufen) → Zug
    // beenden, passend zum Setzen in onStartTimeoutAnimation.
    if (playerNum == 0 && m_myTurn) {
        m_myTurn = false;
        emit myTurnChanged();
    }
}

void GameHandler::onNetworkGameEnded()
{
    // Aus dem Netzwerk-Spiel entfernt (Spielende, geschlossen, gekickt …):
    // den eigenen Zustand sauber zurücksetzen. Ohne dies bleiben m_myTurn und
    // m_game stale; eine spätere Aktion (z.B. Auto-Modus der ComboBox auf der
    // noch sichtbaren GamePage) würde fold()/call() mit gültig aussehender
    // Wache aufrufen und auf Engine-Seite einen null Game-shared_ptr
    // dereferenzieren.
    m_game.reset();
    if (m_myTurn) {
        m_myTurn = false;
        emit myTurnChanged();
    }
    if (m_timeoutSeatId != -1) {
        m_timeoutSeatId = -1;
        emit timeoutChanged();
    }
    m_timeoutBeepTimer->stop();
}

void GameHandler::onNetClientPlayerLeft(unsigned uniquePlayerId)
{
    m_leftPlayers.insert(uniquePlayerId);
    refreshPlayerData();
    emit playersChanged();
}

void GameHandler::checkBustedLocalPlayers()
{
    // Nur im lokalen Spiel (kein Netzwerk-Client).
    if (!m_session || m_session->isNetworkClientRunning()) return;
    if (!m_game) return;

    PlayerList seats = m_game->getSeatsList();
    for (auto it = seats->begin(); it != seats->end(); ++it) {
        int id = (*it)->getMyID();
        // Sitz 0 = menschlicher Spieler, nie automatisch ausblenden.
        if (id <= 0) continue;
        // Leere Sitze überspringen.
        if ((*it)->getMyName().empty()) continue;

        unsigned uid = (*it)->getMyUniqueID();
        // Bereits verlassen → kein Timer nötig.
        if (m_leftPlayers.contains(uid)) continue;

        if ((*it)->getMyCash() == 0) {
            // Noch kein Timer für diesen Spieler: 10-Sekunden-Verzögerung starten.
            if (!m_bustedLocalTimers.contains(uid)) {
                QTimer *t = new QTimer(this);
                t->setSingleShot(true);
                connect(t, &QTimer::timeout, this, [this, uid]() {
                    m_bustedLocalTimers.remove(uid);
                    m_leftPlayers.insert(uid);
                    refreshPlayerData();
                });
                m_bustedLocalTimers.insert(uid, t);
                t->start(10000);
            }
        } else {
            // Spieler hat wieder Chips (z. B. Rebuy) → laufenden Timer verwerfen.
            if (m_bustedLocalTimers.contains(uid)) {
                delete m_bustedLocalTimers.take(uid);
            }
        }
    }
}

void GameHandler::onBlindsSet(int smallBlind)
{
    if (localGameCallbacksBlocked()) return;
    ensureSoundEventHandler();
    if (m_soundEventHandler)
        m_soundEventHandler->blindsWereSet(smallBlind);
}

void GameHandler::refreshBoardCards()
{
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (!hand) return;
    auto board = hand->getBoard();
    if (!board) return;

    int raw[5] = {-1, -1, -1, -1, -1};
    board->getMyCards(raw);

    QVariantList newCards;
    for (int i = 0; i < 5; ++i)
        newCards.append(i < m_boardCardCount ? raw[i] : -1);

    if (newCards != m_boardCards) {
        m_boardCards = newCards;
        emit boardCardsChanged();
    }

    // Keep the "chance" panel in sync with the board.
    refreshHeroHand();
}

void GameHandler::refreshHeroHand()
{
    QString name;
    QVariantList chances;

    if (m_game) {
        auto hand = m_game->getCurrentHand();
        if (hand) {
            auto seats = hand->getSeatsList();
            auto board = hand->getBoard();
            if (seats && !seats->empty() && board) {
                auto hero = seats->front();
                int hc[2] = {-1, -1};
                hero->getMyCards(hc);
                // Show whenever we hold hole cards this hand — even after folding
                // or when not contesting the pot. The board still runs out and the
                // odds stay informative ("what your hand would have become").
                const bool haveCards = hc[0] >= 0 && hc[1] >= 0;
                if (haveCards) {
                    // getMyCardsValueInt() is computed once at hand setup over the
                    // FULL (pre-shuffled) board, so pre-river it is the final hand —
                    // showing it early would leak future cards. Only name the made
                    // hand once all five board cards are legitimately out (river+),
                    // where that value equals the real current hand.
                    const int cvi = hero->getMyCardsValueInt();
                    if (cvi > 0 && static_cast<int>(hand->getCurrentRound()) >= GAME_STATE_RIVER)
                        name = QString::fromStdString(
                            CardsValue::determineHandName(cvi, m_game->getActivePlayerList()));

                    // Per-category odds — mirrors the Qt-widgets chance monitor.
                    int holeCards[2]  = { hc[0], hc[1] };
                    int boardCards[5] = { 0, 0, 0, 0, 0 };
                    board->getMyCards(boardCards);
                    // Clamp post-river (showdown) to river so the final made hand
                    // shows at 100% instead of an empty (uncomputed) table.
                    int r = static_cast<int>(hand->getCurrentRound());
                    if (r > GAME_STATE_RIVER) r = GAME_STATE_RIVER;
                    std::vector<std::vector<int>> ch =
                        CardsValue::calcCardsChance(static_cast<GameState>(r), holeCards, boardCards);

                    static const char *catNames[10] = {
                        "High Card", "One Pair", "Two Pair", "Three of a Kind", "Straight",
                        "Flush", "Full House", "Four of a Kind", "Straight Flush", "Royal Flush"
                    };
                    if (ch.size() >= 2 && ch[0].size() >= 10 && ch[1].size() >= 10) {
                        // High → low (Royal Flush first), like the widgets panel.
                        for (int i = 9; i >= 0; --i) {
                            QVariantMap row;
                            row["label"]    = QString::fromLatin1(catNames[i]);
                            row["pct"]      = ch[0][i];
                            row["possible"] = ch[1][i] != 0;
                            chances.append(row);
                        }
                    }
                }
            }
        }
    }

    if (name != m_heroHandName || chances != m_heroHandChances) {
        m_heroHandName = name;
        m_heroHandChances = chances;
        emit heroHandChanged();
    }
}

void GameHandler::onNextRoundCleanGui()
{
    if (localGameCallbacksBlocked()) return;
    onDisableMyButtons();
    m_pot = 0;
    m_totalPot = 0;
    emit potChanged();
    emit totalPotChanged();
    m_minRaiseAmount = 0;
    m_maxRaiseAmount = 0;
    emit minRaiseAmountChanged();
    emit maxRaiseAmountChanged();
    m_boardCardCount = 0;
    m_boardCards = QVariantList{-1, -1, -1, -1, -1};
    emit boardCardCountChanged();
    emit boardCardsChanged();
    // Keep the chance panel populated between hands (it shows the last hand until
    // the next one is dealt); it is refreshed for the new hand in onAfterDealCards.
    if (!m_winnerSeatIds.isEmpty()) {
        m_winnerSeatIds.clear();
        emit winnerSeatIdsChanged();
    }
    if (!m_winningHandText.isEmpty()) {
        m_winningHandText.clear();
        emit winningHandTextChanged();
    }
    // Showdown beenden, bevor die Spielerdaten neu gebaut werden → Karten zu.
    m_showdownActive = false;
    m_allInRevealed = false;
    if (m_canShowCards) {
        m_canShowCards = false;
        emit canShowCardsChanged();
    }
    refreshPlayerData();
    // Button-Zustand auffrischen: zum Hand-Ende/-Start ist getMyAction() noch
    // die letzte Aktion (!= NONE) → Buttons inaktiv, bis die Engine zur neuen
    // Hand auf NONE zurücksetzt.
    computeCallAndRaiseAmounts();
}

void GameHandler::onDealFlopCards()
{
    if (localGameCallbacksBlocked()) return;
    m_boardCardCount = 3;
    emit boardCardCountChanged();
    refreshBoardCards();
}

void GameHandler::onDealTurnCard()
{
    if (localGameCallbacksBlocked()) return;
    m_boardCardCount = 4;
    emit boardCardCountChanged();
    refreshBoardCards();
}

void GameHandler::onDealRiverCard()
{
    if (localGameCallbacksBlocked()) return;
    m_boardCardCount = 5;
    emit boardCardCountChanged();
    refreshBoardCards();
}

// ─── Q_INVOKABLE actions called from QML ────────────────────────────────────

void GameHandler::fold()
{
    qDebug() << "[FOLDDBG] fold() entry"
             << "myTurn=" << m_myTurn << "tSeat=" << m_timeoutSeatId
             << "isMyTurnToAct=" << isMyTurnToAct();
    if (!m_game || !m_session || !isMyTurnToAct()) {
        qDebug() << "[FOLDDBG] fold() EARLY-RETURN";
        return;
    }

    auto hand = m_game->getCurrentHand();
    if (!hand) return;
    auto seats = hand->getSeatsList();
    if (!seats || seats->empty()) return;
    auto humanPlayer = seats->front();

    qDebug() << "[FOLDDBG] fold() pre-dispatch"
             << "myButton=" << humanPlayer->getMyButton()
             << "round=" << (int)hand->getCurrentRound();

    humanPlayer->setMyAction(PLAYER_ACTION_FOLD, true);
    humanPlayer->setMyTurn(false);
    hand->setPreviousPlayerID(0);

    doActionDone();
}

void GameHandler::call()
{
    qDebug() << "[CALLDBG] call() entry"
             << "myTurn=" << m_myTurn << "tSeat=" << m_timeoutSeatId
             << "isMyTurnToAct=" << isMyTurnToAct();
    if (!m_game || !m_session || !isMyTurnToAct()) {
        qDebug() << "[CALLDBG] call() EARLY-RETURN (game=" << (m_game ? 1 : 0)
                 << " session=" << (m_session ? 1 : 0)
                 << " turn=" << isMyTurnToAct() << ")";
        return;
    }

    auto hand = m_game->getCurrentHand();
    if (!hand) return;
    auto seats = hand->getSeatsList();
    if (!seats || seats->empty()) return;
    auto humanPlayer = seats->front();
    auto bero = hand->getCurrentBeRo();
    if (!bero) return;

    int highestSet = bero->getHighestSet();
    int humanSet = humanPlayer->getMySet();

    qDebug() << "[CALLDBG] call() pre-dispatch"
             << "humanSet=" << humanSet << "highestSet=" << highestSet
             << "humanCash=" << humanPlayer->getMyCash()
             << "myButton=" << humanPlayer->getMyButton()
             << "(1=D,2=SB,3=BB)"
             << "round=" << (int)hand->getCurrentRound()
             << "myAction=" << humanPlayer->getMyAction();

    if (highestSet == 0 || humanSet >= highestSet) {
        // Check – entweder kein Einsatz gesetzt (highestSet == 0) ODER der
        // eigene Einsatz entspricht bereits dem höchsten (klassischer Fall:
        // BB-Option preflop, alle haben gelimpt). Server erwartet hier
        // explizit PLAYER_ACTION_CHECK; ein CALL ohne tatsächliche Chip-
        // Bewegung würde verworfen → Timeout mit Default-Action.
        humanPlayer->setMyAction(PLAYER_ACTION_CHECK, true);
        qDebug() << "[CALLDBG] call() -> CHECK branch";
    } else if (humanPlayer->getMyCash() + humanSet <= highestSet) {
        // All-in call
        humanPlayer->setMySet(humanPlayer->getMyCash());
        humanPlayer->setMyCash(0);
        humanPlayer->setMyAction(PLAYER_ACTION_ALLIN, true);
        qDebug() << "[CALLDBG] call() -> ALLIN branch lastRelSet=" << humanPlayer->getMyLastRelativeSet();
    } else {
        // Regular call
        humanPlayer->setMySet(highestSet - humanSet);
        humanPlayer->setMyAction(PLAYER_ACTION_CALL, true);
        qDebug() << "[CALLDBG] call() -> CALL branch lastRelSet=" << humanPlayer->getMyLastRelativeSet()
                 << "newMySet=" << humanPlayer->getMySet();
    }

    humanPlayer->setMyTurn(false);
    hand->getBoard()->collectSets();
    hand->setPreviousPlayerID(0);

    doActionDone();
    onRefreshPot();
}

void GameHandler::raise(int amount)
{
    qDebug() << "[RAISEDBG] raise() entry amount=" << amount
             << "myTurn=" << m_myTurn << "tSeat=" << m_timeoutSeatId
             << "isMyTurnToAct=" << isMyTurnToAct();
    if (!m_game || !m_session || !isMyTurnToAct()) {
        qDebug() << "[RAISEDBG] raise() EARLY-RETURN";
        return;
    }

    auto hand = m_game->getCurrentHand();
    if (!hand) return;
    auto seats = hand->getSeatsList();
    if (!seats || seats->empty()) return;
    auto humanPlayer = seats->front();
    auto bero = hand->getCurrentBeRo();
    if (!bero) return;

    // Default to minimum raise if no amount specified
    if (amount <= 0) {
        amount = m_minRaiseAmount;
    }
    if (amount <= 0) {
        qDebug() << "[RAISEDBG] raise() amount<=0 after minRaise fallback (m_minRaiseAmount=" << m_minRaiseAmount << ") ABORT";
        return;
    }
    qDebug() << "[RAISEDBG] raise() pre-dispatch amount=" << amount
             << "humanSet=" << humanPlayer->getMySet()
             << "humanCash=" << humanPlayer->getMyCash()
             << "highestSet=" << bero->getHighestSet()
             << "minRaiseEngine=" << bero->getMinimumRaise()
             << "myButton=" << humanPlayer->getMyButton()
             << "round=" << (int)hand->getCurrentRound();

    int tempCash = humanPlayer->getMyCash();

    humanPlayer->setMySet(amount); // adds to set, deducts from cash

    if (amount >= tempCash) {
        // All-in
        humanPlayer->setMyCash(0);
        humanPlayer->setMyAction(PLAYER_ACTION_ALLIN, true);
        if (bero->getHighestSet() + bero->getMinimumRaise() > humanPlayer->getMySet()) {
            bero->setFullBetRule(true);
        }
        if (humanPlayer->getMySet() > bero->getHighestSet()) {
            bero->setMinimumRaise(humanPlayer->getMySet() - bero->getHighestSet());
            bero->setHighestSet(humanPlayer->getMySet());
            hand->setLastActionPlayerID(humanPlayer->getMyUniqueID());
        }
    } else {
        const bool firstBet = (bero->getHighestSet() == 0);
        humanPlayer->setMyAction(firstBet ? PLAYER_ACTION_BET : PLAYER_ACTION_RAISE, true);
        bero->setMinimumRaise(humanPlayer->getMySet() - bero->getHighestSet());
        bero->setHighestSet(humanPlayer->getMySet());
        hand->setLastActionPlayerID(humanPlayer->getMyUniqueID());
    }

    humanPlayer->setMyTurn(false);
    hand->getBoard()->collectSets();
    hand->setPreviousPlayerID(0);

    doActionDone();
    onRefreshPot();
}

void GameHandler::allIn()
{
    if (!m_game || !m_session || !isMyTurnToAct()) return;

    auto hand = m_game->getCurrentHand();
    if (!hand) return;
    auto seats = hand->getSeatsList();
    if (!seats || seats->empty()) return;
    auto humanPlayer = seats->front();
    auto bero = hand->getCurrentBeRo();
    if (!bero) return;

    humanPlayer->setMySet(humanPlayer->getMyCash());
    humanPlayer->setMyCash(0);
    humanPlayer->setMyAction(PLAYER_ACTION_ALLIN, true);

    if (bero->getHighestSet() + bero->getMinimumRaise() > humanPlayer->getMySet()) {
        bero->setFullBetRule(true);
    }
    if (humanPlayer->getMySet() > bero->getHighestSet()) {
        bero->setMinimumRaise(humanPlayer->getMySet() - bero->getHighestSet());
        bero->setHighestSet(humanPlayer->getMySet());
        hand->setLastActionPlayerID(humanPlayer->getMyUniqueID());
    }

    humanPlayer->setMyTurn(false);
    hand->getBoard()->collectSets();
    hand->setPreviousPlayerID(0);

    doActionDone();
    onRefreshPot();
}

void GameHandler::showMyCards()
{
    if (!m_canShowCards) return;
    if (m_session) m_session->showMyCards();
    m_canShowCards = false;
    emit canShowCardsChanged();
}

// ─── Local game startup ──────────────────────────────────────────────────────

bool GameHandler::llmAutopilotActive() const
{
    // Only drive the hero seat in a running LOCAL game (the eval setup); never in
    // network games. humanCanAct() ensures it really is our turn and we can act.
    return m_llm && m_llm->enabled()
           && isLocalGameRunning()
           && humanCanAct();
}

QJsonObject GameHandler::buildLlmObservation()
{
    QJsonObject obs;
    if (!m_game) return obs;
    auto hand = m_game->getCurrentHand();
    if (!hand) return obs;
    auto board = hand->getBoard();
    auto bero = hand->getCurrentBeRo();
    auto seats = hand->getSeatsList();
    if (!board || !bero || !seats || seats->empty()) return obs;
    auto hero = seats->front();
    if (!hero) return obs;

    // Make sure to_call / min / max and the hand odds reflect the current state.
    computeCallAndRaiseAmounts();
    refreshHeroHand();

    const int round = static_cast<int>(hand->getCurrentRound());

    obs["hand_id"]     = hand->getMyID();
    obs["round"]       = roundName(round);
    obs["small_blind"] = hand->getSmallBlind();
    obs["big_blind"]   = hand->getSmallBlind() * 2;
    obs["pot"]         = board->getPot() + board->getSets();

    // IMPORTANT: board->getMyCards() returns all 5 pre-shuffled board cards from
    // hand setup, including ones not yet dealt. Only expose the cards visible on
    // the CURRENT street, or we would leak future community cards to the model.
    int visibleBoard = 0;
    if (round == GAME_STATE_FLOP) visibleBoard = 3;
    else if (round == GAME_STATE_TURN) visibleBoard = 4;
    else if (round >= GAME_STATE_RIVER) visibleBoard = 5;  // river + post-river
    int bc[5] = {-1, -1, -1, -1, -1};
    board->getMyCards(bc);
    QJsonArray boardArr;
    for (int i = 0; i < visibleBoard; ++i)
        if (bc[i] >= 0) boardArr.append(cardToStr(bc[i]));
    obs["board"] = boardArr;

    int hcards[2] = {-1, -1};
    hero->getMyCards(hcards);
    QJsonArray holeArr;
    for (int i = 0; i < 2; ++i)
        if (hcards[i] >= 0) holeArr.append(cardToStr(hcards[i]));
    QJsonObject heroObj;
    heroObj["seat"]           = hero->getMyID();
    heroObj["name"]           = QString::fromStdString(hero->getMyName());
    heroObj["stack"]          = hero->getMyCash();
    heroObj["bet_this_round"] = hero->getMySet();
    heroObj["hole_cards"]     = holeArr;
    heroObj["position"]       = buttonName(hero->getMyButton());
    obs["hero"] = heroObj;

    // Legal action surface (mirrors what the GUI offers a human).
    const bool canRaise = (m_minRaiseAmount > 0 && m_maxRaiseAmount >= m_minRaiseAmount);
    obs["to_call"]   = m_callAmount;
    obs["min_raise"] = m_minRaiseAmount;
    obs["max_raise"] = m_maxRaiseAmount;
    obs["can_check"] = (m_callAmount == 0);
    obs["can_raise"] = canRaise;

    QJsonArray legal;
    legal.append(QStringLiteral("fold"));
    legal.append(m_callAmount == 0 ? QStringLiteral("check") : QStringLiteral("call"));
    if (canRaise) {
        legal.append(m_callAmount == 0 ? QStringLiteral("bet") : QStringLiteral("raise"));
        legal.append(QStringLiteral("allin"));
    }
    obs["legal_actions"] = legal;

    QJsonArray players;
    for (auto it = seats->begin(); it != seats->end(); ++it) {
        if ((*it)->getMyName().empty()) continue;
        QJsonObject po;
        po["seat"]           = (*it)->getMyID();
        po["name"]           = QString::fromStdString((*it)->getMyName());
        po["stack"]          = (*it)->getMyCash();
        po["bet_this_round"] = (*it)->getMySet();
        po["last_action"]    = actionName((*it)->getMyAction());
        po["position"]       = buttonName((*it)->getMyButton());
        po["in_hand"]        = (*it)->getMyAction() != PLAYER_ACTION_FOLD && (*it)->isSessionActive();
        po["is_hero"]        = ((*it)->getMyID() == hero->getMyID());
        players.append(po);
    }
    obs["players"] = players;

    // Context layer 1: the betting action of the CURRENT hand so far.
    if (!m_llmHandActions.isEmpty())
        obs["hand_history"] = m_llmHandActions;

    // The hero's current hand odds (probability of making each category by the
    // river, given only the visible cards) — same data as the on-screen panel.
    if (!m_heroHandChances.isEmpty())
        obs["hand_odds"] = QJsonArray::fromVariantList(m_heroHandChances);

    // Context layer 2: compact summaries of recent finished hands (incl. ones the
    // hero folded), so the model can review how they played out.
    if (!m_llmRecentHands.isEmpty()) {
        QJsonArray recent;
        for (const QJsonObject &h : m_llmRecentHands)
            recent.append(h);
        obs["recent_hands"] = recent;
    }

    // Context layer 3: per-opponent tendencies distilled from observed actions.
    const QString heroName = QString::fromStdString(hero->getMyName());
    QJsonArray oppStats;
    for (auto it = m_llmOppStats.constBegin(); it != m_llmOppStats.constEnd(); ++it) {
        if (it.key() == heroName) continue;
        const OppStat &s = it.value();
        const int n = s.folds + s.checks + s.calls + s.bets + s.raises + s.allins;
        if (n == 0) continue;
        QJsonObject o;
        o["name"]            = it.key();
        o["actions_seen"]    = n;
        o["aggressive_pct"]  = (100 * (s.bets + s.raises + s.allins)) / n;
        o["fold_pct"]        = (100 * s.folds) / n;
        oppStats.append(o);
    }
    if (!oppStats.isEmpty())
        obs["opponent_stats"] = oppStats;

    return obs;
}

QString GameHandler::llmHeroName() const
{
    if (!m_game) return QString();
    auto hand = m_game->getCurrentHand();
    if (!hand) return QString();
    auto seats = hand->getSeatsList();
    if (!seats || seats->empty()) return QString();
    return QString::fromStdString(seats->front()->getMyName());
}

void GameHandler::resetLlmContext()
{
    m_llmOppStats.clear();
    m_llmHandActions = QJsonArray();
    m_llmHandShown = QJsonArray();
    m_llmHandFinalBoard = QJsonArray();
    m_llmHandWinners.clear();
    m_llmHandPot = 0;
    m_llmCurrentHandId = -1;
    m_llmRecentHands.clear();
    if (m_llm) m_llm->resetConversation();
}

void GameHandler::onLlmRecordAction(const QString &name, int action, int amount)
{
    if (!m_llm || !m_llm->enabled()) return;
    // The hero (seat 0) is recorded in onLlmDecision; logPlayerActionMsg here is for
    // the computer opponents. Skipping the hero also avoids any double-counting.
    if (name == llmHeroName()) return;

    int round = -1;
    if (m_game) {
        auto h = m_game->getCurrentHand();
        if (h) round = static_cast<int>(h->getCurrentRound());
    }

    QJsonObject a;
    a["street"] = roundName(round);
    a["name"]   = name;
    a["action"] = actionName(action);
    if (amount > 0 && (action == PLAYER_ACTION_CALL || action == PLAYER_ACTION_BET
                       || action == PLAYER_ACTION_RAISE || action == PLAYER_ACTION_ALLIN))
        a["amount"] = amount;
    m_llmHandActions.append(a);

    OppStat &s = m_llmOppStats[name];
    switch (action) {
    case PLAYER_ACTION_FOLD:  s.folds++;  break;
    case PLAYER_ACTION_CHECK: s.checks++; break;
    case PLAYER_ACTION_CALL:  s.calls++;  break;
    case PLAYER_ACTION_BET:   s.bets++;   break;
    case PLAYER_ACTION_RAISE: s.raises++; break;
    case PLAYER_ACTION_ALLIN: s.allins++; break;
    default: break;
    }
}

void GameHandler::onLlmHandStart(int handId)
{
    if (!m_llm || !m_llm->enabled()) return;
    m_llm->onHandStart();   // clears the reasoning chain when scope == "hand"

    // Finalize the just-completed hand into the recent-hands ring buffer.
    if (m_llmCurrentHandId >= 0 && (!m_llmHandActions.isEmpty() || !m_llmHandWinners.isEmpty())) {
        QJsonObject rec;
        rec["hand_id"] = m_llmCurrentHandId;
        rec["board"]   = m_llmHandFinalBoard;
        rec["pot"]     = m_llmHandPot;
        rec["winners"] = QJsonArray::fromStringList(m_llmHandWinners);
        rec["shown"]   = m_llmHandShown;
        rec["actions"] = m_llmHandActions;   // how the hand was played, for learning
        m_llmRecentHands.append(rec);
        while (m_llmRecentHands.size() > m_llmMaxRecentHands)
            m_llmRecentHands.removeFirst();
    }

    m_llmHandActions = QJsonArray();
    m_llmHandShown = QJsonArray();
    m_llmHandFinalBoard = QJsonArray();
    m_llmHandWinners.clear();
    m_llmHandPot = 0;
    m_llmCurrentHandId = handId;
}

void GameHandler::onLlmHandWinner(const QString &name, int pot, bool mainPot)
{
    if (!m_llm || !m_llm->enabled()) return;
    m_llmHandWinners << (name + QStringLiteral(" $") + QString::number(pot)
                         + (mainPot ? QString() : QStringLiteral(" (side)")));
    m_llmHandPot += pot;

    // Capture the final community cards while the finished hand is still live.
    if (m_game) {
        auto hand = m_game->getCurrentHand();
        if (hand) {
            auto board = hand->getBoard();
            if (board) {
                int bc[5] = {-1, -1, -1, -1, -1};
                board->getMyCards(bc);
                QJsonArray arr;
                for (int i = 0; i < 5; ++i)
                    if (bc[i] >= 0) arr.append(cardToStr(bc[i]));
                m_llmHandFinalBoard = arr;
            }
        }
    }
}

void GameHandler::onLlmShowCards(const QString &name, int card1, int card2)
{
    if (!m_llm || !m_llm->enabled()) return;
    QJsonObject o;
    o["name"] = name;
    QJsonArray cs;
    if (card1 >= 0) cs.append(cardToStr(card1));
    if (card2 >= 0) cs.append(cardToStr(card2));
    o["cards"] = cs;
    m_llmHandShown.append(o);
}

void GameHandler::onLlmDecision(const QString &action, int amount, const QString &reasoning)
{
    m_llmRequestInFlight = false;

    // The game may have been torn down or moved on while the request was in
    // flight; only act if it is still genuinely the hero's turn.
    if (!m_llm || !m_llm->enabled()) return;
    if (!isLocalGameRunning()) return;
    if (!isMyTurnToAct()) {
        qDebug() << "[LLM] decision arrived but not hero's turn; dropping" << action;
        return;
    }

    // Surface the model's intended move + reasoning in the in-game action log, so
    // you can watch its thinking. Shown just before the move is applied.
    QString thought = QStringLiteral("\u{1F916} ") + action.toUpper();
    if ((action == QLatin1String("bet") || action == QLatin1String("raise")) && amount > 0)
        thought += QStringLiteral(" %1").arg(amount);
    if (!reasoning.isEmpty())
        thought += QStringLiteral(" — ") + reasoning;
    appendGameLog(thought, LogSitOut);

    applyLlmAction(action, amount);
}

void GameHandler::onContextUsage(int promptTokens, int completionTokens, int totalTokens, int contextSize)
{
    Q_UNUSED(totalTokens)
    if (promptTokens <= 0) return;

    const QLocale loc = QLocale::system();
    QString txt;
    if (contextSize > 0) {
        const int pct = qMin(100, (promptTokens * 100) / contextSize);
        txt = QStringLiteral("ctx %1 / %2 (%3%)")
              .arg(loc.toString(promptTokens), loc.toString(contextSize)).arg(pct);
    } else {
        txt = QStringLiteral("ctx %1 tok").arg(loc.toString(promptTokens));
    }
    // Generated (reasoning+answer) tokens this turn — the OUTPUT, distinct from the
    // prompt/context above. Helps tell "input context" from "thinking it produced".
    if (completionTokens > 0)
        txt += QStringLiteral("  ·  out %1").arg(loc.toString(completionTokens));
    if (txt != m_contextUsageText) {
        m_contextUsageText = txt;
        emit contextUsageChanged();
    }
}

void GameHandler::onThinking(const QString &reasoningContent)
{
    if (reasoningContent == m_latestThinking) return;
    m_latestThinking = reasoningContent;
    emit latestThinkingChanged();
}

void GameHandler::applyLlmAction(const QString &action, int amount)
{
    // Record the hero's move into the within-hand betting history (opponents are
    // recorded via onLlmRecordAction).
    int heroAmt = 0;
    if (action == QLatin1String("bet") || action == QLatin1String("raise"))
        heroAmt = amount;
    else if (action == QLatin1String("call"))
        heroAmt = m_callAmount;
    else if (action == QLatin1String("allin"))
        heroAmt = m_maxRaiseAmount;
    int round = -1;
    if (m_game) {
        auto h = m_game->getCurrentHand();
        if (h) round = static_cast<int>(h->getCurrentRound());
    }
    QJsonObject a;
    a["street"] = roundName(round);
    a["name"]   = llmHeroName();
    a["action"] = action;
    if (heroAmt > 0) a["amount"] = heroAmt;
    m_llmHandActions.append(a);

    if (action == QLatin1String("fold")) {
        fold();
    } else if (action == QLatin1String("check") || action == QLatin1String("call")) {
        call();
    } else if (action == QLatin1String("bet") || action == QLatin1String("raise")) {
        raise(amount);
    } else if (action == QLatin1String("allin")) {
        allIn();
    } else {
        qWarning() << "[LLM] unexpected action" << action << "- falling back to call/check";
        call();
    }
}

void GameHandler::startLocalGame()
{
    if (!m_session) return;
    m_localGameExitRequested = false;
    resetLlmContext();
    if (m_llm) m_llm->refreshContextSize();   // keep the ctx badge denominator fresh

    GameData gameData;
    if (m_config) {
        gameData.maxNumberOfPlayers = m_config->readConfigInt("NumberOfPlayers");
        gameData.startMoney         = m_config->readConfigInt("StartCash");
        gameData.firstSmallBlind    = m_config->readConfigInt("FirstSmallBlind");
    }
    if (gameData.maxNumberOfPlayers < 2) gameData.maxNumberOfPlayers = 6;
    if (gameData.startMoney <= 0)        gameData.startMoney         = 1500;
    if (gameData.firstSmallBlind <= 0)   gameData.firstSmallBlind    = 10;

    // Defaults match Qt5 local game defaults
    gameData.raiseIntervalMode              = RAISE_ON_HANDNUMBER;
    gameData.raiseSmallBlindEveryHandsValue = 8;
    gameData.raiseMode                      = DOUBLE_BLINDS;
    gameData.guiSpeed                       = 4;
    gameData.delayBetweenHandsSec           = 7;
    gameData.playerActionTimeoutSec         = 20;

    StartData startData;
    startData.numberOfPlayers     = gameData.maxNumberOfPlayers;
    startData.startDealerPlayerId = 0;

    m_session->startLocalGame(gameData, startData);

    // Sync m_game so action methods and refreshes work immediately
    auto game = m_session->getCurrentGame();
    if (game) setGame(game);
}

void GameHandler::endLocalGame()
{
    if (!m_session) return;
    if (m_session->isNetworkClientRunning()) return;

    m_localGameExitRequested = true;
    m_game.reset();

    // Ausstehende Busted-Player-Timer abbrechen.
    qDeleteAll(m_bustedLocalTimers);
    m_bustedLocalTimers.clear();

    // Clear the chance panel when leaving the game (it persists between hands, but
    // should not linger into a new game).
    if (!m_heroHandName.isEmpty() || !m_heroHandChances.isEmpty()) {
        m_heroHandName.clear();
        m_heroHandChances.clear();
        emit heroHandChanged();
    }

    if (m_myTurn) {
        m_myTurn = false;
        emit myTurnChanged();
    }

    refreshPlayerData();
}

bool GameHandler::isLocalGameRunning() const
{
    if (m_localGameExitRequested) return false;
    if (!m_session) return false;
    if (m_session->isNetworkClientRunning()) return false;
    if (m_game) return true;
    return static_cast<bool>(m_session->getCurrentGame());
}

// ─── Game-loop advance slots (called via QMetaObject from QmlGuiInterface) ───

void GameHandler::onRunBeRo()
{
    if (localGameCallbacksBlocked()) return;
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (hand && hand->getCurrentBeRo())
        hand->getCurrentBeRo()->run();
}

void GameHandler::onNextPlayerBeRo()
{
    if (localGameCallbacksBlocked()) return;
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (hand && hand->getCurrentBeRo())
        hand->getCurrentBeRo()->nextPlayer();
}

// Called after the board cards of a street have been dealt. In an all-in
// condition there is no more betting (the running-player list is empty), so
// calling BeRo::run() would throw ERR_RUNNING_PLAYER_NOT_FOUND. Instead we
// advance straight to the next street/showdown via switchRounds(). In a normal
// (non-all-in) round we start the betting via BeRo::run().
void GameHandler::onAfterDealCards()
{
    if (localGameCallbacksBlocked()) return;
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (!hand) return;

    // Show the "chance" panel as soon as hole cards are known (preflop).
    refreshHeroHand();

    if (hand->getAllInCondition()) {
        hand->switchRounds();
    } else if (hand->getCurrentBeRo()) {
        hand->getCurrentBeRo()->run();
    }
}

void GameHandler::onSwitchRounds()
{
    if (localGameCallbacksBlocked()) return;
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (hand) hand->switchRounds();
}

void GameHandler::onPostRiverRunBeRo()
{
    if (localGameCallbacksBlocked()) return;
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (hand && hand->getCurrentBeRo())
        hand->getCurrentBeRo()->postRiverRun();
}

void GameHandler::startNextHandOrEndGame()
{
    if (!m_session || m_localGameExitRequested) return;
    if (m_session->isNetworkClientRunning()) return;
    auto game = m_session->getCurrentGame();
    if (!game) return;

    // Count players who still hold chips. When only one remains the tournament is
    // over: announce the winner and STOP — do NOT start another hand. Starting a
    // hand with a single player crashes the engine (initHand). This mirrors the
    // Qt-widgets client's post-river game-over guard (gametableimpl.cpp ~3100),
    // which the QML client was missing entirely.
    int withCash = 0;
    boost::shared_ptr<PlayerInterface> winner;
    PlayerList active = game->getActivePlayerList();
    for (auto it = active->begin(); it != active->end(); ++it) {
        if ((*it)->getMyCash() > 0) {
            ++withCash;
            winner = *it;
        }
    }

    if (withCash <= 1) {
        auto hand = game->getCurrentHand();
        if (winner && hand && hand->getGuiInterface())
            hand->getGuiInterface()->logPlayerWinGame(winner->getMyName(), game->getMyGameID());
        qInfo() << "[LLM] game over —"
                << (winner ? QString::fromStdString(winner->getMyName()) : QStringLiteral("?"))
                << "wins the tournament; not starting another hand.";
        return;
    }

    game->initHand();
    game->startHand();
}

void GameHandler::onShowdown()
{
    if (localGameCallbacksBlocked()) return;
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (!hand) return;
    auto board = hand->getBoard();
    if (!board) return;

    // Showdown ist jetzt aktiv → Gegnerkarten dürfen aufgedeckt werden
    // (determinePlayerNeedToShowCards() wurde in postRiverRun() bereits aufgerufen).
    m_showdownActive = true;
    refreshPlayerData();
    computeCallAndRaiseAmounts(); // Buttons sofort deaktivieren

    // Pot ist bereits verteilt. Gewinner ermitteln und Haupt-/Side-Pot exakt
    // wie der Widgets-Client (gameTableImpl::postRiverRunAnimation3) trennen.
    std::list<unsigned> winners = board->getWinners();
    auto activeList = hand->getActivePlayerList();
    auto bero = hand->getCurrentBeRo();

    // Side-Pot-Kriterium: bei All-In mit mehreren Geld-Gewinnern gewinnt der
    // Spieler mit dem besten Blatt (höchster cardsValueInt) den Hauptpot, alle
    // übrigen Gewinner einen Side-Pot. getAllInCondition() statt PLAYER_ACTION_ALLIN,
    // da ResetPlayerActions() den All-In-Status zu Beginn jeder Setzrunde löscht.
    const bool hasAllInPlayer = hand->getAllInCondition();
    const int highestWinnerCardsValue = bero ? bero->getHighestCardsValue() : 0;
    int winnersWithMoney = 0;
    if (activeList) {
        for (auto it = activeList->begin(); it != activeList->end(); ++it) {
            const bool isW = std::find(winners.begin(), winners.end(), (*it)->getMyUniqueID()) != winners.end();
            if (isW && (*it)->getLastMoneyWon() > 0)
                ++winnersWithMoney;
        }
    }
    auto isMainPotWinner = [&](const auto &p) -> bool {
        const bool isW = std::find(winners.begin(), winners.end(), p->getMyUniqueID()) != winners.end();
        const bool hasActuallyWon = isW && p->getLastMoneyWon() > 0;
        if (p->getMyAction() == PLAYER_ACTION_FOLD || !hasActuallyWon)
            return false;
        if (hasAllInPlayer && winnersWithMoney > 1
            && p->getMyCardsValueInt() < highestWinnerCardsValue)
            return false; // Side-Pot-Gewinner
        return true;
    };

    // Winner-Badge: jeder nicht gefoldete HAUPTPOT-Gewinner (mehrere bei Split-
    // Pot). Side-Pot-Gewinner bekommen KEIN Badge – wie im Widgets-Client, der
    // das "Winner"-Label nur für isMainPot-Gewinner setzt.
    QVariantList newWinners;
    if (activeList) {
        for (auto it = activeList->begin(); it != activeList->end(); ++it)
            if (isMainPotWinner(*it))
                newWinners.append((*it)->getMyID());
    }
    if (newWinners != m_winnerSeatIds) {
        m_winnerSeatIds = newWinners;
        emit winnerSeatIdsChanged();
    }

    // Name der Gewinner-Hand ermitteln (wie label_WinningCombination im Widgets-
    // Client). Nur sinnvoll, wenn es einen echten Showdown gibt (mehr als ein
    // nicht gefoldeter Spieler) – andernfalls bleibt der Text leer.
    QString newHandText;
    int nonFold = 0;
    if (activeList) {
        for (auto it = activeList->begin(); it != activeList->end(); ++it)
            if ((*it)->getMyAction() != PLAYER_ACTION_FOLD) ++nonFold;
    }
    if (activeList && bero && nonFold > 1) {
        std::string name = CardsValue::determineHandName(bero->getHighestCardsValue(), activeList);
        newHandText = QString::fromStdString(name);
    }

    if (newHandText != m_winningHandText) {
        m_winningHandText = newHandText;
        emit winningHandTextChanged();
    }

    // ── Showdown im Spielverlauf protokollieren (Logik 1:1 aus dem Widgets-Client) ──
    // Die Engine ruft im PokerTH-Client weder logFlipHoleCardsMsg noch
    // logPlayerWinsMsg von selbst auf – im Qt-Widgets-Client macht das die GUI
    // (gameTableImpl::postRiverRunAnimation2/3). Daher hier nachgebildet, sonst
    // fehlen aufgedeckte Karten und der Sieger im "Spielverlauf"-Overlay.
    if (!activeList) return;

    // 1) Aufgedeckte Hole-Cards der Spieler, die laut Engine zeigen müssen
    //    (wie showHoleCards → setMyCardsFlip(1,1) für die Post-River-Runde:
    //    "name shows [c0, c1] - \"Handname\"").
    for (auto it = activeList->begin(); it != activeList->end(); ++it) {
        if ((*it)->getMyAction() == PLAYER_ACTION_FOLD || !(*it)->checkIfINeedToShowCards())
            continue;
        int cards[2] = {-1, -1};
        (*it)->getMyCards(cards);
        if (cards[0] < 0 || cards[1] < 0)
            continue;
        QString line = QString::fromStdString((*it)->getMyName())
                     + " shows [" + logCard(cards[0]) + ", " + logCard(cards[1]) + "]";
        const int cardsValueInt = (*it)->getMyCardsValueInt();
        if (cardsValueInt != -1) {
            std::string handName = CardsValue::determineHandName(cardsValueInt, activeList);
            if (!handName.empty())
                line += " - \"" + QString::fromStdString(handName) + "\"";
        }
        appendGameLog(line);
    }

    // 2) Gewinner – Haupt-/Side-Pot wie postRiverRunAnimation3. Echte Gewinner
    //    stehen in der winners-Liste UND haben tatsächlich Geld gewonnen.
    //    hasAllInPlayer/winnersWithMoney/highestWinnerCardsValue oben berechnet.
    for (auto it = activeList->begin(); it != activeList->end(); ++it) {
        const bool isWinner = std::find(winners.begin(), winners.end(), (*it)->getMyUniqueID()) != winners.end();
        const bool hasActuallyWon = isWinner && (*it)->getLastMoneyWon() > 0;
        if ((*it)->getMyAction() == PLAYER_ACTION_FOLD || !hasActuallyWon)
            continue;
        // Bei All-In mit mehreren Gewinnern: bestes Blatt = Hauptpot, Rest Side-Pot.
        const bool isMainPot = isMainPotWinner(*it);
        QString msg = QString::fromStdString((*it)->getMyName())
                    + " wins $" + QString::number((*it)->getLastMoneyWon());
        if (!isMainPot)
            msg += QStringLiteral(" (side pot)");
        appendGameLog(msg, isMainPot ? LogWinnerMain : LogWinnerSide);
    }

    // 3) Sit-Out für Spieler ohne Cash (wie gameTableImpl nach der Pot-Verteilung).
    for (auto it = activeList->begin(); it != activeList->end(); ++it) {
        if ((*it)->getMyCash() == 0)
            appendGameLog(QString::fromStdString((*it)->getMyName()) + " sits out", LogSitOut);
    }

    // 4) "Show"-Button: Mensch-Spieler (Sitz 0) kann seine Karten freiwillig zeigen,
    //    wenn er nicht gefoldet hat und nicht zeigen MUSS (Logik 1:1 aus dem
    //    Qt-Widgets-Client, gameTableImpl::postRiverRunAnimation2).
    bool newCanShow = false;
    auto seatsList = hand->getSeatsList();
    if (seatsList && !seatsList->empty()) {
        auto humanPlayer = seatsList->front(); // seat 0
        if (humanPlayer->getMyActiveStatus()
            && humanPlayer->getMyAction() != PLAYER_ACTION_FOLD) {
            if (nonFold == 1) {
                // Gewonnen ohne Showdown – kann zeigen
                newCanShow = true;
            } else if (nonFold > 1 && !humanPlayer->checkIfINeedToShowCards()) {
                // Mehrere aktive Spieler, Mensch muss aber nicht zeigen – kann freiwillig zeigen
                newCanShow = true;
            }
        }
    }
    if (newCanShow != m_canShowCards) {
        m_canShowCards = newCanShow;
        emit canShowCardsChanged();
    }
}

void GameHandler::onFlipHolecardsAllIn()
{
    // Karten aller nicht-gefoldeten Spieler aufdecken (All-in-Runout).
    // Die Engine hat setMyCards() bereits für alle All-In-Spieler aufgerufen
    // (clientstate.cpp: AllInShowCardsMessage-Handler).
    qDebug() << "[ALLIN] onFlipHolecardsAllIn() blocked=" << localGameCallbacksBlocked()
             << "hasGame=" << (bool)m_game;
    if (localGameCallbacksBlocked()) return;
    if (!m_game) return;
    auto hand = m_game->getCurrentHand();
    if (!hand) return;

    // Wie im Qt-Widgets-Client: nur aufdecken, wenn >= 2 Spieler nicht gefoldet
    // haben (anderenfalls hat jemand schon gewonnen und es gibt nichts zu zeigen).
    auto active = hand->getActivePlayerList();
    if (!active) return;
    int nonFolded = 0;
    for (auto it = active->begin(); it != active->end(); ++it) {
        int c[2] = {-1, -1};
        (*it)->getMyCards(c);
        qDebug() << "[ALLIN]   active seatId=" << (*it)->getMyID()
                 << "action=" << (int)(*it)->getMyAction()
                 << "cards=" << c[0] << "/" << c[1];
        if ((*it)->getMyAction() != PLAYER_ACTION_FOLD) ++nonFolded;
    }
    qDebug() << "[ALLIN]   nonFolded=" << nonFolded;
    if (nonFolded < 2) {
        qDebug() << "[ALLIN]   GUARD: nonFolded<2 - aborting";
        return;
    }

    m_allInRevealed = true;
    qDebug() << "[ALLIN]   m_allInRevealed set to true, calling refreshPlayerData";
    refreshPlayerData();
}
