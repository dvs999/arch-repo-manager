#ifndef LIBPKG_DATA_DATABASE_H
#define LIBPKG_DATA_DATABASE_H

#include "./package.h"
#include "./siglevel.h"

#include "../global.h"

#include <c++utilities/chrono/datetime.h>
#include <c++utilities/misc/flagenumclass.h>

#include <filesystem>
#include <optional>
#include <unordered_set>

namespace LibPkg {

struct Config;
struct Database;

struct LIBPKG_EXPORT DatabaseInfo {
    std::string name;
    std::string arch;
};

struct LIBPKG_EXPORT PackageSearchResult {
    PackageSearchResult();
    PackageSearchResult(Database &database, const std::shared_ptr<Package> &package);
    bool operator==(const PackageSearchResult &other) const;

    /// \brief The related database.
    /// \remarks
    /// - The find functions always use Database* and it is guaranteed to be never nullptr.
    /// - The deserialization functions always use DatabaseInfo and the values might be empty if the source was empty.
    /// - The serialization functions can cope with both alternatives.
    std::variant<Database *, DatabaseInfo> db;
    std::shared_ptr<Package> pkg;
};

/*!
 * \brief The DatabaseUsage enum specifies the usage of a database within pacman.
 */
enum class DatabaseUsage {
    None = 0,
    Sync = 1, /*! The database is used when synchronizing. */
    Search = (1 << 1), /*! The database is used when searching. */
    Install = (1 << 2), /*! The database is used to install packages. */
    Upgrade = (1 << 3), /*! The database is used to upgrade packages. */
    All = (1 << 4) - 1, /*! The database is used for everything. */
};

} // namespace LibPkg

CPP_UTILITIES_MARK_FLAG_ENUM_CLASS(LibPkg, LibPkg::DatabaseUsage)

namespace LibPkg {

struct LIBPKG_EXPORT PackageUpdate : public ReflectiveRapidJSON::JsonSerializable<PackageUpdate>,
                                     public ReflectiveRapidJSON::BinarySerializable<PackageUpdate> {
    PackageUpdate(PackageSearchResult &&oldVersion = PackageSearchResult(), PackageSearchResult &&newVersion = PackageSearchResult())
        : oldVersion(oldVersion)
        , newVersion(newVersion)
    {
    }
    PackageSearchResult oldVersion;
    PackageSearchResult newVersion;
};

struct LIBPKG_EXPORT PackageUpdates : public ReflectiveRapidJSON::JsonSerializable<PackageUpdates>,
                                      public ReflectiveRapidJSON::BinarySerializable<PackageUpdates> {
    std::vector<PackageUpdate> versionUpdates;
    std::vector<PackageUpdate> packageUpdates;
    std::vector<PackageUpdate> downgrades;
    std::vector<PackageSearchResult> orphans;
};

struct LIBPKG_EXPORT PackageLocation {
    std::filesystem::path pathWithinRepo;
    std::filesystem::path storageLocation;
    std::optional<std::filesystem::filesystem_error> error;
    bool exists = false;
};

struct LIBPKG_EXPORT UnresolvedDependencies : public ReflectiveRapidJSON::JsonSerializable<UnresolvedDependencies>,
                                              public ReflectiveRapidJSON::BinarySerializable<UnresolvedDependencies> {
    std::vector<Dependency> deps;
    std::vector<std::string> libs;
};

struct LIBPKG_EXPORT Database : public ReflectiveRapidJSON::JsonSerializable<Database>, public ReflectiveRapidJSON::BinarySerializable<Database> {
    using PackageMap = std::unordered_map<std::string, std::shared_ptr<Package>>;

    Database(const std::string &name = std::string(), const std::string &path = std::string());
    Database(std::string &&name, std::string &&path);
    void deducePathsFromLocalDirs();
    void resetConfiguration();
    void clearPackages();
    void loadPackages(bool withFiles = false);
    void loadPackages(const std::string &databaseData, CppUtilities::DateTime lastModified);
    void loadPackages(FileMap &&databaseFiles, CppUtilities::DateTime lastModified);
    static bool isFileRelevant(const char *filePath, const char *fileName, mode_t);
    std::vector<std::shared_ptr<Package>> findPackages(const std::function<bool(const Database &, const Package &)> &pred);
    void removePackageDependencies(typename PackageMap::const_iterator packageIterator);
    void addPackageDependencies(const std::shared_ptr<Package> &package);
    void removePackage(const std::string &packageName);
    void removePackage(typename PackageMap::const_iterator packageIterator);
    void updatePackage(const std::shared_ptr<Package> &package);
    void forceUpdatePackage(const std::shared_ptr<Package> &package);
    void replacePackages(const std::vector<std::shared_ptr<Package>> &newPackages, CppUtilities::DateTime lastModified);
    std::unordered_map<std::shared_ptr<Package>, UnresolvedDependencies> detectUnresolvedPackages(
        Config &config, const std::vector<std::shared_ptr<Package>> &newPackages, const DependencySet &removedPackages);
    PackageUpdates checkForUpdates(const std::vector<Database *> &updateSources);
    PackageLocation locatePackage(const std::string &packageName) const;
    std::string filesPathFromRegularPath() const;

