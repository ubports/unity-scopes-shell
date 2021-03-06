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
#include "overviewscope.h"
#include "ubuntulocationservice.h"
#include "favorites.h"

// Qt
#include <QDebug>
#include <QGSettings>
#include <QTimer>
#include <QDBusConnection>
#include <QProcess>
#include <QFile>
#include <QUrlQuery>
#include <QTextStream>

#include <unity/scopes/Registry.h>
#include <unity/scopes/Scope.h>
#include <unity/scopes/ScopeProxyFwd.h>
#include <unity/UnityExceptions.h>

namespace scopes_ng
{

using namespace unity;

#define SCOPES_SCOPE_ID "scopes"
#define PARTNER_ID_FILE "/custom/partner-id"
#define CLICK_SCOPE_ID "clickscope"

void ScopeListWorker::run()
{
    try
    {
        // m_runtimeConfig should be null in most cases, and empty string is for system-wide fallback
        if (!m_scopesRuntime) {
            scopes::Runtime::UPtr runtime_uptr = scopes::Runtime::create(m_runtimeConfig.toStdString());
            m_scopesRuntime = std::move(runtime_uptr);
        }
        auto registry = m_scopesRuntime->registry();
        m_metadataMap = registry->list();
    }
    catch (std::exception const& err)
    {
        qWarning("ERROR! Caught %s", err.what());
    }
    Q_EMIT discoveryFinished();
}

void ScopeListWorker::setRuntimeConfig(QString const& config)
{
    m_runtimeConfig = config;
}

void ScopeListWorker::setRuntime(scopes::Runtime::SPtr const& runtime)
{
    m_scopesRuntime = runtime;
}

scopes::Runtime::SPtr ScopeListWorker::getRuntime() const
{
    return m_scopesRuntime;
}

scopes::MetadataMap ScopeListWorker::metadataMap() const
{
    return m_metadataMap;
}

int Scopes::LIST_DELAY = -1;
const int Scopes::SCOPE_DELETE_DELAY = 3;
const int LOCATION_STARTUP_TIMEOUT = 1000;

class Scopes::Priv : public QObject {
    Q_OBJECT
Q_SIGNALS:
    void safeInvalidateScopeResults(const QString& scopeName);
public:
    std::unique_ptr<core::ScopedConnection> m_list_update_callback_connection_;
};

Scopes::Scopes(QObject *parent)
    : unity::shell::scopes::ScopesInterface(parent)
    , m_noFavorites(false)
    , m_overviewScope(nullptr)
    , m_listThread(nullptr)
    , m_loaded(false)
    , m_prepopulateFirstScope(true)
    , m_locationAccessHelper(new LocationAccessHelper(nullptr))
    , m_priv(new Priv())
{
    QByteArray noFav = qgetenv("UNITY_SCOPES_NO_FAVORITES");
    if (!noFav.isNull()) {
        m_noFavorites = true;
    }

    QByteArray noPrep = qgetenv("UNITY_SCOPES_NO_PREPOPULATE_FIRST");
    if (!noPrep.isNull()) {
        m_prepopulateFirstScope = false;
    }

    connect(m_priv.get(), SIGNAL(safeInvalidateScopeResults(const QString&)), this,
            SLOT(invalidateScopeResults(const QString &)), Qt::QueuedConnection);

    QDBusConnection::sessionBus().connect(QString(), QStringLiteral("/com/canonical/unity/scopes"), QStringLiteral("com.canonical.unity.scopes"), QStringLiteral("InvalidateResults"), this, SLOT(invalidateScopeResults(QString)));

    m_dashSettings = QGSettings::isSchemaInstalled("com.canonical.Unity.Dash") ? new QGSettings("com.canonical.Unity.Dash", QByteArray(), this) : nullptr;
    m_favoriteScopes = new Favorites(this, m_dashSettings);
    QObject::connect(m_favoriteScopes, &Favorites::favoritesChanged, this, &Scopes::favoritesChanged);

    m_overviewScope = OverviewScope::newInstance(this);

    m_registryRefreshTimer.setSingleShot(true);
    connect(&m_registryRefreshTimer, SIGNAL(timeout()), this, SLOT(scopeRegistryChanged()));

    m_locationService.reset(new UbuntuLocationService());
    QObject::connect(m_locationAccessHelper.data(), &LocationAccessHelper::requestInitialLocation, m_locationService.data(), &UbuntuLocationService::requestInitialLocation);
    QObject::connect(m_locationService.data(), &UbuntuLocationService::accessDenied, m_locationAccessHelper.data(), &LocationAccessHelper::accessDenied);
    QObject::connect(m_locationService.data(), &UbuntuLocationService::locationChanged, m_locationAccessHelper.data(), &LocationAccessHelper::positionChanged);
    QObject::connect(m_locationService.data(), &UbuntuLocationService::geoIpLookupFinished, m_locationAccessHelper.data(), &LocationAccessHelper::geoIpLookupFinished);
    m_locationAccessHelper->init();

    createUserAgentString();

    m_scopesToDeleteTimer.setSingleShot(true);
    m_scopesToDeleteTimer.setInterval(1000 * SCOPE_DELETE_DELAY);
    connect(&m_scopesToDeleteTimer, SIGNAL(timeout()), SLOT(purgeScopesToDelete()));
}

Scopes::~Scopes()
{
    if (m_listThread && !m_listThread->isFinished()) {
        // libunity-scopes supports timeouts, so this shouldn't block forever
        m_listThread->wait();
    }
}

QString Scopes::userAgentString() const
{
    return m_userAgent;
}

void Scopes::purgeScopesToDelete()
{
    m_scopesToDelete.clear();
}

QSharedPointer<LocationAccessHelper> Scopes::locationAccessHelper() const
{
    return m_locationAccessHelper;
}

int Scopes::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent)

    return m_scopes.count();
}

