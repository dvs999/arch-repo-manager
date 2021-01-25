#ifndef LIBPKG_DATA_CONFIG_H
#define LIBPKG_DATA_CONFIG_H

#include "./database.h"
#include "./lockable.h"
#include "./siglevel.h"

#include "../global.h"

#include <reflective_rapidjson/binary/serializable.h>
#include <reflective_rapidjson/json/serializable.h>

#include <cstring>
#include <mutex>
#include <regex>
#include <set>

namespace LibPkg {

/*!
 * \brief The SigStatus enum specifies PGP signature verification status return codes.
 */
enum class SignatureStatus { Valid, KeyExpired, SigExpired, KeyUnknown, KeyDisabled, InvalidId };

struct Config;
struct Database;

struct LIBPKG_EXPORT DatabaseStatistics : public ReflectiveRapidJSON::JsonSerializable<Config> {
    DatabaseStatistics(const Database &config);

    const std::string &name;
    const std::size_t packageCount;
    const std::string &arch;
    const CppUtilities::DateTime lastUpdate;
    const std::string &localPkgDir;
    const std::string &mainMirror;
    const bool syncFromMirror;
};

struct LIBPKG_EXPORT Status : public ReflectiveRapidJSON::JsonSerializable<Status> {
    Status(const Config &config);

