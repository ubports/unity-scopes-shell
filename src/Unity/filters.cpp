/*
 * Copyright (C) 2015 Canonical, Ltd.
 *
 * Authors:
 *  Pawel Stolowski <pawel.stolowski@canonical.com>
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
#include "filters.h"
#include "optionselectorfilter.h"
#include "rangeinputfilter.h"
#include "valuesliderfilter.h"
#include <QSet>
#include <QMap>
#include <QQmlEngine>
#include <QDebug>

#include <unity/scopes/OptionSelectorFilter.h>
#include <unity/scopes/RangeInputFilter.h>
#include <unity/scopes/ValueSliderFilter.h>

namespace scopes_ng
{

const int FILTER_CHANGE_PROCESSING_DELAY = 300;

Filters::Filters(unity::scopes::FilterState const& filterState,
        unity::shell::scopes::ScopeInterface *parent) : ModelUpdate(parent),
        m_filterState(new unity::scopes::FilterState(filterState))
{
    m_filterStateChangeTimer.setSingleShot(true);
    QObject::connect(&m_filterStateChangeTimer, &QTimer::timeout, this, &Filters::delayedFilterStateChange);
}

int Filters::rowCount(const QModelIndex&) const
{
    return m_filters.count();
}

QVariant Filters::data(const QModelIndex& index, int role) const
{
    if (index.row() >= m_filters.count())
    {
        return QVariant();
    }

    switch (role)
    {
        case unity::shell::scopes::FiltersInterface::Roles::RoleFilterId:
            return m_filters.at(index.row())->filterId();
        case Qt::DisplayRole:
        case unity::shell::scopes::FiltersInterface::Roles::RoleFilterType:
            return m_filters.at(index.row())->filterType();
        case unity::shell::scopes::FiltersInterface::Roles::RoleFilter:
            return QVariant::fromValue(m_filters.at(index.row()).data());
        default:
            break;
    };

    return QVariant();
}

void Filters::clear()
{
    if (m_filters.size() > 0)
    {
        beginResetModel();
        m_filters.clear();
        m_filterState.reset(new unity::scopes::FilterState());
        endResetModel();
    }
}

void Filters::reset()
{
    // Resetting filters will emit filterStateChanged.
    qDebug() << "Resetting filters to defaults";
    for (auto f: m_filters) {
        qDebug() << "Resetting filter:" << f->filterId();
        auto shellFilter = dynamic_cast<FilterUpdateInterface*>(f.data());
        Q_ASSERT(shellFilter);
        shellFilter->reset();
    }
}

void Filters::onFilterStateChanged()
{
    qDebug() << "Filter::onFilterStateChanged";
    m_filterStateChangeTimer.start(FILTER_CHANGE_PROCESSING_DELAY);
}

void Filters::delayedFilterStateChange()
{
    Q_EMIT filterStateChanged();
}

void Filters::update(QList<unity::scopes::FilterBase::SCPtr> const& filters, bool containsDepartments)
{
    // Primary filter needs to be handled separately and not inserted into the main filters model,
    bool hasPrimaryFilter = containsDepartments;
    QList<unity::scopes::FilterBase::SCPtr> inFilters;
    for (auto f: filters) {
        // we can only have one primary filter. only a single-selection OptionSelectorFilter can be a primary nav.
        bool wantsToBePrimary = (f->display_hints() & unity::scopes::FilterBase::DisplayHints::Primary);
        unity::scopes::OptionSelectorFilter::SCPtr optSelFilter = std::dynamic_pointer_cast<unity::scopes::OptionSelectorFilter const>(f);
        if (optSelFilter == nullptr || optSelFilter->multi_select()) {
            wantsToBePrimary = false;
        }
        if (wantsToBePrimary && !hasPrimaryFilter) {
            hasPrimaryFilter = true;
            //
            const bool hadSamePrimaryFilterBefore = m_primaryFilter && (m_primaryFilter->filterId() == QString::fromStdString(f->id())) && (m_primaryFilter->filterType() == getFilterType(f));
            if (hadSamePrimaryFilterBefore) {
                auto shellFilter = dynamic_cast<FilterUpdateInterface*>(m_primaryFilter.data());
                if (shellFilter) {
                    shellFilter->update(f);
                } else {
                    // this should never happen
                    qCritical() << "Failed to cast filter" << m_primaryFilter->filterId() << "to FilterUpdateInterface";
                }
            } else {
                // we didn't have primary filter before, or it changed substantially, so recreate it
                m_primaryFilter = createFilterObject(f);
                Q_EMIT primaryFilterChanged();
            }
        } else {
            inFilters.append(f);
        }
    }

    // Did we have primary filter before but not now?
    if (!hasPrimaryFilter && m_primaryFilter != nullptr) {
        m_primaryFilter.reset();
        Q_EMIT primaryFilterChanged();
    }

    syncModel(inFilters, m_filters,
            // key function for scopes api filter
            [](const unity::scopes::FilterBase::SCPtr& f) -> QString { return QString::fromStdString(f->id()); },
            // key function for shell api filter
            [](const QSharedPointer<unity::shell::scopes::FilterBaseInterface>& f) -> QString { return f->filterId(); },
            // factory function
            [this](const unity::scopes::FilterBase::SCPtr& f) -> QSharedPointer<unity::shell::scopes::FilterBaseInterface> {
                return createFilterObject(f);
                },
            // filter update function
            [this](int, const unity::scopes::FilterBase::SCPtr &f1, const QSharedPointer<unity::shell::scopes::FilterBaseInterface>& f2) -> bool {
                qDebug() << "Updating filter" << f2->filterId();
                if (f2->filterId() != QString::fromStdString(f1->id()) || f2->filterType() != getFilterType(f1))
                {
                    return false;
                }
                auto shellFilter = dynamic_cast<FilterUpdateInterface*>(f2.data());
                if (shellFilter) {
                    shellFilter->update(f1);
                } else {
                    // this should never happen
                    qCritical() << "Failed to cast filter" << f2->filterId() << "to FilterUpdateInterface";
                }
                return true;
            });
}
//
// Update current filters model and primary filter pointer with filters coming from scope.
void Filters::update(unity::scopes::FilterState const& filterState)
{
    m_filterState.reset(new unity::scopes::FilterState(filterState));

    if (m_primaryFilter) {
        auto shellFilter = dynamic_cast<FilterUpdateInterface*>(m_primaryFilter.data());
        if (shellFilter) {
            shellFilter->update(m_filterState);
        } else {
            // this should never happen
            qCritical() << "Failed to cast filter" << m_primaryFilter->filterId() << "to FilterUpdateInterface";
        }
    }
    for (auto f: m_filters) {
        auto shellFilter = dynamic_cast<FilterUpdateInterface*>(f.data());
        if (shellFilter) {
            shellFilter->update(m_filterState);
        } else {
            // this should never happen
            qCritical() << "Failed to cast filter" << f->filterId() << "to FilterUpdateInterface";
        }
    }
}

QSharedPointer<unity::shell::scopes::FilterBaseInterface> Filters::createFilterObject(unity::scopes::FilterBase::SCPtr const& filter)
{
    QSharedPointer<unity::shell::scopes::FilterBaseInterface> filterObj;
    if (filter->filter_type() == "option_selector")
    {
        unity::scopes::OptionSelectorFilter::SCPtr optfilter = std::dynamic_pointer_cast<unity::scopes::OptionSelectorFilter const>(filter);
        filterObj = QSharedPointer<unity::shell::scopes::FilterBaseInterface>(new scopes_ng::OptionSelectorFilter(optfilter, m_filterState, this));
    }
    else if (filter->filter_type() == "range_input")
    {
        unity::scopes::RangeInputFilter::SCPtr rangefilter = std::dynamic_pointer_cast<unity::scopes::RangeInputFilter const>(filter);
        filterObj = QSharedPointer<unity::shell::scopes::FilterBaseInterface>(new scopes_ng::RangeInputFilter(rangefilter, m_filterState, this));
    }
    else if (filter->filter_type() == "value_slider")
    {
        unity::scopes::ValueSliderFilter::SCPtr valuesliderfilter = std::dynamic_pointer_cast<unity::scopes::ValueSliderFilter const>(filter);
        filterObj = QSharedPointer<unity::shell::scopes::FilterBaseInterface>(new scopes_ng::ValueSliderFilter(valuesliderfilter, m_filterState, this));
    }

    // Warning!!! Make sure any new filter type created by this factory method is reflected below in getFilterType() method as well!

    if (filterObj)
    {
        QQmlEngine::setObjectOwnership(filterObj.data(), QQmlEngine::CppOwnership);
        connect(filterObj.data(), SIGNAL(filterStateChanged()), this, SLOT(onFilterStateChanged()));
    }
    else
    {
        qWarning() << "Unsupported filter type:" << QString::fromStdString(filter->filter_type());
    }

    return filterObj;
}

unity::shell::scopes::FiltersInterface::FilterType Filters::getFilterType(unity::scopes::FilterBase::SCPtr const& filter)
{
    // WARNING! Some filters may rely on inheritance, in that case the dynamic casts below need to be in correct order
    // (e.g. more concrete types first, base/generic types last).
    if (std::dynamic_pointer_cast<unity::scopes::OptionSelectorFilter const>(filter))
    {
        return unity::shell::scopes::FiltersInterface::FilterType::OptionSelectorFilter;
    }
    if (std::dynamic_pointer_cast<unity::scopes::RangeInputFilter const>(filter))
    {
        return unity::shell::scopes::FiltersInterface::FilterType::RangeInputFilter;
    }
    if (std::dynamic_pointer_cast<unity::scopes::ValueSliderFilter const>(filter))
    {
        return unity::shell::scopes::FiltersInterface::FilterType::ValueSliderFilter;
    }
    qFatal("getFilterType(): Unknown filter type: %s\n", filter->filter_type().c_str());
    return unity::shell::scopes::FiltersInterface::FilterType::Invalid;
}

unity::scopes::FilterState Filters::filterState() const
{
    if (m_filterState) {
        return *m_filterState;
    }
    return unity::scopes::FilterState();
}

QSharedPointer<unity::shell::scopes::FilterBaseInterface> Filters::primaryFilter() const
{
    return m_primaryFilter;
}

int Filters::activeFiltersCount() const
{
    int count = 0;
    for (auto filter: m_filters) {
        auto shellFilter = dynamic_cast<FilterUpdateInterface*>(filter.data());
        if (shellFilter->isActive()) {
            qDebug() << "activeFiltersCount: filter" << filter->filterId() << "is active";
            ++count;
        }
    }
    return count;
}

}