int Scopes::count() const
{
    return m_scopes.count();
}

void Scopes::createUserAgentString()
{
    QProcess *lsb_release = new QProcess(this);
    connect(lsb_release, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(lsbReleaseFinished()));
    connect(lsb_release, SIGNAL(error(QProcess::ProcessError)), this, SLOT(initPopulateScopes()));
    lsb_release->start(QStringLiteral("lsb_release -r"), QIODevice::ReadOnly);
}

void Scopes::lsbReleaseFinished()
{
    QProcess *lsb_release = qobject_cast<QProcess *>(sender());
    if (lsb_release) {
        const QString out = lsb_release->readAllStandardOutput();
        const QStringList parts = out.split(QRegExp(QStringLiteral("\\s+")), QString::SkipEmptyParts);
        if (parts.size() == 2) {
            m_versions.push_back(qMakePair(QStringLiteral("release"), parts.at(1)));
        }
        lsb_release->deleteLater();
    }

    // map package version to a simple name we send to SSS
    const QMap<QString, QString> versions({
        {QStringLiteral("unity-plugin-scopes"), QStringLiteral("plugin")},
        {QStringLiteral("unity8"), QStringLiteral("unity8")},
        {QStringLiteral("libunity-scopes"), QStringLiteral("scopes-api")}});

    // determine versions of unity8, unity-plugin-scopes and libunity-scopes1.0
    for (QMap<QString, QString>::const_iterator pkg = versions.constBegin(); pkg != versions.constEnd(); pkg++) {
        QFile versionFile("/var/lib/" + pkg.key() + "/version");
        if (versionFile.open(QIODevice::ReadOnly)) {
            QTextStream str(&versionFile);
            QString ver;
            str >> ver;
            m_versions.push_back(qMakePair(pkg.value(), ver));
        } else {
            qWarning() << "Couldn't determine the version of" << pkg.key();
        }
    }

    QFile buildFile(QStringLiteral("/etc/ubuntu-build"));
    if (buildFile.open(QIODevice::ReadOnly)) {
        QTextStream str(&buildFile);
        QString bld;
        str >> bld;
        m_versions.push_back(qMakePair(QStringLiteral("build"), bld));
    }

    const QString partnerId = readPartnerId();
    if (!partnerId.isEmpty()) {
        m_versions.push_back(qMakePair(QStringLiteral("partner"), partnerId));
    }

    QUrlQuery q;
    q.setQueryItems(m_versions);
    m_versions.clear();
    m_userAgent = q.toString();

    qDebug() << "User agent string:" << m_userAgent;
    initPopulateScopes();
}

QString Scopes::readPartnerId()
{
    // read /custom/partner-id value if present
    QString partnerId;
    QFile partnerIdFile(QStringLiteral(PARTNER_ID_FILE));
    if (partnerIdFile.exists())
    {
        if (partnerIdFile.open(QIODevice::ReadOnly))
        {
            QTextStream str(&partnerIdFile);
            partnerId = str.readLine();
        }
        else
        {
            qWarning() << "Cannot open" << QStringLiteral(PARTNER_ID_FILE) << "for reading";
        }
    }
    return partnerId;
}

