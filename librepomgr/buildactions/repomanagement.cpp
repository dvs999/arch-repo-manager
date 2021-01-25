#include "./buildactionprivate.h"

#include "../logging.h"

#include <c++utilities/chrono/datetime.h>
#include <c++utilities/conversion/stringbuilder.h>
#include <c++utilities/conversion/stringconversion.h>
#include <c++utilities/io/ansiescapecodes.h>
#include <c++utilities/io/path.h>

#include <boost/process/search_path.hpp>
#include <boost/process/start_dir.hpp>

#include <filesystem>
#include <iostream>

using namespace std;
using namespace std::literals::string_view_literals;
using namespace CppUtilities;
using namespace CppUtilities::EscapeCodes;

namespace LibRepoMgr {

PackageMovementAction::PackageMovementAction(ServiceSetup &setup, const std::shared_ptr<BuildAction> &buildAction)
    : InternalBuildAction(setup, buildAction)
{
}

bool PackageMovementAction::prepareRepoAction(RequiredDatabases requiredDatabases)
{
    // initialize build action
    auto configReadLock = init(BuildActionAccess::ReadConfig, requiredDatabases | RequiredDatabases::OneDestination, RequiredParameters::Packages);
    if (std::holds_alternative<std::monostate>(configReadLock)) {
        return false;
    }

    auto setupLock = m_setup.lockToRead();
    m_repoRemovePath = findExecutable(m_setup.building.repoRemovePath);
    if (requiredDatabases & RequiredDatabases::OneSource) {
        m_repoAddPath = findExecutable(m_setup.building.repoAddPath);
    }
    setupLock.unlock();

    // check executables
    if (!checkExecutable(m_repoRemovePath)) {
        reportError("Unable to find repo-remove executable \"" % m_setup.building.repoRemovePath + "\" in PATH.");
        return false;
    }
    if (requiredDatabases & RequiredDatabases::OneSource && !checkExecutable(m_repoAddPath)) {
        reportError("Unable to find repo-add executable \"" % m_setup.building.repoAddPath + "\" in PATH.");
        return false;
    }

    // locate databases and packages
    const auto *const destinationDb = *m_destinationDbs.begin();
    m_destinationRepoDirectory = destinationDb->localPkgDir;
    m_destinationDatabaseFile = fileName(destinationDb->path);
    if (requiredDatabases & RequiredDatabases::OneSource) {
        const auto *const sourceDb = *m_sourceDbs.begin();
        m_sourceRepoDirectory = sourceDb->localPkgDir;
        m_sourceDatabaseFile = fileName(sourceDb->path);
    }
    locatePackages();
    configReadLock = std::monostate();

    // error-out early if not even a single package could be located
    if (m_packageLocations.empty()) {
        m_result.errorMessage = "none of the specified packages could be located";
        reportResultWithData(BuildActionResult::Failure);
        return false;
    }

    // init working directory
    initWorkingDirectory();
    if (!m_result.errorMessage.empty()) {
        reportResultWithData(BuildActionResult::Failure);
        return false;
    }
    return true;
}

void PackageMovementAction::initWorkingDirectory()
{
    if (m_buildAction->directory.empty()) {
        auto directory = argsToString(m_buildAction->type == BuildActionType::MovePackages ? "repo-move-" : "repo-remove-",
            DateTime::gmtNow().toIsoStringWithCustomDelimiters(TimeSpan(), '-', '-'), '-',
            std::filesystem::path(m_destinationDatabaseFile).stem().string());
        auto buildActionLock = m_setup.building.lockToWrite();
        m_buildAction->directory = std::move(directory);
    }
    m_workingDirectory = determineWorkingDirectory(repoManagementWorkingDirectory);
    try {
        std::filesystem::create_directories(m_workingDirectory);
    } catch (const std::filesystem::filesystem_error &e) {
        m_buildAction->log()(Phrases::ErrorMessage, "Unable to make working directory: ", e.what(), '\n');
        m_result.errorMessage = argsToString("unable to make working directory: ", e.what());
    }
}

void PackageMovementAction::locatePackages()
{
    // determine repo path and package paths
    LibPkg::Database *db;
    if (m_sourceDbs.empty()) {
        db = *m_destinationDbs.begin();
    } else {
        db = *m_sourceDbs.begin();
    }
    const auto &packages = db->packages;
    for (const auto &packageName : m_buildAction->packageNames) {
        const auto &package = packages.find(packageName);
        if (package == packages.end()) {
            m_result.failedPackages.emplace_back(packageName, "package not listed in database file");
            continue;
        }
        auto packageLocation = db->locatePackage(package->second->computeFileName());
        if (packageLocation.error.has_value()) {
            m_result.failedPackages.emplace_back(
                packageName, argsToString("unable to locate package within repo directory: ", packageLocation.error.value().what()));
            continue;
        }
        if (!packageLocation.exists) {
            m_result.failedPackages.emplace_back(packageName, "package not present within repo directory");
            continue;
        }
        m_packageLocations.emplace_back(packageName, std::move(packageLocation), true);
    }
}

void PackageMovementAction::reportResultWithData(BuildActionResult result)
{
    auto buildLock = m_setup.building.lockToWrite();
    m_buildAction->resultData = std::move(m_result);
    reportResult(result);
}

RemovePackages::RemovePackages(ServiceSetup &setup, const std::shared_ptr<BuildAction> &buildAction)
    : PackageMovementAction(setup, buildAction)
{
}

void RemovePackages::run()
{
    if (!prepareRepoAction(RequiredDatabases::OneDestination)) {
        return;
    }

    // make list of package names to pass to repo-remove
    m_result.processedPackages.reserve(m_packageLocations.size());
    for (const auto &[packageName, packageLocation, ok] : m_packageLocations) {
        m_result.processedPackages.emplace_back(packageName);
    }

    // remove package from database file
    auto repoRemoveProcess = m_buildAction->makeBuildProcess("repo-remove", m_workingDirectory + "/repo-remove.log",
        std::bind(&RemovePackages::handleRepoRemoveResult, this, std::placeholders::_1, std::placeholders::_2));
    repoRemoveProcess->launch(
        boost::process::start_dir(m_destinationRepoDirectory), m_repoRemovePath, m_destinationDatabaseFile, m_result.processedPackages);
    m_buildAction->log()(Phrases::InfoMessage, "Invoking repo-remove within \"", m_destinationRepoDirectory, "\" for \"", m_destinationDatabaseFile,
        "\", see logfile for details\n");
}

void RemovePackages::handleRepoRemoveResult(boost::process::child &&child, ProcessResult &&result)
{
    CPP_UTILITIES_UNUSED(child)
    if (result.errorCode) {
        const auto errorCodeMessage = result.errorCode.message();
        const auto &errorMessage = result.error.empty() ? errorCodeMessage : result.error;
        m_result.errorMessage = "unable to remove packages: " + errorMessage;
        m_buildAction->log()(Phrases::ErrorMessage, "Unable to invoke repo-remove: ", errorMessage, '\n');
    } else if (result.exitCode != 0) {
        m_result.errorMessage = argsToString("unable to remove package: repo-remove returned with exit code ", result.exitCode);
        m_buildAction->log()(Phrases::ErrorMessage, "repo-remove invocation exited with non-zero exit code: ", result.exitCode, '\n');
    } else {
        movePackagesToArchive();
        return;
    }
    m_result.failedPackages.reserve(m_result.failedPackages.size() + m_result.processedPackages.size());
    for (auto &processedPackage : m_result.processedPackages) {
        m_result.failedPackages.emplace_back(std::move(processedPackage), "repo-remove error");
    }
    m_result.processedPackages.clear();
    reportResultWithData(BuildActionResult::Failure);
}

void RemovePackages::movePackagesToArchive()
{
    m_buildAction->log()(Phrases::InfoMessage, "Moving packages to archive directory");
    std::filesystem::path archivePath;
    auto processedPackageIterator = m_result.processedPackages.begin();
    for (const auto &[packageName, packageLocation, ok] : m_packageLocations) {
        try {
            archivePath = packageLocation.pathWithinRepo.parent_path() / "archive";
            std::filesystem::create_directory(archivePath);
            std::filesystem::rename(packageLocation.pathWithinRepo, archivePath / packageLocation.pathWithinRepo.filename());
            if (packageLocation.storageLocation.empty()) {
                continue;
            }
            // FIXME: The file at the storage location *might* still be used elsewhere. Better leave that to a repo cleanup task (to be implemented later).
            archivePath = packageLocation.storageLocation.parent_path() / "archive";
            std::filesystem::create_directory(archivePath);
            std::filesystem::rename(packageLocation.storageLocation, archivePath / packageLocation.storageLocation.filename());
            ++processedPackageIterator;
        } catch (const std::filesystem::filesystem_error &e) {
            processedPackageIterator = m_result.processedPackages.erase(processedPackageIterator);
            m_result.failedPackages.emplace_back(packageName, argsToString("unable to archive: ", e.what()));
        }
    }
    if (m_result.failedPackages.empty()) {
        reportResultWithData(BuildActionResult::Success);
        return;
    }
    m_result.errorMessage = argsToString("failed to remove ", m_result.failedPackages.size(), " packages");
    reportResultWithData(BuildActionResult::Failure);
}

MovePackages::MovePackages(ServiceSetup &setup, const std::shared_ptr<BuildAction> &buildAction)
    : PackageMovementAction(setup, buildAction)
{
}

void MovePackages::run()
{
    if (!prepareRepoAction(RequiredDatabases::OneSource | RequiredDatabases::OneDestination)) {
        return;
    }

    // copy packages from the source repo to the destination repo
    // make list of package names to pass to repo-add
    m_result.processedPackages.reserve(m_packageLocations.size());
    for (auto &[packageName, packageLocation, ok] : m_packageLocations) {
        try {
            if (packageLocation.storageLocation.empty()) {
                std::filesystem::copy_file(packageLocation.pathWithinRepo, m_destinationRepoDirectory / packageLocation.pathWithinRepo.filename());
            } else {
                const auto symlinkTarget = std::filesystem::read_symlink(packageLocation.pathWithinRepo);
                if (symlinkTarget.is_absolute()) {
                    ok = false;
                    m_result.failedPackages.emplace_back(packageName,
                        argsToString("unable to copy to destination repo: \"", packageLocation.pathWithinRepo,
                            "\" is a symlink with absolute target path (only relative target paths supported)"));
                    continue;
                }
                const auto newStorageLocation = m_destinationRepoDirectory / symlinkTarget;
                std::filesystem::create_directory(
                    newStorageLocation.parent_path()); // ensure the parent, e.g. the "any" directory exists; assume further parents already exist
                std::filesystem::copy(packageLocation.pathWithinRepo, m_destinationRepoDirectory / packageLocation.pathWithinRepo.filename(),
                    std::filesystem::copy_options::copy_symlinks);
                std::filesystem::copy_file(packageLocation.storageLocation, newStorageLocation);
            }
        } catch (const std::filesystem::filesystem_error &e) {
            ok = false;
            m_result.failedPackages.emplace_back(packageName, argsToString("unable to copy to destination repo: ", e.what()));
            continue;
        }
        m_fileNames.emplace_back(packageLocation.pathWithinRepo.filename());
        m_result.processedPackages.emplace_back(packageName);
    }

    // error-out early if not even a single package could be copied
    if (m_fileNames.empty()) {
        m_result.errorMessage = "none of the specified packages could be copied to the destination repo";
        reportResultWithData(BuildActionResult::Failure);
        return;
    }

    // conclude build action when both, repo-add and repo-remove have been exited and handled
    const auto processSession = MultiSession<void>::create(m_setup.building.ioContext, std::bind(&MovePackages::conclude, this));

    // add packages to database file of destination repo
    auto repoAddProcess = m_buildAction->makeBuildProcess("repo-add", m_workingDirectory + "/repo-add.log",
        std::bind(&MovePackages::handleRepoAddResult, this, processSession, std::placeholders::_1, std::placeholders::_2));
    repoAddProcess->launch(boost::process::start_dir(m_destinationRepoDirectory), m_repoAddPath, m_destinationDatabaseFile, m_fileNames);

    // remove package from database file of source repo
    auto repoRemoveProcess = m_buildAction->makeBuildProcess("repo-remove", m_workingDirectory + "/repo-remove.log",
        std::bind(&MovePackages::handleRepoRemoveResult, this, processSession, std::placeholders::_1, std::placeholders::_2));
    repoRemoveProcess->launch(boost::process::start_dir(m_sourceRepoDirectory), m_repoRemovePath, m_sourceDatabaseFile, m_result.processedPackages);

    m_buildAction->log()(ps(Phrases::InfoMessage), "Invoking repo-add within \"", m_destinationRepoDirectory, "\" for \"", m_destinationDatabaseFile,
        "\", see logfile for details\n", ps(Phrases::InfoMessage), "Invoking repo-remove within \"", m_sourceRepoDirectory, "\" for \"",
        m_sourceDatabaseFile, "\", see logfile for details\n");
}

void MovePackages::handleRepoRemoveResult(MultiSession<void>::SharedPointerType processSession, boost::process::child &&child, ProcessResult &&result)
{
    // handle error
    CPP_UTILITIES_UNUSED(processSession)
    CPP_UTILITIES_UNUSED(child)
    if (result.errorCode) {
        const auto errorCodeMessage = result.errorCode.message();
        const auto &errorMessage = result.error.empty() ? errorCodeMessage : result.error;
        m_result.errorMessage = "unable to remove packages: " + errorMessage;
        m_buildAction->log()(Phrases::ErrorMessage, "Unable to invoke repo-remove: ", errorMessage, '\n');
        return;
    } else if (result.exitCode != 0) {
        m_result.errorMessage = argsToString("unable to remove package: repo-remove returned with exit code ", result.exitCode);
        m_buildAction->log()(Phrases::ErrorMessage, "repo-remove invocation exited with non-zero exit code: ", result.exitCode, '\n');
        return;
    }

    // remove packages from source repo
    for (auto &[packageName, packageLocation, ok] : m_packageLocations) {
        // ignore packages which we've couldn't even copy to destination repo
        if (!ok) {
            continue;
        }
        // delete package within source repo; leave package at storage location because some other repo might still link to it
        try {
            std::filesystem::remove(packageLocation.pathWithinRepo);
        } catch (const std::runtime_error &e) {
            ok = false;
            m_result.failedPackages.emplace_back(packageName, argsToString("unable to remove from source repo: ", e.what()));
            m_result.processedPackages.erase(
                std::remove(m_result.processedPackages.begin(), m_result.processedPackages.end(), packageName), m_result.processedPackages.end());
        }
    }
}

void MovePackages::handleRepoAddResult(MultiSession<void>::SharedPointerType processSession, boost::process::child &&child, ProcessResult &&result)
{
    // handle error
    CPP_UTILITIES_UNUSED(processSession)
    CPP_UTILITIES_UNUSED(child)
    if (result.errorCode) {
        const auto errorCodeMessage = result.errorCode.message();
        const auto &errorMessage = result.error.empty() ? errorCodeMessage : result.error;
        m_addErrorMessage = "unable to add packages: " + errorMessage;
        m_buildAction->log()(Phrases::ErrorMessage, "Unable to invoke repo-add: ", errorMessage, '\n');
        return;
    } else if (result.exitCode != 0) {
        m_addErrorMessage = argsToString("unable to add packages: repo-add returned with exit code ", result.exitCode);
        m_buildAction->log()(Phrases::ErrorMessage, "repo-add invocation exited with non-zero exit code: ", result.exitCode, '\n');
        return;
    }

    // nothing more to do; packages have already been copied before invoking repo-add
}

void MovePackages::conclude()
{
    // check for errors
    const auto hasRepoRemoveError = !m_result.errorMessage.empty();
    const auto hasRepoAddError = !m_addErrorMessage.empty();
    std::string_view failureReason;
    if (hasRepoAddError && hasRepoRemoveError) {
        failureReason = "repo-add and repo-remove error";
        m_result.errorMessage = argsToString(m_result.errorMessage, ',', ' ', m_addErrorMessage);
    } else if (hasRepoAddError) {
        failureReason = "repo-add error";
        m_result.errorMessage = std::move(m_addErrorMessage);
    } else if (hasRepoRemoveError) {
        failureReason = "repo-remove error";
    }

    // report success if there are no repo-add/repo-remove errors or otherwise failed packages; otherwise report failure
    if (!hasRepoAddError && !hasRepoRemoveError) {
        if (m_result.errorMessage.empty() && !m_result.failedPackages.empty()) {
            m_result.errorMessage = argsToString("failed to move ", m_result.failedPackages.size(), " packages");
        }
        reportResultWithData(m_result.failedPackages.empty() ? BuildActionResult::Success : BuildActionResult::Failure);
        return;
    }

    // consider all packages failed if there are repo-add/repo-remove errors
    m_result.failedPackages.reserve(m_result.failedPackages.size() + m_result.processedPackages.size());
    for (auto &processedPackage : m_result.processedPackages) {
        m_result.failedPackages.emplace_back(std::move(processedPackage), failureReason);
    }
    m_result.processedPackages.clear();
    reportResultWithData(BuildActionResult::Failure);
}

CheckForProblems::CheckForProblems(ServiceSetup &setup, const std::shared_ptr<BuildAction> &buildAction)
    : InternalBuildAction(setup, buildAction)
{
}

void CheckForProblems::run()
{
    // initialize build action
    auto configReadLock = init(BuildActionAccess::ReadConfig, RequiredDatabases::OneOrMoreDestinations, RequiredParameters::None);
    if (std::holds_alternative<std::monostate>(configReadLock)) {
        return;
    }

    auto result = std::unordered_map<std::string, std::vector<RepositoryProblem>>();
    for (auto *const db : m_destinationDbs) {
        // check whether files exist
        auto &problems = result[db->name];
        try {
            if (db->path.empty() || !std::filesystem::is_regular_file(db->path)) {
                problems.emplace_back(RepositoryProblem{ .desc = "db file \"" % db->path + "\" is not a regular file" });
            }
            const auto filesPath = db->filesPath.empty() ? db->filesPathFromRegularPath() : db->filesPath;
            if (filesPath.empty() || !std::filesystem::is_regular_file(filesPath)) {
                problems.emplace_back(RepositoryProblem{ .desc = "files db file \"" % filesPath + "\" is not a regular file" });
            }
            if (db->localPkgDir.empty()) {
                goto checkForUnresolvedPackages; // skip checking for presence of package if the local package directory is not configured
            }
            if (!std::filesystem::is_directory(db->localPkgDir)) {
                problems.emplace_back(
                    RepositoryProblem{ .desc = "configured local package directory \"" % db->localPkgDir + "\" is not a directory" });
            }
            for (const auto &[pkgName, pkg] : db->packages) {
                if (!pkg->packageInfo) {
                    problems.emplace_back(RepositoryProblem{ .desc = "no package info present", .pkg = pkgName });
                    continue;
                }
                const auto packageLocation = db->locatePackage(pkg->packageInfo->fileName);
                if (!packageLocation.exists) {
                    problems.emplace_back(
                        RepositoryProblem{ .desc = "binary package \"" % pkg->packageInfo->fileName + "\" not present", .pkg = pkgName });
                }
            }
        } catch (const std::filesystem::filesystem_error &e) {
            problems.emplace_back(RepositoryProblem{ .desc = argsToString("unable to check presence of files: ", e.what()) });
        }
        // check for unresolved dependencies and missing libraries
    checkForUnresolvedPackages:
        auto unresolvedPackages
            = db->detectUnresolvedPackages(m_setup.config, std::vector<std::shared_ptr<LibPkg::Package>>(), LibPkg::DependencySet());
        for (auto &[package, unresolvedDeps] : unresolvedPackages) {
            problems.emplace_back(RepositoryProblem{ .desc = std::move(unresolvedDeps), .pkg = package->name });
        }
    }

    const auto buildLock = m_setup.building.lockToWrite();
    m_buildAction->resultData = std::move(result);
    reportResult(BuildActionResult::Success);
}

CleanRepository::CleanRepository(ServiceSetup &setup, const std::shared_ptr<BuildAction> &buildAction)
    : InternalBuildAction(setup, buildAction)
{
}

void CleanRepository::handleFatalError(InternalBuildAction::InitReturnType &init)
{
    init = std::monostate();
    m_buildAction->appendOutput(Phrases::ErrorMessage, "Cleanup aborted due to fatal errors\n");
    const auto buildLock = m_setup.building.lockToWrite();
    m_buildAction->resultData = std::move(m_messages);
    reportResult(BuildActionResult::Failure);
}

void CleanRepository::run()
{
    // initialize build action
    const auto flags = static_cast<CleanRepositoryFlags>(m_buildAction->flags);
    m_dryRun = flags & CleanRepositoryFlags::DryRun;
    m_buildAction->appendOutput(Phrases::InfoMessage, m_dryRun ? "Preparing cleanup, dry run\n" : "Preparing cleanup\n");
    auto configReadLock = init(BuildActionAccess::ReadConfig, RequiredDatabases::OneOrMoreDestinations, RequiredParameters::None);
    if (std::holds_alternative<std::monostate>(configReadLock)) {
        return;
    }

    // find relevant repository directories
    enum class RepoDirType {
        New,
        ArchSpecific,
        Any,
        Src,
    };
    struct RepoDir {
        std::filesystem::path canonicalPath;
        std::vector<std::pair<std::filesystem::path, std::string>> toArchive; // old packages not belonging to the DB anymore
        std::vector<std::filesystem::path> toDelete; // non-package junk files
        std::unordered_set<LibPkg::Database *> relevantDbs;
        RepoDirType type = RepoDirType::New;
    };
    std::unordered_map<std::string, RepoDir> repoDirs;
    bool fatalError = false;
    const auto addAnyAndSrcDir = [this, &repoDirs](LibPkg::Database &db) {
        // find the "any" directory which contains arch neutral packages which are possibly shared between databases
        try {
            auto anyPath = std::filesystem::canonical(db.localPkgDir + "/../any");
            auto &anyDir = repoDirs[anyPath.string()];
            if (anyDir.type == RepoDirType::New) {
                anyDir.type = RepoDirType::Any;
                anyDir.canonicalPath = std::move(anyPath);
            }
            anyDir.relevantDbs.emplace(&db);
        } catch (const std::filesystem::filesystem_error &e) {
            m_messages.errors.emplace_back("Unable to consider \"any\" dir of \"" % db.name % "\": " + e.what());
        }
        // find the "src" directory which contains source package
        try {
            auto srcPath = std::filesystem::canonical(db.localPkgDir + "/../src");
            auto &srcDir = repoDirs[srcPath.string()];
            if (srcDir.type == RepoDirType::New) {
                srcDir.type = RepoDirType::Src;
                srcDir.canonicalPath = std::move(srcPath);
            }
            srcDir.relevantDbs.emplace(&db);
        } catch (const std::filesystem::filesystem_error &e) {
            m_messages.errors.emplace_back("Unable to consider \"src\" dir of \"" % db.name % "\": " + e.what());
        }
    };
    for (auto *const db : m_destinationDbs) {
        if (db->localPkgDir.empty()) {
            m_messages.errors.emplace_back("Unable to clean \"" % db->name + "\": no local package directory configured");
            continue;
        }
        // find the "arch-specific" directory which contains the packages (or links to the them in the "any" directory) and the database files
        auto parentPath = std::filesystem::path();
        try {
            auto archSpecificPath = std::filesystem::canonical(db->localPkgDir);
            auto &archSpecificDir = repoDirs[archSpecificPath.string()];
            parentPath = archSpecificPath.parent_path();
            if (archSpecificDir.type == RepoDirType::New) {
                archSpecificDir.type = RepoDirType::ArchSpecific;
                archSpecificDir.canonicalPath = std::move(archSpecificPath);
            }
            archSpecificDir.relevantDbs.emplace(db);
        } catch (const std::filesystem::filesystem_error &e) {
            m_messages.errors.emplace_back("Unable consider \"arch-specific\" dir of \"" % db->name % "\": " + e.what());
        }
        // find the "any" and "src" directory
        addAnyAndSrcDir(*db);
        // require the parent path to be present
        if (parentPath.empty()) {
            fatalError = true;
            continue;
        }
        // find other directories next to the "arch-specific" package directory
        // note: These directories belong to other databases representing the same repository but for other architectures.
        try {
            for (const auto &otherDir : std::filesystem::directory_iterator(parentPath)) {
                if (!otherDir.is_directory() || otherDir.path() == db->localPkgDir || otherDir.path() == "any" || otherDir.path() == "src") {
                    continue;
                }
                repoDirs[otherDir.path().string()];
            }
        } catch (const std::filesystem::filesystem_error &e) {
            m_messages.errors.emplace_back("Unable consider find repositories next to \"" % db->name % "\": " + e.what());
            fatalError = true;
        }
    }
    if (fatalError) {
        handleFatalError(configReadLock);
        return;
    }

    // find relevant databases for repo dirs discovered in "find other directories next to …" step
    std::vector<std::unique_ptr<LibPkg::Database>> otherDbs;
    for (auto &[dirName, dirInfo] : repoDirs) {
        if (dirInfo.type != RepoDirType::New) {
            continue;
        }
        std::vector<std::string> dbFileNames;
        try {
            // find the database file
            dirInfo.canonicalPath = std::filesystem::canonical(dirName);
            for (const auto &repoItem : std::filesystem::directory_iterator(dirInfo.canonicalPath)) {
                if (!repoItem.is_regular_file() && !repoItem.is_symlink()) {
                    continue;
                }
                if (repoItem.path().extension() == ".db") {
                    dbFileNames.emplace_back(repoItem.path().filename());
                }
            }
            if (dbFileNames.empty()) {
                throw std::runtime_error("no *.db file present");
            }
            if (dbFileNames.size() > 1) {
                throw std::runtime_error("multiple/ambigous *.db files present: " + joinStrings(dbFileNames, ", "));
            }
            // initialize temporary database object for the repository
            auto &db
                = otherDbs.emplace_back(std::make_unique<LibPkg::Database>(dirName, argsToString(dirInfo.canonicalPath, '/', dbFileNames.front())));
            db->loadPackages();
            dirInfo.relevantDbs.emplace(db.get());
            // find the "any" and "src" directory
            db->localPkgDir = dirInfo.canonicalPath.string();
            addAnyAndSrcDir(*db);
            // consider the repository dir arch-specific
            dirInfo.type = RepoDirType::ArchSpecific;
        } catch (const std::runtime_error &e) {
            m_messages.errors.emplace_back("Unable read database file in repo dir \"" % dirName % "\": " + e.what());
            fatalError = true;
        }
    }
    if (fatalError) {
        handleFatalError(configReadLock);
        return;
    }

    // verify that each repo dir has at least one relevant database now
    for (auto &[dirName, dirInfo] : repoDirs) {
        if (dirInfo.relevantDbs.empty()) {
            m_messages.errors.emplace_back("Unable to associate a database with repo dir \"" % dirName + "\".");
            fatalError = true;
        }
    }
    if (fatalError) {
        handleFatalError(configReadLock);
        return;
    }

    // flag packages no longer referenced by any database for moving it to the archive folder; flag chunk files for deletion
    for (auto &[dirName, dirInfo] : repoDirs) {
        try {
            for (const auto &repoItem : std::filesystem::directory_iterator(dirInfo.canonicalPath)) {
                // skip directories and files which are not regular files or symlinks
                if (repoItem.is_directory() || repoItem.is_other()) {
                    continue;
                }

                // skip the database file itself
                const auto fileName = repoItem.path().filename().string();
                if (fileName.find(".db") != std::string::npos || fileName.find(".files") != std::string::npos) {
                    continue;
                }

                // delete other non-package files
                if (fileName.find(".pkg") == std::string::npos && fileName.find(".src") == std::string::npos) {
                    dirInfo.toDelete.emplace_back(repoItem);
                    continue;
                }

                // determine package name from file name
                const auto [packageName, error] = [&fileName] {
                    try {
                        const auto [name, version, arch] = LibPkg::Package::fileNameComponents(fileName);
                        return std::pair(std::string(name), false);
                    } catch (const std::runtime_error &e) {
                        return std::pair(std::string(e.what()), true);
                    }
                }();
                if (error) {
                    m_messages.warnings.emplace_back(
                        "Unable to parse package name of \"" % fileName % "\" (" % packageName + "). Not touching it to be safe.");
                    continue;
                }

                // check whether the file is still referenced by and relevant database and move it to archive if not
                auto fileStillReferenced = false;
                std::vector<std::string_view> actuallyReferencedFileNames;
                for (const auto *const db : dirInfo.relevantDbs) {
                    const auto i = db->packages.find(packageName);
                    if (i == db->packages.end()) {
                        continue;
                    }
                    const auto &pkg = i->second;
                    const auto &pkgInfo = pkg->packageInfo;
                    if (!pkgInfo || pkgInfo->fileName.empty()) {
                        m_messages.warnings.emplace_back(
                            "Database entry for package \"" % pkg->name % "\" misses the file name. Not touching \"" % fileName + "\" to be safe.");
                        fileStillReferenced = true;
                        continue;
                    }
                    if (pkgInfo->fileName == fileName) {
                        fileStillReferenced = true;
                        break;
                    } else {
                        actuallyReferencedFileNames.emplace_back(pkgInfo->fileName);
                    }
                }
                if (!fileStillReferenced) {
                    dirInfo.toArchive.emplace_back(
                        std::pair(repoItem, joinStrings<decltype(actuallyReferencedFileNames), std::string>(actuallyReferencedFileNames, ", ")));
                }
            }
        } catch (const std::filesystem::filesystem_error &e) {
            m_messages.errors.emplace_back("Unable to iterate though repo directory \"" % dirName % "\": " + e.what());
        }
    }

    configReadLock = std::monostate();

    // do the actual file system operations
    for (auto &[dirName, dirInfo] : repoDirs) {
        // skip source repos for now
        // note: So far we would get false-positives if pkgname does not contain pkgbase.
        if (dirInfo.type == RepoDirType::Src) {
            continue;
        }
        // delete files
        std::size_t processesItems = 0;
        for (auto &toDelete : dirInfo.toDelete) {
            try {
                if (!m_dryRun) {
                    std::filesystem::remove(toDelete);
                }
                ++processesItems;
                m_messages.notes.emplace_back("Deleted " + toDelete.string());
            } catch (const std::filesystem::filesystem_error &e) {
                m_messages.errors.emplace_back(argsToString("Unable to delete: ", e.what()));
            }
        }
        // archive files
        const auto archiveDir = dirInfo.canonicalPath / "archive";
        try {
            if (!std::filesystem::is_directory(archiveDir)) {
                std::filesystem::create_directory(archiveDir, dirInfo.canonicalPath);
            }
        } catch (const std::filesystem::filesystem_error &e) {
            m_messages.errors.emplace_back(argsToString("Unable to create archive directory: ", e.what()));
            continue;
        }
        for (const auto &[path, referencedPath] : dirInfo.toArchive) {
            try {
                if (!m_dryRun) {
                    std::filesystem::rename(path, archiveDir / path.filename());
                }
                ++processesItems;
                m_messages.notes.emplace_back(
                    "Archived " % path.string() % " (current version: " % (referencedPath.empty() ? "removed"sv : referencedPath) + ")");
            } catch (const std::filesystem::filesystem_error &e) {
                m_messages.errors.emplace_back(argsToString("Unable to archive: ", e.what()));
            }
        }
        m_buildAction->appendOutput(Phrases::InfoMessage, "Archived/deleted ", processesItems, " files in \"", dirName, '\"', '\n');
    }

    const auto buildLock = m_setup.building.lockToWrite();
    m_buildAction->resultData = std::move(m_messages);
    reportResult(m_messages.errors.empty() ? BuildActionResult::Success : BuildActionResult::Failure);
}

} // namespace LibRepoMgr