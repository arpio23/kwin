/*
    SPDX-FileCopyrightText: 2023 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "wayland/transaction.h"
#include "utils/filedescriptor.h"
#include "wayland/subcompositor_interface.h"
#include "wayland/surface_interface_p.h"
#include "wayland/transaction_p.h"

using namespace KWin;

namespace KWaylandServer
{

TransactionDmaBufLocker *TransactionDmaBufLocker::get(GraphicsBuffer *buffer)
{
    static QHash<GraphicsBuffer *, TransactionDmaBufLocker *> lockers;
    if (auto it = lockers.find(buffer); it != lockers.end()) {
        return *it;
    }

    const DmaBufAttributes *attributes = buffer->dmabufAttributes();
    if (!attributes) {
        return nullptr;
    }

    auto locker = new TransactionDmaBufLocker(attributes);
    lockers[buffer] = locker;
    QObject::connect(buffer, &QObject::destroyed, [buffer]() {
        delete lockers.take(buffer);
    });

    return locker;
}

TransactionDmaBufLocker::TransactionDmaBufLocker(const DmaBufAttributes *attributes)
{
    for (int i = 0; i < attributes->planeCount; ++i) {
        auto notifier = new QSocketNotifier(attributes->fd[i].get(), QSocketNotifier::Read);
        notifier->setEnabled(false);
        connect(notifier, &QSocketNotifier::activated, this, [this, notifier]() {
            notifier->setEnabled(false);
            m_pending.removeOne(notifier);
            if (m_pending.isEmpty()) {
                const auto transactions = m_transactions; // unlock() may destroy this
                m_transactions.clear();
                for (Transaction *transition : transactions) {
                    transition->unlock();
                }
            }
        });
        m_notifiers.emplace_back(notifier);
    }
}

void TransactionDmaBufLocker::add(Transaction *transition)
{
    if (arm()) {
        transition->lock();
        m_transactions.append(transition);
    }
}

bool TransactionDmaBufLocker::arm()
{
    if (!m_pending.isEmpty()) {
        return true;
    }
    for (const auto &notifier : m_notifiers) {
        if (!FileDescriptor::isReadable(notifier->socket())) {
            notifier->setEnabled(true);
            m_pending.append(notifier.get());
        }
    }
    return !m_pending.isEmpty();
}

Transaction::Transaction()
{
}

void Transaction::lock()
{
    m_locks++;
}

void Transaction::unlock()
{
    Q_ASSERT(m_locks > 0);
    m_locks--;
    if (m_locks == 0) {
        tryApply();
    }
}

bool Transaction::isReady() const
{
    if (m_locks) {
        return false;
    }

    for (const TransactionEntry &entry : m_entries) {
        if (!entry.surface) {
            continue;
        }
        if (entry.surface->firstTransaction() != this) {
            return false;
        }
    }

    return true;
}

Transaction *Transaction::next(SurfaceInterface *surface) const
{
    for (const TransactionEntry &entry : m_entries) {
        if (entry.surface == surface) {
            return entry.nextTransaction;
        }
    }
    return nullptr;
}

void Transaction::add(SurfaceInterface *surface)
{
    SurfaceState *pending = SurfaceInterfacePrivate::get(surface)->pending.get();

    for (TransactionEntry &entry : m_entries) {
        if (entry.surface == surface) {
            if (pending->bufferIsSet) {
                entry.buffer = GraphicsBufferRef(pending->buffer);
            }
            pending->mergeInto(entry.state.get());
            return;
        }
    }

    auto state = std::make_unique<SurfaceState>();
    pending->mergeInto(state.get());

    m_entries.emplace_back(TransactionEntry{
        .surface = surface,
        .buffer = GraphicsBufferRef(state->buffer),
        .state = std::move(state),
    });
}

void Transaction::amend(SurfaceInterface *surface, std::function<void(SurfaceState *)> mutator)
{
    for (TransactionEntry &entry : m_entries) {
        if (entry.surface == surface) {
            mutator(entry.state.get());
        }
    }
}

void Transaction::merge(Transaction *other)
{
    for (size_t i = 0; i < other->m_entries.size(); ++i) {
        m_entries.emplace_back(std::move(other->m_entries[i]));
    }
    other->m_entries.clear();
}

static bool isAncestor(SurfaceInterface *surface, SurfaceInterface *ancestor)
{
    SurfaceInterface *candidate = surface;
    while (candidate) {
        SubSurfaceInterface *subsurface = candidate->subSurface();
        if (!subsurface) {
            return false;
        }

        if (subsurface->parentSurface() == ancestor) {
            return true;
        }

        candidate = subsurface->parentSurface();
    }

    return false;
}

static SurfaceInterface *mainSurface(SurfaceInterface *surface)
{
    SubSurfaceInterface *subsurface = surface->subSurface();
    if (subsurface) {
        return subsurface->mainSurface();
    }
    return surface;
}

void Transaction::apply()
{
    // Sort surfaces so descendants come first, then their ancestors.
    std::sort(m_entries.begin(), m_entries.end(), [](const TransactionEntry &a, const TransactionEntry &b) {
        if (isAncestor(a.surface, b.surface)) {
            return true;
        }
        if (isAncestor(b.surface, a.surface)) {
            return false;
        }
        return mainSurface(a.surface) < mainSurface(b.surface);
    });

    for (TransactionEntry &entry : m_entries) {
        if (entry.surface) {
            SurfaceInterfacePrivate::get(entry.surface)->applyState(entry.state.get());
        }
    }

    for (TransactionEntry &entry : m_entries) {
        if (entry.surface) {
            if (entry.surface->lastTransaction() == this) {
                entry.surface->setFirstTransaction(nullptr);
                entry.surface->setLastTransaction(nullptr);
            } else {
                entry.surface->setFirstTransaction(entry.nextTransaction);
            }
        }

        if (entry.nextTransaction) {
            entry.nextTransaction->tryApply();
        }
    }

    delete this;
}

bool Transaction::tryApply()
{
    if (!isReady()) {
        return false;
    }
    apply();
    return true;
}

void Transaction::commit()
{
    for (TransactionEntry &entry : m_entries) {
        if (entry.state->bufferIsSet && entry.state->buffer) {
            // Avoid applying the transaction until all graphics buffers have become idle.
            if (auto locker = TransactionDmaBufLocker::get(entry.state->buffer)) {
                locker->add(this);
            }
        }

        if (entry.surface->firstTransaction()) {
            Transaction *lastTransaction = entry.surface->lastTransaction();
            for (TransactionEntry &lastEntry : lastTransaction->m_entries) {
                if (lastEntry.surface == entry.surface) {
                    lastEntry.nextTransaction = this;
                }
            }
        } else {
            entry.surface->setFirstTransaction(this);
        }

        entry.surface->setLastTransaction(this);
    }

    if (!tryApply()) {
        for (const TransactionEntry &entry : m_entries) {
            Q_EMIT entry.surface->stateStashed(entry.state->serial);
        }
    }
}

} // namespace KWaylandServer

#include "moc_transaction.cpp"