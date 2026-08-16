/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#include "listenqueue.h"

#include "tapeoutconstants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimerEvent>

#include <algorithm>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {
//! Batched so a busy session does not rewrite the file once per track.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
constexpr auto WriteInterval = 30s;
#else
constexpr auto WriteInterval = 30000;
#endif

/*!
 * A listen's identity for queue purposes. Tapedeck derives its own `source_id`
 * from the user, the timestamp and the title, so this mirrors that: matching on
 * the same fields keeps the queue's idea of "the same listen" in step with the
 * server's.
 */
bool sameListen(const Fooyin::Tapeout::Listen& a, const Fooyin::Tapeout::Listen& b)
{
    return a.timestamp == b.timestamp && a.title == b.title && a.artist == b.artist;
}
} // namespace

namespace Fooyin::Tapeout {
ListenQueue::ListenQueue(QString filepath, QObject* parent)
    : QObject{parent}
    , m_filepath{std::move(filepath)}
{
    read();
}

ListenQueue::~ListenQueue()
{
    if(m_dirty) {
        write();
    }
}

void ListenQueue::add(const Listen& listen)
{
    if(!listen.isValid()) {
        qCDebug(TAPEOUT) << "Refusing to queue a listen with no title or artist";
        return;
    }

    const bool known = std::ranges::any_of(m_listens, [&listen](const Listen& l) { return sameListen(l, listen); });
    if(known) {
        qCDebug(TAPEOUT) << "Listen already queued:" << listen.artist << "-" << listen.title;
        return;
    }

    m_listens.push_back(listen);
    scheduleWrite();
}

std::vector<Listen> ListenQueue::take(int max) const
{
    const auto count = std::min<size_t>(static_cast<size_t>(std::max(max, 0)), m_listens.size());
    return {m_listens.cbegin(), m_listens.cbegin() + static_cast<qsizetype>(count)};
}

void ListenQueue::remove(const std::vector<Listen>& listens)
{
    if(listens.empty()) {
        return;
    }

    const auto removed = std::erase_if(m_listens, [&listens](const Listen& queued) {
        return std::ranges::any_of(listens, [&queued](const Listen& l) { return sameListen(l, queued); });
    });

    if(removed > 0) {
        scheduleWrite();
    }
}

int ListenQueue::count() const
{
    return static_cast<int>(m_listens.size());
}

bool ListenQueue::isEmpty() const
{
    return m_listens.empty();
}

void ListenQueue::read()
{
    m_listens.clear();

    QFile file{m_filepath};
    if(!file.exists()) {
        return;
    }
    if(!file.open(QIODevice::ReadOnly)) {
        qCWarning(TAPEOUT) << "Could not open the listen queue:" << m_filepath;
        return;
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    file.close();

    if(error.error != QJsonParseError::NoError) {
        // A corrupt queue must not take the plugin down with it, but it also
        // must not be silently deleted — the file is left alone so it can be
        // recovered by hand.
        qCWarning(TAPEOUT) << "The listen queue is unreadable and will be ignored:" << error.errorString();
        return;
    }

    const QJsonArray array = doc.isArray() ? doc.array() : doc.object().value("listens"_L1).toArray();
    for(const auto& value : array) {
        Listen listen = Listen::deserialise(value.toObject());
        if(listen.isValid()) {
            m_listens.push_back(std::move(listen));
        }
    }

    // Oldest first, so a flush submits in the order things were played.
    std::ranges::sort(m_listens, [](const Listen& a, const Listen& b) { return a.timestamp < b.timestamp; });

    qCInfo(TAPEOUT) << "Restored" << m_listens.size() << "queued listens";
}

void ListenQueue::write()
{
    m_writeTimer.stop();
    m_dirty = false;

    const QFileInfo info{m_filepath};
    if(!info.dir().exists()) {
        QDir{}.mkpath(info.absolutePath());
    }

    QJsonArray array;
    for(const Listen& listen : m_listens) {
        array.append(listen.serialise());
    }

    QJsonObject root;
    root.insert("version"_L1, 1);
    root.insert("listens"_L1, array);

    // Written atomically: a queue truncated by a crash mid-write would lose
    // every listen it was holding.
    QSaveFile file{m_filepath};
    if(!file.open(QIODevice::WriteOnly)) {
        qCWarning(TAPEOUT) << "Could not write the listen queue:" << m_filepath;
        return;
    }

    file.write(QJsonDocument{root}.toJson(QJsonDocument::Compact));
    if(!file.commit()) {
        qCWarning(TAPEOUT) << "Could not commit the listen queue:" << m_filepath;
    }
}

void ListenQueue::scheduleWrite()
{
    m_dirty = true;
    if(!m_writeTimer.isActive()) {
        m_writeTimer.start(WriteInterval, this);
    }
}

void ListenQueue::timerEvent(QTimerEvent* event)
{
    if(event->timerId() == m_writeTimer.timerId()) {
        write();
        return;
    }
    QObject::timerEvent(event);
}
} // namespace Fooyin::Tapeout
