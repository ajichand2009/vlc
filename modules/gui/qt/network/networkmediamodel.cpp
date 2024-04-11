/*****************************************************************************
 * Copyright (C) 2019 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "networkmediamodel.hpp"
#include "mediatreelistener.hpp"

#include "maininterface/mainctx.hpp"

#include "util/locallistbasemodel.hpp"

#include "playlist/media.hpp"
#include "playlist/playlist_controller.hpp"

#include <unordered_set>

#include <QSemaphore>
#include <QDateTime>
#include <QTimeZone>


namespace {

//use the same queue as in mlfoldermodel
static const char* const ML_FOLDER_ADD_QUEUE = "ML_FOLDER_ADD_QUEUE";

struct NetworkMediaItem
{
    QString name;
    QString uri;
    QUrl mainMrl;
    QString protocol;
    bool indexed;
    NetworkMediaModel::ItemType type;
    bool canBeIndexed;
    NetworkTreeItem tree;
    QString artwork;
    qint64 fileSize;
    QDateTime fileModified;
};

using NetworkMediaItemPtr = std::shared_ptr<NetworkMediaItem>;
using NetworkMediaItemSet = std::unordered_set<NetworkMediaItemPtr>;

inline bool isADir(const NetworkMediaItemPtr& x)
{
    return (x->type == NetworkMediaModel::ItemType::TYPE_DIRECTORY);
}

}

// ListCache specialisation

template<>
bool ListCache<NetworkMediaItemPtr>::compareItems(const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b)
{
    //just compare the pointers here
    return a == b;
}

// NetworkMediaModelPrivate

class NetworkMediaModelPrivate
    : public LocalListBaseModelPrivate<NetworkMediaItemPtr>
{
    Q_DECLARE_PUBLIC(NetworkMediaModel)

public:
    NetworkMediaModelPrivate(NetworkMediaModel* pub)
        : LocalListBaseModelPrivate<NetworkMediaItemPtr>(pub)
        , m_preparseSem(1)
    {}

public:
    const NetworkMediaItem* getItemForRow(int row) const
    {
        const NetworkMediaItemPtr* ref = item(row);
        if (ref)
            return ref->get();
        return nullptr;
    }

    void setIntexedState(const NetworkMediaItemPtr& item, bool indexed)
    {
        if (item->indexed == indexed)
            return;
        auto it = std::find(m_items.begin(), m_items.end(), item);
        if (it != m_items.end())
        {
            //contruct by copy, don't mutate items in m_items,
            //we want the change to be notified
            auto newItem = std::make_shared<NetworkMediaItem>(*item);
            newItem->indexed = indexed;

            *it = newItem;
            ++m_revision;
            invalidateCache();
        }
    }

    void removeItem(const SharedInputItem& node, const std::vector<SharedInputItem>& itemsList)
    {
        Q_Q(NetworkMediaModel);
        if (node != q->m_treeItem.media)
            return;

        for (auto p_item : itemsList)
        {
            QUrl itemUri = QUrl::fromEncoded(p_item->psz_uri);
            auto it = std::find_if(
                m_items.begin(), m_items.end(),
                [&](const NetworkMediaItemPtr& i) {
                    return QString::compare( qfu(p_item->psz_name), i->name, Qt::CaseInsensitive ) == 0 &&
                           itemUri.scheme() == i->mainMrl.scheme();
                });
            if ( it == m_items.end() )
                continue;

            m_items.erase( it );
        }

        // notify updates
        ++m_revision;
        invalidateCache();
    }

    void refreshMediaList(
        MediaTreePtr tree,
        std::vector<SharedInputItem> children,
        bool clear )
    {
        Q_Q(NetworkMediaModel);

        if (clear)
            m_items.clear();

        std::vector<NetworkMediaItem> items;
        for (const auto& inputItem: children)
        {
            auto item = std::make_shared<NetworkMediaItem>();
            item->name = inputItem->psz_name;
            item->protocol = "";
            item->indexed = false;
            item->type = static_cast<NetworkMediaModel::ItemType>(inputItem->i_type);
            item->uri = QString(inputItem->psz_uri);
            item->mainMrl = (item->type == NetworkMediaModel::TYPE_DIRECTORY || item->type == NetworkMediaModel::TYPE_NODE) ?
                               QUrl::fromEncoded(QByteArray(inputItem->psz_uri).append('/')) :
                               QUrl::fromEncoded(inputItem->psz_uri);

            item->canBeIndexed = canBeIndexed( item->mainMrl , item->type );


            input_item_t* intputItemRaw = inputItem.get();
            char* str = input_item_GetArtworkURL(intputItemRaw);
            if (str)
            {
                item->artwork = QString::fromUtf8(str);
                free(str);
            }

            str = input_item_GetInfo(intputItemRaw, ".stat", "size");
            if (str)
            {
                item->fileSize = QString(str).toLongLong();
                free(str);
            }
            else
                item->fileSize = 0;

            str = input_item_GetInfo(intputItemRaw, ".stat", "mtime");
            if (str)
            {
                bool ok;

                qint64 time = QString(str).toLongLong(&ok);
                free(str);

                if (ok)
                    item->fileModified = QDateTime::fromSecsSinceEpoch(time, QTimeZone::systemTimeZone());
            }

            item->tree = NetworkTreeItem( tree, inputItem.get() );

            if ( m_mediaLib && item->canBeIndexed)
            {
                struct Ctx {
                    bool succeed;
                    bool isIndexed;
                };
                m_mediaLib->runOnMLThread<Ctx>(q,
                    //ML thread
                    [item](vlc_medialibrary_t* ml, Ctx& ctx){
                        auto ret = vlc_ml_is_indexed( ml, qtu(item->mainMrl.toString( QUrl::FullyEncoded )), &ctx.isIndexed );
                        ctx.succeed = (ret == VLC_SUCCESS);
                    },
                    //UI thread
                    [this, item](quint64, Ctx& ctx){
                        if (!ctx.succeed)
                            return;
                        setIntexedState(item, ctx.isIndexed);
                    });
            }

            m_items[item->uri] = item;
        }

        ++m_revision;
        invalidateCache();
    }

    bool canBeIndexed(const QUrl& url , NetworkMediaModel::ItemType itemType )
    {
        return m_mediaLib
            && static_cast<input_item_type_e>(itemType) != ITEM_TYPE_FILE
            && (url.scheme() == "smb" || url.scheme() == "ftp" || url.scheme() == "file");
    }

    //BaseModelPrivateT interface implementation
public:
    LocalListCacheLoader<NetworkMediaItemPtr>::ItemCompare getSortFunction() const override
    {
        if (m_sortCriteria == "mrl" )
        {
            if (m_sortOrder == Qt::SortOrder::DescendingOrder)
                return [](const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b) -> bool {
                    if(isADir(a) != isADir(b)) return isADir(a);
                    return QString::compare(a->mainMrl.toString(), b->mainMrl.toString()) > 0;
                };
            else
                return [](const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b) -> bool {
                    if(isADir(a) != isADir(b)) return isADir(a);
                    return QString::compare(a->mainMrl.toString(), b->mainMrl.toString()) < 0;
                };
        }
        else if (m_sortCriteria == "fileSizeRaw64" )
        {
            if (m_sortOrder == Qt::SortOrder::DescendingOrder)
                return [](const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b) -> bool {
                    if(isADir(a) != isADir(b)) return isADir(a);
                    return a->fileSize < b->fileSize;
                };
            else
                return [](const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b) -> bool {
                    if(isADir(a) != isADir(b)) return isADir(a);
                    return a->fileSize > b->fileSize;
                };
        }
        else if (m_sortCriteria == "fileModified")
        {
            if (m_sortOrder == Qt::SortOrder::DescendingOrder)
                return [](const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b) -> bool {
                    if(isADir(a) != isADir(b)) return isADir(a);
                    return a->fileModified < b->fileModified;
                };
            else
                return [](const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b) -> bool {
                    if(isADir(a) != isADir(b)) return isADir(a);
                    return a->fileModified > b->fileModified;
                };
        }
        else // m_sortCriteria == "name"
        {
            if (m_sortOrder == Qt::SortOrder::DescendingOrder)
                return [](const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b) -> bool {
                    if(isADir(a) != isADir(b)) return isADir(a);
                    return QString::compare(a->name, b->name, Qt::CaseInsensitive) > 0;
                };
            else
                return [](const NetworkMediaItemPtr& a, const NetworkMediaItemPtr& b) -> bool {
                    if(isADir(a) != isADir(b)) return isADir(a);
                    return QString::compare(a->name, b->name, Qt::CaseInsensitive) < 0;
                };
        }
    }

    bool initializeModel() override
    {
        Q_Q(NetworkMediaModel);

        if (!q->m_ctx || !m_hasTree || m_qmlInitializing)
            return false;

        auto libvlc = vlc_object_instance(q->m_ctx->getIntf());


        m_listener.reset();
        m_items.clear();

        if (!q->m_treeItem)
            return false;

        auto tree = q->m_treeItem.tree.get();
        auto l = std::make_unique<MediaTreeListener>( q->m_treeItem.tree,
                                                     std::make_unique<NetworkMediaModel::ListenerCb>(q) );
        if ( l->listener == nullptr )
            return false;

        if (q->m_treeItem.media)
        {
            q->m_name = q->m_treeItem.media->psz_name;
            emit q->nameChanged();
            q->m_url = QUrl::fromEncoded( QByteArray{ q->m_treeItem.media->psz_uri }.append( '/' ) );
            emit q->urlChanged();
            q->m_type = static_cast<NetworkMediaModel::ItemType>(q->m_treeItem.media->i_type);
            emit q->typeChanged();
            q->m_canBeIndexed = canBeIndexed( q->m_url, q->m_type );
            emit q->canBeIndexedChanged();
            if (m_mediaLib)
            {
                auto uri = QByteArray(q->m_treeItem.media->psz_uri).append('/');
                struct Ctx {
                    bool succeed;
                    bool isIndexed;
                };
                m_mediaLib->runOnMLThread<Ctx>(q,
                    //ML thread
                    [uri](vlc_medialibrary_t* ml, Ctx& ctx){
                        auto ret = vlc_ml_is_indexed( ml, uri.constData(), &ctx.isIndexed );
                        ctx.succeed = (ret == VLC_SUCCESS);
                    },
                    //ML thread
                    [this](quint64,Ctx& ctx){
                        Q_Q(NetworkMediaModel);
                        if (!ctx.succeed)
                            return;
                        q->m_indexed = ctx.isIndexed;
                        emit q->isIndexedChanged();
                    });
            }
        }

        {
            input_item_node_t* mediaNode = nullptr;
            input_item_node_t* parent = nullptr;
            vlc_media_tree_Lock(tree);
            vlc_media_tree_PreparseCancel( libvlc, this );
            std::vector<SharedInputItem> itemList;
            q->m_path = {QVariant::fromValue(PathNode(q->m_treeItem, q->m_name))};
            if (vlc_media_tree_Find( tree, q->m_treeItem.media.get(), &mediaNode, &parent))
            {
                itemList.reserve(mediaNode->i_children);
                for (int i = 0; i < mediaNode->i_children; i++)
                    itemList.emplace_back(mediaNode->pp_children[i]->p_item);

                while (parent && parent->p_item) {
                    q->m_path.push_front(QVariant::fromValue(PathNode(
                        NetworkTreeItem(q->m_treeItem.tree, parent->p_item),
                        parent->p_item->psz_name)));
                    input_item_node_t *node = nullptr;
                    input_item_node_t *grandParent = nullptr;
                    if (!vlc_media_tree_Find( tree, parent->p_item, &node, &grandParent)) {
                        break;
                    }
                    parent = grandParent;
                }
            }
            vlc_media_tree_Unlock(tree);
            if (!itemList.empty())
                refreshMediaList( q->m_treeItem.tree, std::move( itemList ), true );
            emit q->pathChanged();
        }

        m_preparseSem.acquire();
        vlc_media_tree_Preparse( tree, libvlc, q->m_treeItem.media.get(), this );

        m_listener = std::move( l );

        return true;
    }

    // LocalListCacheLoader::ModelSource interface implementation
protected:
    std::vector<NetworkMediaItemPtr> getModelData(const QString& pattern) const override
    {
        std::vector<NetworkMediaItemPtr> items;
        items.reserve(m_items.size() / 2);
        for (const auto &item : m_items)
        {
            if (item->name.contains(pattern, Qt::CaseInsensitive))
                items.push_back(item);
        }

        return items;
    }

public:
    MediaLib* m_mediaLib;
    bool m_hasTree = false;
    QSemaphore m_preparseSem;
    std::unique_ptr<MediaTreeListener> m_listener;
    QHash<QString, NetworkMediaItemPtr> m_items;
};

// NetworkMediaModel::ListenerCb implementation

struct NetworkMediaModel::ListenerCb : public MediaTreeListener::MediaTreeListenerCb {
    ListenerCb(NetworkMediaModel *model) : model(model) {}

    void onItemCleared( MediaTreePtr tree, input_item_node_t* node ) override;
    void onItemAdded( MediaTreePtr tree, input_item_node_t* parent, input_item_node_t *const children[], size_t count ) override;
    void onItemRemoved( MediaTreePtr tree, input_item_node_t * node, input_item_node_t *const children[], size_t count ) override;
    void onItemPreparseEnded( MediaTreePtr tree, input_item_node_t* node, enum input_item_preparse_status status ) override;

    NetworkMediaModel *model;
};

// NetworkMediaModel implementation

NetworkMediaModel::NetworkMediaModel( QObject* parent )
    : BaseModel( new  NetworkMediaModelPrivate(this), parent )
{
}

NetworkMediaModel::~NetworkMediaModel()
{
    Q_D(NetworkMediaModel);
    //this can only be acquired from UI thread
    if (!d->m_preparseSem.tryAcquire())
    {
        auto libvlc = vlc_object_instance(m_ctx->getIntf());
        vlc_media_tree_PreparseCancel( libvlc, this );
        //wait for the callback call on cancel
        d->m_preparseSem.acquire();
    }
}

QVariant NetworkMediaModel::data( const QModelIndex& index, int role ) const
{
    Q_D(const NetworkMediaModel);
    if (!m_ctx)
        return {};
    const NetworkMediaItem* item = d->getItemForRow(index.row());
    if (!item)
        return {};

    switch ( role )
    {
        case NETWORK_NAME:
            return item->name;
        case NETWORK_MRL:
            return item->mainMrl;
        case NETWORK_INDEXED:
            return item->indexed;
        case NETWORK_CANINDEX:
            return item->canBeIndexed;
        case NETWORK_TYPE:
            return item->type;
        case NETWORK_PROTOCOL:
            return item->protocol;
        case NETWORK_TREE:
            return QVariant::fromValue( item->tree );
        case NETWORK_ARTWORK:
            return item->artwork;
        case NETWORK_FILE_SIZE:
            return item->fileSize;
        case NETWORK_FILE_MODIFIED:
            return item->fileModified;
        default:
            return {};
    }
}

QHash<int, QByteArray> NetworkMediaModel::roleNames() const
{
    return {
        { NETWORK_NAME, "name" },
        { NETWORK_MRL, "mrl" },
        { NETWORK_INDEXED, "indexed" },
        { NETWORK_CANINDEX, "can_index" },
        { NETWORK_TYPE, "type" },
        { NETWORK_PROTOCOL, "protocol" },
        { NETWORK_TREE, "tree" },
        { NETWORK_ARTWORK, "artwork" },
        { NETWORK_FILE_SIZE, "fileSizeRaw64" },
        { NETWORK_FILE_MODIFIED, "fileModified" }
    };
}

Qt::ItemFlags NetworkMediaModel::flags( const QModelIndex& idx ) const
{
    return QAbstractListModel::flags( idx ) | Qt::ItemIsEditable;
}

bool NetworkMediaModel::setData( const QModelIndex& idx, const QVariant& value, int role )
{
    Q_D(NetworkMediaModel);

    if (!d->m_mediaLib)
        return false;

    if ( role != NETWORK_INDEXED )
        return false;

    bool enabled = value.toBool();

    const NetworkMediaItemPtr* ref = d->item(idx.row());
    if (!ref)
        return false;
    //local reference
    NetworkMediaItemPtr item = *ref;

    if ( item->indexed == enabled )
        return  true;

    QUrl mainMrl = item->mainMrl;
    struct Ctx {
        bool succeed;
    };
    d->m_mediaLib->runOnMLThread<Ctx>(this,
    //ML thread
    [enabled, mainMrl]
    (vlc_medialibrary_t* ml, Ctx& ctx){
        int res;
        if ( enabled )
            res = vlc_ml_add_folder( ml, qtu( mainMrl.toString( QUrl::FullyEncoded ) ) );
        else
            res = vlc_ml_remove_folder( ml, qtu( mainMrl.toString( QUrl::FullyEncoded ) ) );
        ctx.succeed = res == VLC_SUCCESS;
    },
    //UI thread
    [this, item, enabled](qint64, Ctx& ctx){
        Q_D(NetworkMediaModel);
        if (!ctx.succeed)
            return;
        d->setIntexedState(item, enabled);
    },
    ML_FOLDER_ADD_QUEUE);

    return true;
}


void NetworkMediaModel::setIndexed(bool indexed)
{
    Q_D(NetworkMediaModel);

    if (indexed == m_indexed || !m_canBeIndexed)
        return;
    QString url = m_url.toString( QUrl::FullyEncoded );
    struct Ctx {
        bool success;
    };
    d->m_mediaLib->runOnMLThread<Ctx>(this,
    //ML thread
    [url, indexed](vlc_medialibrary_t* ml, Ctx& ctx)
    {
        int res;
        if ( indexed )
            res = vlc_ml_add_folder(  ml, qtu( url ) );
        else
            res = vlc_ml_remove_folder( ml, qtu( url ) );
        ctx.success = (res == VLC_SUCCESS);
    },
    //UI thread
    [this, indexed](quint64, Ctx& ctx){
        if (ctx.success)
        {
            m_indexed = indexed;
            emit isIndexedChanged();
        }
    },
    ML_FOLDER_ADD_QUEUE);
}

void NetworkMediaModel::setCtx(MainCtx* ctx)
{
    Q_D(NetworkMediaModel);
    if (m_ctx == ctx)
        return;

    assert(ctx);
    m_ctx = ctx;
    d->m_mediaLib = ctx->getMediaLibrary();

    d->initializeModel();
    emit ctxChanged();
}

void NetworkMediaModel::setTree(QVariant parentTree)
{
    Q_D(NetworkMediaModel);
    if (parentTree.canConvert<NetworkTreeItem>())
        m_treeItem = parentTree.value<NetworkTreeItem>();
    else
        m_treeItem = NetworkTreeItem();
    d->m_hasTree = true;

    d->initializeModel();
    emit treeChanged();
}

bool NetworkMediaModel::insertIntoPlaylist(const QModelIndexList &itemIdList, const ssize_t playlistIndex)
{
    Q_D(NetworkMediaModel);
    if (!(m_ctx && d->m_hasTree))
        return false;
    QVector<vlc::playlist::Media> medias;
    medias.reserve( itemIdList.size() );
    for ( const QModelIndex &id : itemIdList )
    {
        if ( !id.isValid() )
            continue;

        const NetworkMediaItem* item = d->getItemForRow(id.row());
        if (!item)
            continue;

        medias.append( vlc::playlist::Media {item->tree.media.get()} );
    }
    if (medias.isEmpty())
        return false;
    m_ctx->getIntf()->p_mainPlaylistController->insert(playlistIndex, medias, false);
    return true;
}

bool NetworkMediaModel::addToPlaylist(const int index)
{
    Q_D(NetworkMediaModel);
    if (!(m_ctx && d->m_hasTree))
        return false;
    const NetworkMediaItem* item = d->getItemForRow(index);
    if (!item)
        return false;

    vlc::playlist::Media media{ item->tree.media.get() };
    m_ctx->getIntf()->p_mainPlaylistController->append( QVector<vlc::playlist::Media>{ media }, false);
    return true;
}

bool NetworkMediaModel::addToPlaylist(const QVariantList &itemIdList)
{
    bool ret = false;
    for (const QVariant& varValue: itemIdList)
    {
        int index = -1;

        if (varValue.canConvert<int>())
            index = varValue.value<int>();
        else if (varValue.canConvert<QModelIndex>())
            index = varValue.value<QModelIndex>().row();
        else
            continue;

        ret |= addToPlaylist(index);
    }
    return ret;
}

bool NetworkMediaModel::addToPlaylist(const QModelIndexList &itemIdList)
{
    bool ret = false;
    for (const QModelIndex& index: itemIdList)
    {
        if (!index.isValid())
            continue;
        ret |= addToPlaylist(index.row());
    }
    return ret;
}

bool NetworkMediaModel::addAndPlay(int index)
{
    Q_D(NetworkMediaModel);
    if (!(m_ctx && d->m_hasTree))
        return false;

    const NetworkMediaItem* item = d->getItemForRow(index);
    if (!item)
        return false;

    vlc::playlist::Media media{ item->tree.media.get() };
    m_ctx->getIntf()->p_mainPlaylistController->append( QVector<vlc::playlist::Media>{ media }, true);
    return true;
}

bool NetworkMediaModel::addAndPlay(const QVariantList& itemIdList)
{
    bool ret = false;
    for (const QVariant& varValue: itemIdList)
    {
        int index = -1;

        if (varValue.canConvert<int>())
            index = varValue.value<int>();
        else if (varValue.canConvert<QModelIndex>())
            index = varValue.value<QModelIndex>().row();
        else
            continue;

        if (!ret)
            ret |= addAndPlay(index);
        else
            ret |= addToPlaylist(index);
    }
    return ret;
}

bool NetworkMediaModel::addAndPlay(const QModelIndexList& itemIdList)
{
    bool ret = false;
    for (const QModelIndex& index: itemIdList)
    {
        if (!index.isValid())
            continue;

        if (!ret)
            ret |= addAndPlay(index.row());
        else
            ret |= addToPlaylist(index.row());
    }
    return ret;
}

/* Q_INVOKABLE */
QVariantList NetworkMediaModel::getItemsForIndexes(const QModelIndexList & indexes) const
{
    Q_D(const NetworkMediaModel);
    QVariantList items;

    for (const QModelIndex & modelIndex : indexes)
    {
        int index = modelIndex.row();

        const NetworkMediaItem* item = d->getItemForRow(index);
        if (!item)
            return {};

        const NetworkTreeItem & tree = item->tree;

        items.append(QVariant::fromValue(SharedInputItem(tree.media.get(), true)));
    }

    return items;
}

