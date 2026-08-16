/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#pragma once

#include "listen.h"

#include <QBasicTimer>
#include <QObject>

#include <vector>

namespace Fooyin::Tapeout {
/*!
 * Listens waiting to reach Tapedeck, held on disk so a laptop closed on the
 * train still scrobbles when it gets home.
 *
 * Retrying is safe by design: Tapedeck dedups on `(user_id, source_id,
 * source_name)` and again on a fuzzy window over title and timestamp, so a
 * listen sent twice is recognised rather than duplicated. That is what lets this
 * hold a listen until it is *acknowledged* instead of hoping the first send
 * landed.
 */
class ListenQueue : public QObject
{
    Q_OBJECT

public:
    explicit ListenQueue(QString filepath, QObject* parent = nullptr);
    ~ListenQueue() override;

    void add(const Listen& listen);

    //! Oldest first, capped at @p max — Tapedeck refuses batches over 1000.
    [[nodiscard]] std::vector<Listen> take(int max) const;

    //! Drops listens Tapedeck has acknowledged, whether stored or deduplicated.
    void remove(const std::vector<Listen>& listens);

    [[nodiscard]] int count() const;
    [[nodiscard]] bool isEmpty() const;

    void read();
    void write();

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    void scheduleWrite();

    QString m_filepath;
    std::vector<Listen> m_listens;
    QBasicTimer m_writeTimer;
    bool m_dirty{false};
};
} // namespace Fooyin::Tapeout