void Scopes::initPopulateScopes()
{
    // initiate scopes
    // delaying spawning the worker thread, causes problems with qmlplugindump
    // without it
    if (LIST_DELAY < 0) {
        QByteArray listDelay = qgetenv("UNITY_SCOPES_LIST_DELAY");
        LIST_DELAY = listDelay.isNull() ? 100 : listDelay.toInt();
    }
    QTimer::singleShot(LIST_DELAY, this, SLOT(populateScopes()));
}

// *N.B.* populateScopes() is intended for use only on start-up!
// In any other circumstance, use refreshScopeMetadata() to invalidate results.
void Scopes::populateScopes()
{
    auto thread = new ScopeListWorker;
    QByteArray runtimeConfig = qgetenv("UNITY_SCOPES_RUNTIME_PATH");
    thread->setRuntimeConfig(QString::fromLocal8Bit(runtimeConfig));
    QObject::connect(thread, &ScopeListWorker::discoveryFinished, this, &Scopes::discoveryFinished);
    QObject::connect(thread, &ScopeListWorker::finished, thread, &QObject::deleteLater);

    m_listThread = thread;
    m_listThread->start();
}

void Scopes::discoveryFinished()
{
    qDebug() << "Scopes discovery finished";

    ScopeListWorker* thread = qobject_cast<ScopeListWorker*>(sender());

    m_scopesRuntime = thread->getRuntime();

    if (!m_scopesRuntime) {
        // This signifies a serious problem, such as a broken locale setup. We cannot recover from that.
        // Return early so that scope objects are not created etc. The Dash will remain blank.
        qWarning() << "FATAL ERROR! Scopes runtime couldn't be initialized. Please check your system settings and locale data/setup. The Dash will remain empty until the underlying problem is fixed.";
        m_loaded = true;
        Q_EMIT loadedChanged();
        return;
    }

    auto scopes = thread->metadataMap();
    m_priv->m_list_update_callback_connection_.reset(
            new core::ScopedConnection(
                    m_scopesRuntime->registry()->set_list_update_callback(
                            std::bind(&Scopes::Priv::safeInvalidateScopeResults,
                                      m_priv.get(), SCOPES_SCOPE_ID))));

    beginResetModel();

    if (m_noFavorites) {
        // add all visible scopes
        for (auto it = scopes.begin(); it != scopes.end(); ++it) {
            if (!it->second.invisible()) {
                Scope::Ptr scope = Scope::newInstance(this);
                connect(scope.data(), SIGNAL(isActiveChanged()), this, SLOT(prepopulateNextScopes()));
                scope->setScopeData(it->second);
                m_scopes.append(scope);
            }
        }
    }

    // cache all the metadata
    m_cachedMetadata.clear();
    for (auto it = scopes.begin(); it != scopes.end(); ++it) {
        m_cachedMetadata[QString::fromStdString(it->first)] = std::make_shared<unity::scopes::ScopeMetadata>(it->second);
    }

    if (m_locationService->hasLocation() || qEnvironmentVariableIsSet("UNITY_SCOPES_NO_WAIT_LOCATION"))
    {
        // If we already have a location just query the scopes now
        completeDiscoveryFinished();
    }
    else
    {
        qDebug() << "Waiting for initial location update";

        // Otherwise we have to wait for location data
        // Either the the location data needs to change, or the timeout happens
        connect(m_locationService.data(), &UbuntuLocationService::locationChanged,
                this, &Scopes::completeDiscoveryFinished);
        connect(m_locationService.data(), &UbuntuLocationService::accessDenied,
                this, &Scopes::completeDiscoveryFinished);
        connect(m_locationService.data(), &UbuntuLocationService::locationTimeout,
                this, &Scopes::completeDiscoveryFinished);
        connect(&m_startupQueryTimeout, &QTimer::timeout, this,
                &Scopes::completeDiscoveryFinished);
        m_startupQueryTimeout.setSingleShot(true);
        m_startupQueryTimeout.setInterval(LOCATION_STARTUP_TIMEOUT);
        m_startupQueryTimeout.start();
    }
}

