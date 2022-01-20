#include "./storageprivate.h"

#include <c++utilities/conversion/stringbuilder.h>

using namespace CppUtilities;

namespace LibPkg {

template <typename StorageEntryType>
template <typename IndexType>
auto StorageCacheEntries<StorageEntryType>::find(const IndexType &ref) -> StorageEntry *
{
    const auto &index = m_entries.template get<IndexType>();
    if (auto i = index.find(ref); i != index.end()) {
        m_entries.relocate(m_entries.begin(), m_entries.template project<0>(i));
        return &i.get_node()->value();
    }
    return nullptr;
}

template <typename StorageEntryType> auto StorageCacheEntries<StorageEntryType>::insert(StorageEntry &&entry) -> StorageEntry &
{
    const auto [i, newItem] = m_entries.emplace_front(entry);
    if (!newItem) {
        m_entries.relocate(m_entries.begin(), i);
    } else if (m_entries.size() > m_limit) {
        m_entries.pop_back();
    }
    return i.get_node()->value();
}

template <typename StorageEntryType> std::size_t StorageCacheEntries<StorageEntryType>::clear(const Storage &storage)
{
    auto count = std::size_t();
    for (auto i = m_entries.begin(); i != m_entries.end();) {
        if (i->ref.relatedStorage == &storage) {
            i = m_entries.erase(i);
            ++count;
        } else {
            ++i;
        }
    }
    return count;
}

template <typename StorageEntriesType, typename TransactionType, typename SpecType>
auto StorageCache<StorageEntriesType, TransactionType, SpecType>::retrieve(Storage &storage, StorageID storageID) -> SpecType
{
    // check for package in cache
    const auto ref = typename StorageEntryByID<typename Entries::StorageEntry>::result_type{ storageID, &storage };
    auto lock = std::unique_lock(m_mutex);
    if (auto *const existingCacheEntry = m_entries.find(ref)) {
        return SpecType(existingCacheEntry->id, existingCacheEntry->entry);
    }
    // check for package in storage, populate cache entry
    lock.unlock();
    auto entry = std::make_shared<Entry>();
    auto txn = storage.packages.getROTransaction();
    if (auto id = txn.get(storageID, *entry)) {
        using CacheEntry = typename Entries::StorageEntry;
        using CacheRef = typename Entries::Ref;
        auto newCacheEntry = CacheEntry(CacheRef(storage, entry), id);
        newCacheEntry.entry = entry;
        lock = std::unique_lock(m_mutex);
        m_entries.insert(std::move(newCacheEntry));
        lock.unlock();
        return SpecType(id, entry);
    }
    return SpecType(0, std::shared_ptr<Entry>());
}

template <typename StorageEntriesType, typename TransactionType, typename SpecType>
auto StorageCache<StorageEntriesType, TransactionType, SpecType>::retrieve(Storage &storage, const std::string &entryName) -> SpecType
{
    // check for package in cache
    using CacheRef = typename Entries::Ref;
    const auto ref = CacheRef(storage, entryName);
    auto lock = std::unique_lock(m_mutex);
    if (auto *const existingCacheEntry = m_entries.find(ref)) {
        return SpecType(existingCacheEntry->id, existingCacheEntry->entry);
    }
    lock.unlock();
    // check for package in storage, populate cache entry
    auto entry = std::make_shared<Entry>();
    auto txn = storage.packages.getROTransaction();
    if (auto id = txn.template get<0>(entryName, *entry)) {
        using CacheEntry = typename Entries::StorageEntry;
        auto newCacheEntry = CacheEntry(ref, id);
        newCacheEntry.entry = entry;
        lock = std::unique_lock(m_mutex);
        m_entries.insert(std::move(newCacheEntry));
        lock.unlock();
        return SpecType(id, entry);
    }
    return SpecType(0, std::shared_ptr<Entry>());
}

template <typename StorageEntriesType, typename TransactionType, typename SpecType>
auto StorageCache<StorageEntriesType, TransactionType, SpecType>::store(Storage &storage, const std::shared_ptr<Entry> &entry, bool force)
    -> StoreResult
{
    // check for package in cache
    using CacheEntry = typename Entries::StorageEntry;
    using CacheRef = typename Entries::Ref;
    const auto ref = CacheRef(storage, entry->name);
    auto res = StorageCache::StoreResult();
    auto lock = std::unique_lock(m_mutex);
    auto *cacheEntry = m_entries.find(ref);
    if (cacheEntry) {
        res.id = cacheEntry->id;
        res.oldEntry = cacheEntry->entry;
        if (cacheEntry->entry == entry && !force) {
            // do nothing if cached package is the same as specified one
            return res;
        } else {
            // retain certain information obtained from package contents if this is actually the same package as before
            entry->addDepsAndProvidesFromOtherPackage(*cacheEntry->entry);
        }
    }
    lock.unlock();
    // check for package in storage
    auto txn = storage.packages.getRWTransaction();
    if (!res.oldEntry) {
        res.oldEntry = std::make_shared<Entry>();
        if (txn.template get<0>(entry->name, *res.oldEntry)) {
            entry->addDepsAndProvidesFromOtherPackage(*res.oldEntry);
        } else {
            res.oldEntry.reset();
        }
    }
    // update package in storage
    res.id = txn.put(*entry, res.id);
    // update cache entry
    lock = std::unique_lock(m_mutex);
    if (cacheEntry) {
        cacheEntry->ref.entryName = &entry->name;
    } else {
        cacheEntry = &m_entries.insert(CacheEntry(ref, res.id));
    }
    cacheEntry->entry = entry;
    lock.unlock();
    txn.commit();
    res.updated = true;
    return res;
}

template <typename StorageEntriesType, typename TransactionType, typename SpecType>
auto StorageCache<StorageEntriesType, TransactionType, SpecType>::store(Storage &storage, Txn &txn, const std::shared_ptr<Entry> &entry)
    -> StoreResult
{
    // check for package in cache
    using CacheEntry = typename Entries::StorageEntry;
    using CacheRef = typename Entries::Ref;
    const auto ref = CacheRef(storage, entry->name);
    auto res = StorageCache::StoreResult();
    auto lock = std::unique_lock(m_mutex);
    auto *cacheEntry = m_entries.find(ref);
    if (cacheEntry) {
        // retain certain information obtained from package contents if this is actually the same package as before
        res.id = cacheEntry->id;
        entry->addDepsAndProvidesFromOtherPackage(*(res.oldEntry = cacheEntry->entry));
    }
    lock.unlock();
    // check for package in storage
    if (!res.oldEntry) {
        res.oldEntry = std::make_shared<Entry>();
        if (txn.template get<0>(entry->name, *res.oldEntry)) {
            entry->addDepsAndProvidesFromOtherPackage(*res.oldEntry);
        } else {
            res.oldEntry.reset();
        }
    }
    // update package in storage
    res.id = txn.put(*entry, res.id);
    // update cache entry
    lock = std::unique_lock(m_mutex);
    if (cacheEntry) {
        cacheEntry->ref.entryName = &entry->name;
    } else {
        cacheEntry = &m_entries.insert(CacheEntry(ref, res.id));
    }
    cacheEntry->entry = entry;
    lock.unlock();
    res.updated = true;
    return res;
}

template <typename StorageEntriesType, typename TransactionType, typename SpecType>
bool StorageCache<StorageEntriesType, TransactionType, SpecType>::invalidate(Storage &storage, const std::string &entryName)
{
    // remove package from cache
    const auto ref = typename Entries::Ref(storage, entryName);
    auto lock = std::unique_lock(m_mutex);
    m_entries.erase(ref);
    lock.unlock();
    // remove package from storage
    auto txn = storage.packages.getRWTransaction();
    if (auto i = txn.template find<0>(entryName); i != txn.end()) {
        i.del();
        txn.commit();
        return true;
    }
    return false;
}

template <typename StorageEntriesType, typename TransactionType, typename SpecType>
void StorageCache<StorageEntriesType, TransactionType, SpecType>::clear(Storage &storage)
{
    clearCacheOnly(storage);
    auto packagesTxn = storage.packages.getRWTransaction();
    packagesTxn.clear();
    packagesTxn.commit();
    auto providedDepsTxn = storage.providedDeps.getRWTransaction();
    providedDepsTxn.clear();
    providedDepsTxn.commit();
    auto requiredDepsTxn = storage.requiredDeps.getRWTransaction();
    requiredDepsTxn.clear();
    requiredDepsTxn.commit();
    auto providedLibsTxn = storage.providedLibs.getRWTransaction();
    providedLibsTxn.clear();
    providedLibsTxn.commit();
    auto requiredLibsTxn = storage.requiredLibs.getRWTransaction();
    requiredLibsTxn.clear();
    requiredLibsTxn.commit();
}

template <typename StorageEntriesType, typename TransactionType, typename SpecType>
void StorageCache<StorageEntriesType, TransactionType, SpecType>::clearCacheOnly(Storage &storage)
{
    const auto lock = std::unique_lock(m_mutex);
    m_entries.clear(storage);
}

template struct StorageCacheRef<DatabaseStorage, Package>;
template struct StorageCacheEntry<PackageCacheRef, Package>;
template class StorageCacheEntries<PackageCacheEntry>;
template struct StorageCache<PackageCacheEntries, PackageStorage::RWTransaction, PackageSpec>;

StorageDistribution::StorageDistribution(const char *path, std::uint32_t maxDbs)
{
    m_env = LMDBSafe::getMDBEnv(path, MDB_NOSUBDIR, 0600, maxDbs);
}

DatabaseStorage::DatabaseStorage(const std::shared_ptr<LMDBSafe::MDBEnv> &env, PackageCache &packageCache, std::string_view uniqueDatabaseName)
    : packageCache(packageCache)
    , packages(env, argsToString(uniqueDatabaseName, "_packages"))
    , providedDeps(env, argsToString(uniqueDatabaseName, "_provides"))
    , requiredDeps(env, argsToString(uniqueDatabaseName, "_requires"))
    , providedLibs(env, argsToString(uniqueDatabaseName, "_libprovides"))
    , requiredLibs(env, argsToString(uniqueDatabaseName, "_librequires"))
    , m_env(env)
{
}

std::size_t hash_value(const PackageCacheRef &ref)
{
    const auto hasher1 = boost::hash<const LibPkg::DatabaseStorage *>();
    const auto hasher2 = boost::hash<std::string>();
    return ((hasher1(ref.relatedStorage) ^ (hasher2(*ref.entryName) << 1)) >> 1);
}

std::size_t hash_value(const PackageCacheEntryByID &entryByID)
{
    const auto hasher1 = boost::hash<StorageID>();
    const auto hasher2 = boost::hash<const LibPkg::DatabaseStorage *>();
    return ((hasher1(entryByID.id) ^ (hasher2(entryByID.storage) << 1)) >> 1);
}

} // namespace LibPkg