    std::string name;
    std::string path;
    std::string filesPath;
    std::vector<std::string> mirrors;
    PackageMap packages;
    DatabaseUsage usage = DatabaseUsage::None;
    SignatureLevel signatureLevel = SignatureLevel::Default;
    std::string arch = "x86_64";
    std::vector<std::string> dependencies;
    DependencySet providedDeps;
    DependencySet requiredDeps;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Package>>> providedLibs;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Package>>> requiredLibs;
    std::string localPkgDir;
    std::string localDbDir;
    CppUtilities::DateTime lastUpdate;
    bool syncFromMirror = false;
    bool toBeDiscarded = false;

    // FIXME: The member variables packages, providedDeps, requiredDeps, providedLibs and requiredLibs should
    //        not be updated directly/individually; better make them private and provide a getter to a const ref.
};

inline Database::Database(const std::string &name, const std::string &path)
    : name(name)
    , path(path)
{
}

inline Database::Database(std::string &&name, std::string &&path)
    : name(std::move(name))
    , path(std::move(path))
{
}

inline PackageSearchResult::PackageSearchResult()
    : db(nullptr)
{
}

inline PackageSearchResult::PackageSearchResult(Database &database, const std::shared_ptr<Package> &package)
    : db(&database)
    , pkg(package)
{
}

inline bool PackageSearchResult::operator==(const PackageSearchResult &other) const
{
    const auto *const *const db1 = std::get_if<Database *>(&db);
    const auto *const *const db2 = std::get_if<Database *>(&other.db);
    if (!db1 || !db2) {
        return false;
    }
    return ((!*db1 && !*db2) || (*db1 && *db2 && (**db1).name == (**db2).name)) && pkg == other.pkg;
}

} // namespace LibPkg

namespace std {

template <> struct hash<LibPkg::PackageSearchResult> {
    std::size_t operator()(const LibPkg::PackageSearchResult &res) const
    {
        using std::hash;
        const std::string *dbName = nullptr;
        if (const auto *const dbInfo = std::get_if<LibPkg::DatabaseInfo>(&res.db)) {
            dbName = &dbInfo->name;
        } else if (const auto *const db = std::get<LibPkg::Database *>(res.db)) {
            dbName = &db->name;
        }
        return ((hash<string>()(dbName ? *dbName : string()) ^ (hash<std::shared_ptr<LibPkg::Package>>()(res.pkg) << 1)) >> 1);
    }
};

} // namespace std

namespace ReflectiveRapidJSON {

namespace JsonReflector {

// declare custom (de)serialization for PackageSearchResult
template <>
LIBPKG_EXPORT void push<LibPkg::PackageSearchResult>(
    const LibPkg::PackageSearchResult &reflectable, RAPIDJSON_NAMESPACE::Value &value, RAPIDJSON_NAMESPACE::Document::AllocatorType &allocator);
template <>
LIBPKG_EXPORT void pull<LibPkg::PackageSearchResult>(LibPkg::PackageSearchResult &reflectable,
    const RAPIDJSON_NAMESPACE::GenericValue<RAPIDJSON_NAMESPACE::UTF8<char>> &value, JsonDeserializationErrors *errors);

} // namespace JsonReflector

namespace BinaryReflector {

template <>
LIBPKG_EXPORT void writeCustomType<LibPkg::PackageSearchResult>(BinarySerializer &serializer, const LibPkg::PackageSearchResult &packageSearchResult);
template <>
LIBPKG_EXPORT void readCustomType<LibPkg::PackageSearchResult>(BinaryDeserializer &deserializer, LibPkg::PackageSearchResult &packageSearchResult);

} // namespace BinaryReflector

} // namespace ReflectiveRapidJSON

#endif // LIBPKG_DATA_DATABASE_H