void Scopes::completeDiscoveryFinished()
{
    qDebug() << "Scopes discovery completed";
    // Kill off everything that could potentially trigger the startup queries
    m_startupQueryTimeout.stop();
    disconnect(&m_startupQueryTimeout, &QTimer::timeout, this,
               &Scopes::completeDiscoveryFinished);
    disconnect(m_locationService.data(), &UbuntuLocationService::locationChanged,
               this, &Scopes::completeDiscoveryFinished);
    disconnect(m_locationService.data(), &UbuntuLocationService::accessDenied,
               this, &Scopes::completeDiscoveryFinished);
    disconnect(m_locationService.data(), &UbuntuLocationService::locationTimeout,
               this, &Scopes::completeDiscoveryFinished);

    processFavoriteScopes();
    endResetModel();

    m_loaded = true;
    Q_EMIT loadedChanged();
    Q_EMIT countChanged();
    Q_EMIT overviewScopeChanged();
    Q_EMIT metadataRefreshed();

    m_listThread = nullptr;

    if (m_prepopulateFirstScope) {
        m_prepopulateFirstScope = false;
        prepopulateFirstScope();
    }
}

void Scopes::prepopulateFirstScope()
{
    if (!m_scopes.isEmpty()) {
        auto& scope = m_scopes.front();
        if (!scope->initialQueryDone()) {
            qDebug() << "Pre-populating first scope:" << scope->id();
            scope->setSearchQuery(QLatin1String(""));
            // must dispatch search explicitly since setSearchQuery will not do that for inactive scope
            scope->dispatchSearch(true);
        }
    }
}

void Scopes::prepopulateNextScopes()
{
    for (auto it = m_scopes.begin(); it != m_scopes.end(); it++) {
        // query next two scopes following currently active scope
        if ((*it)->isActive()) {
            ++it;
            for (int i = 0; i<2 && it != m_scopes.end(); i++) {
                auto scope = *(it++);
                if (!scope->initialQueryDone()) {
                    qDebug() << "Pre-populating scope" << scope->id();
                    scope->setSearchQuery(QLatin1String(""));
                    // must dispatch search explicitly since setSearchQuery will not do that for inactive scope
                    scope->dispatchSearch(true);
                }
            }
            break;
        }
    }
}

void Scopes::processFavoriteScopes()
{
    qDebug() << "Scopes::processFavoriteScopes()";

    if (m_noFavorites) {
        return;
    }

    //
    // create new Scope objects or remove existing according to the list of favorities.
    // notify about scopes model changes accordingly.
    if (m_dashSettings) {
        for (auto const& fv: m_favoriteScopes->getFavorites())
        {
            // favorited scope not installed?
            if (m_cachedMetadata.find(fv) == m_cachedMetadata.end())
            {
                qDebug() << "Favorited scope" << fv << "is no longer available, un-favoriting";
                m_favoriteScopes->setFavorite(fv, false);
            }
        }

        // special-case clickscope; append it to favorites if it was uninstalled (and in consequence removed from favorites) - see LP: #1603186
        if (m_cachedMetadata.contains(CLICK_SCOPE_ID) && !m_favoriteScopes->hasScope(CLICK_SCOPE_ID)) {
            qDebug() << "Favoriting" << CLICK_SCOPE_ID;
            m_favoriteScopes->setFavorite(CLICK_SCOPE_ID, true);
        }

        QSet<QString> scopesInTheModel;
        int row = 0;
        // remove un-favorited scopes
        for (auto it = m_scopes.begin(); it != m_scopes.end();)
        {
            if (!m_favoriteScopes->hasScope((*it)->id()))
            {
                qDebug() << "Scope" << (*it)->id() << "is no longer favorited, removing";

                beginRemoveRows(QModelIndex(), row, row);
                Scope::Ptr toDelete = *it;
                toDelete->setFavorite(false);
                // we need to delay actual deletion of Scope object so that shell can animate it
                m_scopesToDelete.push_back(toDelete);
                // if the timer is already active, we just wait a bit longer, which is no problem
                m_scopesToDeleteTimer.start();
                it = m_scopes.erase(it);
                endRemoveRows();
            }
            else
            {
                scopesInTheModel.insert((*it)->id());
                ++it;
                ++row;
            }
        }

        // add new favorites
        row = 0;
        for (auto const& fv: m_favoriteScopes->getFavorites())
        {
            if (!scopesInTheModel.contains(fv))
            {
                auto it = m_cachedMetadata.find(fv);
                if (it != m_cachedMetadata.end())
                {
                    qDebug() << "Scope" << fv << "is favorited, adding to scopes model";

                    Scope::Ptr scope = Scope::newInstance(this, true);
                    connect(scope.data(), SIGNAL(isActiveChanged()), this, SLOT(prepopulateNextScopes()));
                    scope->setScopeData(*(it.value()));
                    beginInsertRows(QModelIndex(), row, row);
                    m_scopes.insert(row, scope);
                    endInsertRows();
                }
            }
            ++row;
        }

        // iterate over results, move rows if positions changed
        for (int i = 0; i<m_scopes.size(); )
        {
            auto scope = m_scopes.at(i);
            const QString id = scope->id();
            int pos = m_favoriteScopes->position(id);
            if (pos >= 0) {
                if (pos != i) {
                    qDebug() << "Moving scope" << id << "to row" << pos << "to match position in favorites";
                    beginMoveRows(QModelIndex(), i, i, QModelIndex(), pos + (pos > i ? 1 : 0));
                    m_scopes.move(i, pos);
                    endMoveRows();
                    continue;
                }
            }
            i++;
        }
    }
}

