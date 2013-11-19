/*
 * Copyright (C) 2011 Canonical, Ltd.
 *
 * Authors:
 *  Michal Hruby <michal.hruby@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Self
#include "scopes.h"

// Local
#include "scope.h"

// Qt
#include <QDebug>
#include <QTimer>

namespace scopes_ng
{

Scopes::Scopes(QObject *parent)
    : QAbstractListModel(parent)
    , m_loaded(false)
{
    m_roles[Scopes::RoleScope] = "scope";
    m_roles[Scopes::RoleId] = "id";
    m_roles[Scopes::RoleVisible] = "visible";

    // simulate a bit of asynchrocinity, might not be needed
    QTimer::singleShot(1, this, SLOT(populateScopes()));
}

QHash<int, QByteArray> Scopes::roleNames() const
{
    return m_roles;
}

int Scopes::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent)

    return m_scopes.count();
}

void Scopes::populateScopes()
{
    beginResetModel();

    // FIXME: a little bit of hardcoded data for now
    auto scope = new Scope(this);
    m_scopes.append(scope);

    endResetModel();
}

QVariant Scopes::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    Scope* scope = m_scopes.at(index.row());

    switch (role) {
        case Scopes::RoleScope:
            return QVariant::fromValue(scope);
        case Scopes::RoleId:
            return QVariant::fromValue(scope->id());
        case Scopes::RoleVisible:
            return QVariant::fromValue(scope->visible());
        default:
            return QVariant();
    }
}

QVariant Scopes::get(int row) const
{
    return data(QAbstractListModel::index(row), 0);
}

QVariant Scopes::get(const QString& scope_id) const
{
    Q_FOREACH(Scope* scope, m_scopes) {
        if (scope->id() == scope_id) {
            return QVariant::fromValue(scope);
        }
    }

    return QVariant();
}

bool Scopes::loaded() const
{
    return m_loaded;
}

} // namespace scopes_ng