void NetworkMediaModel::ListenerCb::onItemCleared( MediaTreePtr tree, input_item_node_t* node)
{
    SharedInputItem p_node { node->p_item };
    QMetaObject::invokeMethod(model, [model=model, p_node = std::move(p_node), tree = std::move(tree)]() {
        if (p_node != model->m_treeItem.media)
            return;
        input_item_node_t *res;
        input_item_node_t *parent;
        // XXX is tree == m_treeItem.tree?
        vlc_media_tree_Lock( model->m_treeItem.tree.get() );
        bool found = vlc_media_tree_Find( model->m_treeItem.tree.get(), model->m_treeItem.media.get(),
                                          &res, &parent );
        vlc_media_tree_Unlock( model->m_treeItem.tree.get() );
        if (!found)
            return;

        std::vector<SharedInputItem> itemList;
        itemList.reserve( static_cast<size_t>(res->i_children) );
        for (int i = 0; i < res->i_children; i++)
            itemList.emplace_back(res->pp_children[i]->p_item);

        model->d_func()->refreshMediaList( std::move( tree ), std::move( itemList ), true );
    }, Qt::QueuedConnection);
}

void NetworkMediaModel::ListenerCb::onItemAdded( MediaTreePtr tree, input_item_node_t* parent,
                                                 input_item_node_t *const children[],
                                                 size_t count )
{
    SharedInputItem p_parent { parent->p_item };
    std::vector<SharedInputItem> itemList;
    itemList.reserve( count );
    for (size_t i = 0; i < count; i++)
        itemList.emplace_back(children[i]->p_item);

    QMetaObject::invokeMethod(model, [model=model, p_parent = std::move(p_parent), tree = std::move(tree), itemList=std::move(itemList)]() {
        if ( p_parent == model->m_treeItem.media )
            model->d_func()->refreshMediaList( std::move( tree ), std::move( itemList ), false );
    }, Qt::QueuedConnection);
}

void NetworkMediaModel::ListenerCb::onItemRemoved( MediaTreePtr, input_item_node_t * node,
                                                   input_item_node_t *const children[],
                                                   size_t count )
{
    std::vector<SharedInputItem> itemList;
    itemList.reserve( count );
    for ( auto i = 0u; i < count; ++i )
        itemList.emplace_back( children[i]->p_item );

    SharedInputItem p_node { node->p_item };
    QMetaObject::invokeMethod(
        model,
        [model=model, p_node=std::move(p_node), itemList=std::move(itemList)]() {
            model->d_func()->removeItem(p_node, itemList);
    }, Qt::QueuedConnection);
}

void NetworkMediaModel::ListenerCb::onItemPreparseEnded(MediaTreePtr, input_item_node_t* node, enum input_item_preparse_status )
{
    model->d_func()->m_preparseSem.release();
    SharedInputItem p_node { node->p_item };
    QMetaObject::invokeMethod(model, [model=model, p_node=std::move(p_node)]() {
        if (p_node != model->m_treeItem.media)
            return;

        model->d_func()->m_loading = false;
        emit model->loadingChanged();
    });
}