void Scopes::favoritesChanged()
{
    processFavoriteScopes();

    if (m_overviewScope)
    {
        m_overviewScope->updateFavorites(m_favoriteScopes->getFavorites());
    }
}

void Scopes::refreshFinished()
{
    qDebug() << "Scopes registry refresh finished";

    ScopeListWorker* thread = qobject_cast<ScopeListWorker*>(sender());

    auto scopes = thread->metadataMap();

    // cache all the metadata
    m_cachedMetadata.clear();
    for (auto it = scopes.begin(); it != scopes.end(); ++it) {
        m_cachedMetadata[QString::fromStdString(it->first)] = std::make_shared<unity::scopes::ScopeMetadata>(it->second);
    }

    processFavoriteScopes();

    Q_EMIT metadataRefreshed();

    m_listThread = nullptr;
}

void Scopes::invalidateScopeResults(QString const& scopeName)
{
    // HACK! mediascanner invalidates local media scopes, but those are aggregated, so let's "forward" the call
    if (scopeName == QLatin1String("mediascanner-music")) {
        invalidateScopeResults(QStringLiteral("musicaggregator"));
    } else if (scopeName == QLatin1String("mediascanner-video")) {
        invalidateScopeResults(QStringLiteral("videoaggregator"));
    } else if (scopeName == QLatin1String(SCOPES_SCOPE_ID)) {
        // emitted when smart-scopes proxy or scope registry discovers new scopes
        m_registryRefreshTimer.start(5000);
        return;
    }

    auto scope = getScopeById(scopeName);
    if (scope == nullptr) {
        // check temporary scopes
        scope = qobject_cast<Scope*>(findTempScope(scopeName));
    }

    if (scope) {
        scope->invalidateResults();
    } else {
        qWarning() << "invalidateScopeResults: no such scope '" << scopeName << "'";
    }
}

void Scopes::scopeRegistryChanged()
{
    qDebug() << "Refreshing scope metadata";
    refreshScopeMetadata();
    Q_FOREACH(Scope::Ptr scope, m_scopes) {
        scope->invalidateChildScopes();
        scope->invalidateResults(true);
    }

    Q_FOREACH(Scope::Ptr scope, m_tempScopes) {
        scope->invalidateChildScopes();
        scope->invalidateResults(true);
    }
}

QVariant Scopes::data(const QModelIndex& index, int role) const
{
    int row = index.row();
    if (row >= m_scopes.size())
    {
        qWarning() << "Scopes::data - invalid index" << row << "size"
                << m_scopes.size();
        return QVariant();
    }

    Scope::Ptr scope = m_scopes.at(index.row());

    switch (role) {
        case Scopes::RoleScope:
            return QVariant::fromValue(scope.data());
        case Scopes::RoleId:
            return QString(scope->id());
        case Scopes::RoleTitle:
            return QString(scope->name());
        default:
            return QVariant();
    }
}

unity::shell::scopes::ScopeInterface* Scopes::getScope(int row) const
{
    return getScopeByRow(row).data();
}

Scope::Ptr Scopes::getScopeByRow(int row) const
{
    if (row >= m_scopes.size() || row < 0) {
        return Scope::Ptr();
    }
    return m_scopes[row];
}

unity::shell::scopes::ScopeInterface* Scopes::getScope(const QString& scopeId) const
{
    return getScopeById(scopeId).data();
}