    std::vector<DatabaseStatistics> dbStats;
    const std::set<std::string> &architectures;
    const std::string &pacmanDatabasePath;
    const std::vector<std::string> &packageCacheDirs;
};

struct TopoSortItem;

struct LIBPKG_EXPORT BuildOrderResult : public ReflectiveRapidJSON::JsonSerializable<BuildOrderResult> {
    std::vector<PackageSearchResult> order;
    std::vector<PackageSearchResult> cycle;
    std::vector<std::string> ignored;
    bool success = false;
};

enum BuildOrderOptions {
    None = 0x0, /**< none of the other options enabled */
    IncludeSourceOnlyDependencies = 0x2, /**< whether source-only dependencies should be added the list of resulting packages */
    IncludeAllDependencies
    = 0x3, /**< whether *all* dependencies should be added the list of resulting packages (implies IncludeSourceOnlyDependencies) */
    ConsiderBuildDependencies = 0x4, /**< whether build dependencies should be taken into account for the topo sort */
};

struct LicenseFile : public ReflectiveRapidJSON::JsonSerializable<LicenseFile>, public ReflectiveRapidJSON::BinarySerializable<LicenseFile> {
    LicenseFile() = default;
    LicenseFile(std::string &&filename, std::string &&content)
        : filename(filename)
        , content(content)
    {
    }
    std::string filename;
    std::string content;
};

struct LIBPKG_EXPORT CommonLicense : public ReflectiveRapidJSON::JsonSerializable<CommonLicense>,
                                     public ReflectiveRapidJSON::BinarySerializable<CommonLicense> {
    std::set<std::string> relevantPackages;
    std::vector<LicenseFile> files;
};

struct LIBPKG_EXPORT LicenseResult : public ReflectiveRapidJSON::JsonSerializable<LicenseResult>,
                                     public ReflectiveRapidJSON::BinarySerializable<LicenseResult> {
    std::map<std::string, CommonLicense> commonLicenses;
    std::map<std::string, std::vector<LicenseFile>> customLicences;
    std::vector<std::string> consideredPackages;
    std::vector<std::string> ignoredPackages;
    std::vector<std::string> notes;
    std::string mainProject;
    std::set<std::string> dependendProjects;
    std::string licenseSummary;
    bool success = true;
};

constexpr BuildOrderOptions operator|(BuildOrderOptions lhs, BuildOrderOptions rhs)
{
    return static_cast<BuildOrderOptions>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

constexpr bool operator&(BuildOrderOptions lhs, BuildOrderOptions rhs)
{
    return (static_cast<int>(lhs) & static_cast<int>(rhs)) != 0;
}

struct LIBPKG_EXPORT Config : public Lockable, public ReflectiveRapidJSON::BinarySerializable<Config> {
    // load config and packages
    void loadPacmanConfig(const char *pacmanConfigPath);
    void loadAllPackages(bool withFiles);

    // caching
    std::uint64_t restoreFromCache();
    std::uint64_t dumpCacheFile();
    void markAllDatabasesToBeDiscarded();
    void discardDatabases();

    // computions
    Status computeStatus() const;
    BuildOrderResult computeBuildOrder(const std::vector<std::string> &dependencyDenotations, BuildOrderOptions options);
    LicenseResult computeLicenseInfo(const std::vector<std::string> &dependencyDenotations);
    std::variant<std::vector<Database *>, std::string> computeDatabaseDependencyOrder(Database &database);
    std::vector<Database *> computeDatabasesRequiringDatabase(Database &database);
    void pullDependentPackages(const std::vector<Dependency> &dependencies, const std::shared_ptr<Package> &relevantPackage,
        const std::unordered_set<LibPkg::Database *> &relevantDbs, std::unordered_set<Package *> &runtimeDependencies,
        DependencySet &missingDependencies);
    void pullDependentPackages(const std::shared_ptr<Package> &package, const std::unordered_set<LibPkg::Database *> &relevantDbs,
        std::unordered_set<LibPkg::Package *> &runtimeDependencies, DependencySet &missingDependencies);

    // search for packages
    static std::pair<std::string_view, std::string_view> parseDatabaseDenotation(std::string_view databaseDenotation);
    Database *findDatabase(std::string_view name, std::string_view architecture);
    Database *findDatabaseFromDenotation(std::string_view databaseDenotation);
    Database *findOrCreateDatabase(std::string &&name, std::string_view architecture);
    Database *findOrCreateDatabase(std::string_view name, std::string_view architecture);
    Database *findOrCreateDatabaseFromDenotation(std::string_view databaseDenotation);
    static std::tuple<std::string_view, std::string_view, std::string_view> parsePackageDenotation(std::string_view packageDenotation);
    std::vector<PackageSearchResult> findPackages(std::string_view packageDenotation);
    std::vector<PackageSearchResult> findPackages(std::string_view dbName, std::string_view dbArch, std::string_view packageName);
    std::vector<PackageSearchResult> findPackages(std::tuple<std::string_view, std::string_view, std::string_view> dbAndPackageName);
    PackageSearchResult findPackage(const Dependency &dependency);
    std::vector<PackageSearchResult> findPackages(const Dependency &dependency, bool reverse = false);
    std::vector<PackageSearchResult> findPackagesProvidingLibrary(const std::string &library, bool reverse = false);
    std::vector<PackageSearchResult> findPackages(const std::regex &regex);
    std::vector<PackageSearchResult> findPackages(const Package &package);
    std::vector<PackageSearchResult> findPackages(
        const std::function<bool(const Database &)> &databasePred, const std::function<bool(const Database &, const Package &)> &packagePred);
    std::vector<PackageSearchResult> findPackages(const std::function<bool(const Database &, const Package &)> &pred);

    // utilities
    std::list<std::string> forEachPackage(const std::function<std::string(Database *db)> &processNextDatabase,
        const std::function<std::string(Database *db, std::shared_ptr<Package> &pkg, std::mutex &dbMutex)> &processNextPackage);

    std::vector<Database> databases;
    Database aur = Database("aur");
    std::set<std::string> architectures;
    std::string pacmanDatabasePath;
    std::vector<std::string> packageCacheDirs;
    SignatureLevelConfig signatureLevel;

private:
    bool addDepsRecursivelyInTopoOrder(std::vector<std::unique_ptr<TopoSortItem>> &allItems, std::vector<TopoSortItem *> &items,
        std::vector<std::string> &ignored, std::vector<PackageSearchResult> &cycleTracking, const Dependency &dependency, BuildOrderOptions options,
        bool onlyDependency);
    bool addLicenseInfo(LicenseResult &result, const Dependency &dependency);
    std::string addLicenseInfo(LicenseResult &result, PackageSearchResult &searchResult, const std::shared_ptr<Package> &package);
};

inline Status Config::computeStatus() const
{
    return Status(*this);
}

inline std::vector<PackageSearchResult> Config::findPackages(std::string_view dbName, std::string_view dbArch, std::string_view packageName)
{
    return findPackages(std::make_tuple(dbName, dbArch, packageName));
}

inline std::vector<PackageSearchResult> Config::findPackages(std::string_view packageDenotation)
{
    return findPackages(parsePackageDenotation(packageDenotation));
}

} // namespace LibPkg

#endif // LIBPKG_DATA_CONFIG_H