Scope::Ptr Scopes::getScopeById(QString const& scopeId) const
{
    Q_FOREACH(Scope::Ptr scope, m_scopes) {
        if (scope->id() == scopeId) {
            return scope;
        }
    }

    return Scope::Ptr();
}

QStringList Scopes::getFavoriteIds() const
{
    return m_favoriteScopes->getFavorites();
}

void Scopes::setFavorite(QString const& scopeId, bool value)
{
    if (scopeId == QStringLiteral(CLICK_SCOPE_ID) && !value)
    {
        qWarning() << "Cannot unfavorite" << scopeId;
        return;
    }

    int row = m_favoriteScopes->setFavorite(scopeId, value);
    if (row >= 0)
    {
        if (value) {
            auto it = m_cachedMetadata.find(scopeId);
            if (it != m_cachedMetadata.end())
            {
                Scope::Ptr scope = Scope::newInstance(this, true);
                connect(scope.data(), SIGNAL(isActiveChanged()), this, SLOT(prepopulateNextScopes()));
                scope->setScopeData(*(it.value()));
                beginInsertRows(QModelIndex(), row, row);
                m_scopes.insert(row, scope);
                endInsertRows();
            } else {
                qWarning() << "setFavorite: unknown scope" << scopeId;
            }
        } else {
            for (auto it = m_scopes.begin(); it != m_scopes.end(); it++)
            {
                if ((*it)->id() == scopeId) {
                    beginRemoveRows(QModelIndex(), row, row);
                    Scope::Ptr toDelete = *it;
                    toDelete->setFavorite(false);
                    // we need to delay actual deletion of Scope object so that shell can animate it
                    m_scopesToDelete.push_back(toDelete);
                    // if the timer is already active, we just wait a bit longer, which is no problem
                    m_scopesToDeleteTimer.start();
                    it = m_scopes.erase(it);
                    endRemoveRows();
                    break;
                }
            }
        }
    }
}

void Scopes::addTempScope(Scope::Ptr const& scope)
{
    m_tempScopes.insert(scope->id(), scope);
}

void Scopes::closeScope(unity::shell::scopes::ScopeInterface* scope)
{
    m_tempScopes.remove(scope->id());
}

Scope::Ptr Scopes::findTempScope(QString const& id) const
{
    auto it = m_tempScopes.find(id);
    if (it != m_tempScopes.end())
    {
        return *it;
    }
    return Scope::Ptr();
}

void Scopes::moveFavoriteTo(QString const& scopeId, int index)
{
    int oldPos = m_favoriteScopes->position(scopeId);
    if (oldPos != index) {
        m_favoriteScopes->moveFavoriteTo(scopeId, index);
        beginMoveRows(QModelIndex(), oldPos, oldPos, QModelIndex(), index + (index > oldPos ? 1 : 0));
        m_scopes.move(oldPos, index);
        endMoveRows();
    }
}

QMap<QString, unity::scopes::ScopeMetadata::SPtr> Scopes::getAllMetadata() const
{
    return m_cachedMetadata;
}

scopes::ScopeMetadata::SPtr Scopes::getCachedMetadata(QString const& scopeId) const
{
    auto it = m_cachedMetadata.constFind(scopeId);
    if (it != m_cachedMetadata.constEnd()) {
        return it.value();
    }

    return scopes::ScopeMetadata::SPtr();
}

void Scopes::refreshScopeMetadata()
{
    // make sure there's just one listing in-progress at any given time
    if (m_listThread == nullptr && m_scopesRuntime) {
        auto thread = new ScopeListWorker;
        thread->setRuntime(m_scopesRuntime);
        QObject::connect(thread, &ScopeListWorker::discoveryFinished, this, &Scopes::refreshFinished);
        QObject::connect(thread, &ScopeListWorker::finished, thread, &QObject::deleteLater);

        m_listThread = thread;
        m_listThread->start();
    }
}

unity::shell::scopes::ScopeInterface* Scopes::overviewScope() const
{
    return overviewScopeSPtr().data();
}

Scope::Ptr Scopes::overviewScopeSPtr() const
{
    Scope::Ptr result;
    if (m_loaded)
    {
        result = m_overviewScope;
    }
    return result;
}

bool Scopes::loaded() const
{
    return m_loaded;
}

UbuntuLocationService::Ptr Scopes::locationService() const
{
    return m_locationService;
}

} // namespace scopes_ng

#include <scopes.moc>
