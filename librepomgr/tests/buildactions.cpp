﻿#include "./parser_helper.h"

#include "../logging.h"
#include "../serversetup.h"

#include "../buildactions/buildaction.h"
#include "../buildactions/buildactionprivate.h"
#include "../buildactions/subprocess.h"

#include <c++utilities/conversion/stringconversion.h>
#include <c++utilities/io/ansiescapecodes.h>
#include <c++utilities/io/misc.h>
#include <c++utilities/io/path.h>
#include <c++utilities/tests/testutils.h>
using CppUtilities::operator<<; // must be visible prior to the call site

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include <c++utilities/tests/outputcheck.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/process/search_path.hpp>

#include <chrono>

using namespace std;
using namespace CPPUNIT_NS;
using namespace CppUtilities;
using namespace CppUtilities::Literals;

using namespace LibRepoMgr;

/*!
 * \brief The BuildActionsTests class contains tests for classes/functions related to build actions.
 */
class BuildActionsTests : public TestFixture {
    CPPUNIT_TEST_SUITE(BuildActionsTests);
    CPPUNIT_TEST(testLogging);
    CPPUNIT_TEST(testProcessSession);
    CPPUNIT_TEST(testBuildActionProcess);
    CPPUNIT_TEST(testBufferSearch);
    CPPUNIT_TEST(testParsingInfoFromPkgFiles);
    CPPUNIT_TEST(testPreparingBuild);
    CPPUNIT_TEST(testConductingBuild);
    CPPUNIT_TEST_SUITE_END();

public:
    BuildActionsTests();
    void setUp() override;
    void tearDown() override;

    void testLogging();
    void testProcessSession();
    void testBuildActionProcess();
    void testBufferSearch();
    void testParsingInfoFromPkgFiles();
    void testPreparingBuild();
    void testConductingBuild();

private:
    void loadBasicTestSetup();
    void loadTestConfig();
    void logTestSetup();
    template <typename InternalBuildActionType> void setupBuildAction();
    void resetBuildAction();
    void runBuildAction(const char *message, TimeSpan timeout = TimeSpan::fromSeconds(5));
    template <typename InternalBuildActionType> InternalBuildActionType *internalBuildAction();

    ServiceSetup m_setup;
    std::shared_ptr<BuildAction> m_buildAction;
    std::filesystem::path m_workingDir;
    double m_timeoutFactor = 0.0;
};

CPPUNIT_TEST_SUITE_REGISTRATION(BuildActionsTests);

BuildActionsTests::BuildActionsTests()
{
    if (const char *noBuildActionTimeout = std::getenv("BUILD_ACTION_TIMEOUT_FACTOR")) {
        m_timeoutFactor = stringToNumber<double>(noBuildActionTimeout);
    }
}

void BuildActionsTests::setUp()
{
    // save the working directory; the code under test might change it and we want to restore the initial working directory later
    m_workingDir = std::filesystem::current_path();
    cerr << EscapeCodes::Phrases::Info << "test working directory: " << m_workingDir.native() << endl;
}

void BuildActionsTests::tearDown()
{
    std::filesystem::current_path(m_workingDir);
}

/*!
 * \brief Assigns certain build variables to use fake scripts (instead of invoking e.g. the real makepkg).
 * \remarks The fake scripts are esentially no-ops which merely print the script name and the passed arguments.
 */
void BuildActionsTests::loadBasicTestSetup()
{
    m_setup.workingDirectory = TestApplication::instance()->workingDirectory();
    m_setup.building.workingDirectory = m_setup.workingDirectory + "/building";
    m_setup.building.makePkgPath = std::filesystem::absolute(testFilePath("scripts/fake_makepkg.sh"));
    m_setup.building.makeChrootPkgPath = std::filesystem::absolute(testFilePath("scripts/fake_makechrootpkg.sh"));
    m_setup.building.updatePkgSumsPath = std::filesystem::absolute(testFilePath("scripts/fake_updatepkgsums.sh"));
    m_setup.building.repoAddPath = std::filesystem::absolute(testFilePath("scripts/fake_repo_add.sh"));
    m_setup.configFilePath = std::filesystem::absolute(testFilePath("test-config/server.conf"));

    std::filesystem::remove_all(m_setup.workingDirectory);
    std::filesystem::create_directories(m_setup.building.workingDirectory);
}

/*!
 * \brief Runs the startup code almost like the actual service does.
 * \remarks Changes the current working directory! Make paths obtained via testFilePath() absolute before calling this function
 *          if they are supposed to be used later.
 */
void BuildActionsTests::loadTestConfig()
{
    m_setup.loadConfigFiles(false);
    m_setup.building.workingDirectory = m_setup.workingDirectory + "/building";
    m_setup.printDatabases();
    cerr << EscapeCodes::Phrases::Info << "current working directory: " << std::filesystem::current_path() << endl;
    cerr << EscapeCodes::Phrases::Info << "setup working directory: " << m_setup.workingDirectory << endl;
    logTestSetup();
}

/*!
 * \brief Logs all databases and packages of the current test setup.
 */
void BuildActionsTests::logTestSetup()
{
    for (const auto &db : m_setup.config.databases) {
        cout << EscapeCodes::Phrases::Info << "Packages of " << db.name << ':' << EscapeCodes::Phrases::End;
        for (const auto &[pkgName, pkg] : db.packages) {
            cout << " - " << pkgName << '\n';
        }
    }
    cout.flush();
}

/*!
 * \brief Initializes the fixture's build action.helper
 */
template <typename InternalBuildActionType> void BuildActionsTests::setupBuildAction()
{
    m_buildAction = std::make_shared<BuildAction>(0, &m_setup);
}

/*!
 * \brief Resets the fixture's build action.
 */
void BuildActionsTests::resetBuildAction()
{
    m_buildAction->status = BuildActionStatus::Created;
    m_buildAction->result = BuildActionResult::None;
    m_buildAction->resultData = std::string();
}

/*!
 * \brief Runs the fixture's build action (initialized via setupBuildAction()) until it has finished.
 * \param message The message for asserting whether the build action has finished yet.
 * \param timeout The max. time to wait for the build action to finish. It does not interrupt the handler which is currently
 *        executed (so tests can still get stuck).
 *
 */
void BuildActionsTests::runBuildAction(const char *message, CppUtilities::TimeSpan timeout)
{
    resetBuildAction();
    m_buildAction->start(m_setup);
    auto &ioc = m_setup.building.ioContext;
    ioc.restart();
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard = boost::asio::make_work_guard(ioc);
    m_buildAction->setConcludeHandler([&workGuard] { workGuard.reset(); });
    if (m_timeoutFactor == 0.0) {
        ioc.run();
    } else {
        ioc.run_for(std::chrono::microseconds(static_cast<std::chrono::microseconds::rep>((timeout * m_timeoutFactor).totalMicroseconds())));
    }
    CPPUNIT_ASSERT_EQUAL_MESSAGE(message, BuildActionStatus::Finished, m_buildAction->status);
}

/*!
 * \brief Returns the internal build action for the fixture's build action.
 * \remarks Invokes undefined behavior if \tp InternalBuildActionType is not the actual type.
 */
template <typename InternalBuildActionType> InternalBuildActionType *BuildActionsTests::internalBuildAction()
{
    auto *const internalBuildAction = m_buildAction->m_internalBuildAction.get();
    CPPUNIT_ASSERT_MESSAGE("internal build action assigned", internalBuildAction);
    return static_cast<InternalBuildActionType *>(internalBuildAction);
}

/*!
 * \brief Tests basic logging.
 */
void BuildActionsTests::testLogging()
{
    using namespace EscapeCodes;
    m_buildAction = make_shared<BuildAction>(0, &m_setup);
    {
        const auto stderrCheck = OutputCheck(
            [](const std::string &output) {
                TESTUTILS_ASSERT_LIKE_FLAGS(
                    "messages logged on stderr", ".*ERROR.*some error: message.*\n.*info.*\n.*", std::regex::extended, output);
            },
            cerr);
        m_buildAction->log()(Phrases::ErrorMessage, "some error: ", "message", '\n');
        m_buildAction->log()(Phrases::InfoMessage, "info", '\n');
    }
    CPPUNIT_ASSERT_EQUAL_MESSAGE("messages added to build action output",
        "\e[1;31m==> ERROR: \e[0m\e[1msome error: message\n\e[1;37m==> \e[0m\e[1minfo\n"s, m_buildAction->output);
}

/*!
 * \brief Tests the ProcessSession class (which is used to spawn processes within build actions capturing the output).
 */
void BuildActionsTests::testProcessSession()
{
    auto &ioc = m_setup.building.ioContext;
    auto session = std::make_shared<ProcessSession>(ioc, [&ioc](boost::process::child &&child, ProcessResult &&result) {
        CPP_UTILITIES_UNUSED(child)
        CPPUNIT_ASSERT_EQUAL(std::error_code(), result.errorCode);
        CPPUNIT_ASSERT_EQUAL(0, result.exitCode);
        CPPUNIT_ASSERT_EQUAL(std::string(), result.error);
        CPPUNIT_ASSERT_EQUAL("line1\nline2"s, result.output);
        ioc.stop();
    });
    session->launch(boost::process::search_path("echo"), "-n", "line1\nline2");
    session.reset();
    ioc.run();
}

/*!
 * \brief Tests the BuildProcessSession class (which is used to spawn processes within build actions creating a log file).
 */
void BuildActionsTests::testBuildActionProcess()
{
    m_buildAction = std::make_shared<BuildAction>(0, &m_setup);

    const auto scriptPath = testFilePath("scripts/print_some_data.sh");
    const auto logFilePath = std::filesystem::path(TestApplication::instance()->workingDirectory()) / "logfile.log";
    std::filesystem::create_directory(logFilePath.parent_path());
    if (std::filesystem::exists(logFilePath)) {
        std::filesystem::remove(logFilePath);
    }

    auto &ioc = m_setup.building.ioContext;
    auto session = std::make_shared<BuildProcessSession>(
        m_buildAction.get(), ioc, "test", std::string(logFilePath), [&ioc](boost::process::child &&child, ProcessResult &&result) {
            CPPUNIT_ASSERT_EQUAL(std::error_code(), result.errorCode);
            CPPUNIT_ASSERT_EQUAL(0, result.exitCode);
            CPPUNIT_ASSERT_GREATER(0, child.native_handle());
            ioc.stop();
        });
    session->launch(scriptPath);
    session.reset();
    ioc.run();

    auto logLines = splitString(readFile(logFilePath), "\r\n");
    CPPUNIT_ASSERT_EQUAL(5001_st, logLines.size());
    CPPUNIT_ASSERT_EQUAL("printing some numbers"s, logLines.front());
    CPPUNIT_ASSERT_EQUAL("line 5000"s, logLines.back());
    TESTUTILS_ASSERT_LIKE_FLAGS("PID logged", ".*test PID\\: [0-9]+.*\n.*"s, std::regex::extended, m_buildAction->output);
}

/*!
 * \brief Tests the BufferSearch class.
 */
void BuildActionsTests::testBufferSearch()
{
    // make a buffer
    BuildProcessSession::BufferPoolType bufferPool(30);
    auto buffer = bufferPool.newBuffer();

    // setup testing the search
    std::string expectedResult;
    bool hasResult = false;
    BufferSearch bs("Updated version: ", "\e\n", "Starting build", [&expectedResult, &hasResult](std::string &&result) {
        CPPUNIT_ASSERT_EQUAL(expectedResult, result);
        CPPUNIT_ASSERT_MESSAGE("callback only invoked once", !hasResult);
        hasResult = true;
    });

    // feed data into the search
    bs(buffer, 0);
    std::strcpy(buffer->data(), "Starting Updated");
    bs(buffer, 16);
    std::strcpy(buffer->data(), " version: some ");
    bs(buffer, 15);
    expectedResult = "some version number";
    std::strcpy(buffer->data(), "version number\emore chars");
    bs(buffer, 25);
    CPPUNIT_ASSERT(hasResult);
    std::strcpy(buffer->data(), "... Starting build ...");
    bs(buffer, 22);
}

/*!
 * \brief Tests the ReloadLibraryDependencies build action.
 */
void BuildActionsTests::testParsingInfoFromPkgFiles()
{
    // init config
    LibPkg::Config &config = m_setup.config;
    config.databases = { { "foo.db" }, { "bar.db" }, { "baz.db" } };

    // init db object
    LibPkg::Database &fooDb = config.databases[0];
    auto harfbuzz = fooDb.packages["mingw-w64-harfbuzz"] = LibPkg::Package::fromPkgFileName("mingw-w64-harfbuzz-1.4.2-1-any.pkg.tar.xz");
    auto syncthingtray = fooDb.packages["syncthingtray"] = LibPkg::Package::fromPkgFileName("syncthingtray-0.6.2-1-x86_64.pkg.tar.xz");
    fooDb.localPkgDir = directory(testFilePath("repo/foo/mingw-w64-harfbuzz-1.4.2-1-any.pkg.tar.xz"));
    LibPkg::Database &barDb = config.databases[1];
    auto cmake = barDb.packages["cmake"] = LibPkg::Package::fromPkgFileName("cmake-3.8.2-1-x86_64.pkg.tar.xz");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("origin", LibPkg::PackageOrigin::PackageFileName, cmake->origin);
    barDb.localPkgDir = directory(testFilePath("repo/bar/cmake-3.8.2-1-x86_64.pkg.tar.xz"));

    auto buildAction = std::make_shared<BuildAction>(0, &m_setup);
    auto reloadLibDependencies = ReloadLibraryDependencies(m_setup, buildAction);
    reloadLibDependencies.run();
    const auto &messages = std::get<BuildActionMessages>(buildAction->resultData);
    CPPUNIT_ASSERT_EQUAL(std::vector<std::string>(), messages.errors);
    CPPUNIT_ASSERT_EQUAL(std::vector<std::string>(), messages.warnings);
    CPPUNIT_ASSERT_EQUAL(std::vector<std::string>(), messages.notes);

    using namespace TestHelper;
    checkHarfbuzzPackagePeDependencies(*harfbuzz);
    checkSyncthingTrayPackageSoDependencies(*syncthingtray);
    checkCmakePackageSoDependencies(*cmake);

    const auto pkgsRequiringLibGCC = config.findPackagesProvidingLibrary("pe-i386::libgcc_s_sjlj-1.dll", true);
    CPPUNIT_ASSERT_EQUAL(1_st, pkgsRequiringLibGCC.size());
    CPPUNIT_ASSERT_EQUAL(harfbuzz, pkgsRequiringLibGCC.front().pkg);

    const auto pkgsProvidingLibSyncthingConnector = config.findPackagesProvidingLibrary("elf-x86_64::libsyncthingconnector.so.0.6.2", false);
    CPPUNIT_ASSERT_EQUAL(1_st, pkgsProvidingLibSyncthingConnector.size());
    CPPUNIT_ASSERT_EQUAL(syncthingtray, pkgsProvidingLibSyncthingConnector.front().pkg);
}

/*!
 * \brief Tests the PrepareBuild build action.
 */
void BuildActionsTests::testPreparingBuild()
{
    // get meta info
    auto &metaInfo = m_setup.building.metaInfo;
    const auto &typeInfo = metaInfo.typeInfoForId(BuildActionType::PrepareBuild);
    const auto pkgbuildsDirsSetting = std::string(typeInfo.settings[static_cast<std::size_t>(PrepareBuildSettings::PKGBUILDsDirs)].param);

    // load basic test setup and create build action
    loadBasicTestSetup();
    m_buildAction = std::make_shared<BuildAction>(0, &m_setup);
    m_buildAction->type = BuildActionType::PrepareBuild;
    m_buildAction->directory = "prepare-build-test";
    m_buildAction->flags = static_cast<BuildActionFlagType>(PrepareBuildFlags::CleanSrcDir);
    m_buildAction->settings[pkgbuildsDirsSetting] = std::filesystem::absolute(testDirPath("building/pkgbuilds"));
    m_buildAction->packageNames = { "boost", "mingw-w64-gcc" };

    // prepare test configuration
    // - Pretend all dependencies of boost are there except "zstd-1.4.5-1-x86_64.pkg.tar.zst" (so zstd is supposed pulled into the build automatically).
    // - There's a dummy package for zstd which incurs no further dependencies.
    // - The package mingw-w64-gcc is also just a dummy here to test handling variants; it has no dependencies here.
    loadTestConfig();
    auto coreDb = m_setup.config.findDatabase("core", "x86_64");
    CPPUNIT_ASSERT_MESSAGE("core db exists", coreDb);
    for (const auto pkgFileName :
        { "python-3.8.6-1-x86_64.pkg.tar.zst"sv, "python2-2.7.18-2-x86_64.pkg.tar.zst"sv, "bzip2-1.0.8-4-x86_64.pkg.tar.zst"sv,
            "findutils-4.7.0-2-x86_64.pkg.tar.xz"sv, "icu-67.1-1-x86_64.pkg.tar.zst"sv, "openmpi-4.0.5-2-x86_64.pkg.tar.zst"sv,
            "python-numpy-1.19.4-1-x86_64.pkg.tar.zst"sv, "python2-numpy-1.16.6-1-x86_64.pkg.tar.zst"sv, "zlib-1:1.2.11-4-x86_64.pkg.tar.xz"sv }) {
        coreDb->updatePackage(LibPkg::Package::fromPkgFileName(pkgFileName));
    }

    // run without destination database
    runBuildAction("prepare build without destination db");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("failure without destination db", BuildActionResult::Failure, m_buildAction->result);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "failure without destination db", "not exactly one destination database specified"s, std::get<std::string>(m_buildAction->resultData));

    // run with destination database (yes, the database is called "boost" in this test setup as well)
    m_buildAction->destinationDbs = { "boost" };
    runBuildAction("prepare build: successful preparation");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("success", BuildActionResult::Success, m_buildAction->result);
    CPPUNIT_ASSERT_MESSAGE("build preparation present", std::holds_alternative<BuildPreparation>(m_buildAction->resultData));
    const auto &buildPreparation = std::get<BuildPreparation>(m_buildAction->resultData);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("target db set", "boost"s, buildPreparation.targetDb);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("target arch set", "x86_64"s, buildPreparation.targetArch);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("staging db set", "boost-staging"s, buildPreparation.stagingDb);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no cyclic leftovers", 0_st, buildPreparation.cyclicLeftovers.size());
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no warnings", 0_st, buildPreparation.warnings.size());
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no error", std::string(), buildPreparation.error);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("manually ordered not set", false, buildPreparation.manuallyOrdered);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("db config has 2 dbs", 2_st, buildPreparation.dbConfig.size());
    CPPUNIT_ASSERT_EQUAL_MESSAGE("first db", "boost"s, buildPreparation.dbConfig[0].first);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("second db", "core"s, buildPreparation.dbConfig[1].first);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("staging db config has 3 dbs", 3_st, buildPreparation.stagingDbConfig.size());
    CPPUNIT_ASSERT_EQUAL_MESSAGE("first staging db", "boost-staging"s, buildPreparation.stagingDbConfig[0].first);
    const auto &batches = buildPreparation.batches;
    CPPUNIT_ASSERT_EQUAL_MESSAGE("two batches present", 2_st, batches.size());
    const auto expectedFirstBatch = std::vector<std::string>{ "mingw-w64-gcc", "zstd" };
    CPPUNIT_ASSERT_EQUAL_MESSAGE("first batch", expectedFirstBatch, batches[0]);
    const auto expectedSecondBatch = std::vector<std::string>{ "boost" };
    CPPUNIT_ASSERT_EQUAL_MESSAGE("second batch", expectedSecondBatch, batches[1]);
    CPPUNIT_ASSERT_MESSAGE(
        "build-preparation.json created", std::filesystem::is_regular_file("building/build-data/prepare-build-test/build-preparation.json"));
    CPPUNIT_ASSERT_MESSAGE(
        "build-progress.json created", std::filesystem::is_regular_file("building/build-data/prepare-build-test/build-progress.json"));
    for (const auto pkg : { "boost"sv, "mingw-w64-gcc"sv, "zstd"sv }) {
        CPPUNIT_ASSERT_MESSAGE(
            "PKGBUILD for " % pkg + " created", std::filesystem::is_regular_file("building/build-data/prepare-build-test/" % pkg + "/src/PKGBUILD"));
    }
}

/*!
 * \brief Tests the ConductBuild build action.
 */
void BuildActionsTests::testConductingBuild()
{
    // load basic test setup and create build action
    loadBasicTestSetup();
    m_buildAction = std::make_shared<BuildAction>(0, &m_setup);
    m_buildAction->type = BuildActionType::ConductBuild;
    m_buildAction->directory = "conduct-build-test";
    m_buildAction->packageNames = { "boost" };
    m_buildAction->flags = static_cast<BuildActionFlagType>(ConductBuildFlags::BuildAsFarAsPossible | ConductBuildFlags::SaveChrootOfFailures
        | ConductBuildFlags::UpdateChecksums | ConductBuildFlags::AutoStaging);

    // run without build preparation
    runBuildAction("conduct build without build preparation");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("failure without preparation JSON", BuildActionResult::Failure, m_buildAction->result);
    TESTUTILS_ASSERT_LIKE(
        "no preparation JSON", "Unable to restore build-preparation.json:.*not exist.*", std::get<std::string>(m_buildAction->resultData));

    // create fake build preparation
    const auto origPkgbuildFile = workingCopyPathAs("building/build-data/conduct-build-test/boost/src/PKGBUILD", "orig-src-dir/boost/PKGBUILD");
    const auto origSourceDir = std::filesystem::absolute(directory(origPkgbuildFile));
    auto prepData = readFile(testFilePath("building/build-data/conduct-build-test/build-preparation.json"));
    findAndReplace(prepData, "$ORIGINAL_SOURCE_DIRECTORY", origSourceDir.native());
    findAndReplace(prepData, "$TEST_FILES_PATH", "TODO");
    const auto buildDir = std::filesystem::absolute(workingCopyPath("building", WorkingCopyMode::NoCopy));
    const auto prepFile
        = std::filesystem::absolute(workingCopyPath("building/build-data/conduct-build-test/build-preparation.json", WorkingCopyMode::NoCopy));
    writeFile(prepFile.native(), prepData);
    auto progressData = readFile(testFilePath("building/build-data/conduct-build-test/build-progress.json"));
    const auto progressFile
        = std::filesystem::absolute(workingCopyPath("building/build-data/conduct-build-test/build-progress.json", WorkingCopyMode::NoCopy));
    writeFile(progressFile.native(), progressData);
    std::filesystem::copy(testDirPath("building/build-data/conduct-build-test/boost"),
        m_setup.workingDirectory + "/building/build-data/conduct-build-test/boost", std::filesystem::copy_options::recursive);

    // run without chroot configuration
    runBuildAction("conduct build without chroot configuration");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("failure without chroot configuration", BuildActionResult::Failure, m_buildAction->result);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "no chroot configuration", "The chroot directory is not configured."s, std::get<std::string>(m_buildAction->resultData));

    // configure chroot directory
    m_setup.building.chrootDir = testDirPath("test-config/chroot-dir");

    // run with misconfigured destination db
    runBuildAction("conduct build with misconfigured destination db (1)");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("failure without destination db (1)", BuildActionResult::Failure, m_buildAction->result);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("destination db missing (1)",
        "Auto-staging is enabled but the staging database \"boost-staging@x86_64\" specified in build-preparation.json can not be found."s,
        std::get<std::string>(m_buildAction->resultData));
    loadTestConfig();
    runBuildAction("conduct build with misconfigured destination db (2)");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("failure without destination db (2)", BuildActionResult::Failure, m_buildAction->result);
    TESTUTILS_ASSERT_LIKE("destination db missing (2)", "Destination repository \"repos/boost/os/x86_64\" does not exist.*"s,
        std::get<std::string>(m_buildAction->resultData));

    // create repositories
    const auto reposPath = testDirPath("test-config/repos");
    const auto reposWorkingCopyPath = std::filesystem::path(m_setup.workingDirectory + "/repos");
    std::filesystem::create_directory(reposWorkingCopyPath);
    std::filesystem::copy(reposPath, reposWorkingCopyPath, std::filesystem::copy_options::recursive);

    // run without chroot directory
    runBuildAction("conduct build without chroot directory");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no chroot directory: results in failure", BuildActionResult::Failure, m_buildAction->result);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no chroot directory: result data states affected packages", "failed to build packages: boost"s,
        std::get<std::string>(m_buildAction->resultData));
    auto *internalData = internalBuildAction<ConductBuild>();
    TESTUTILS_ASSERT_LIKE("no chroot directory: package-level error message",
        "Chroot directory \".*/test-config/chroot-dir/arch-x86_64/root\" is no directory."s,
        internalData->m_buildProgress.progressByPackage["boost"].error);

    // create chroot directory
    const auto chrootSkelPath = testDirPath("test-config/chroot-skel");
    const auto chrootDirWorkingCopyPath = std::filesystem::path(m_setup.workingDirectory + "/chroot-dir");
    const auto rootChrootWorkingCopyPath = chrootDirWorkingCopyPath / "arch-x86_64/root";
    std::filesystem::create_directory(chrootDirWorkingCopyPath);
    std::filesystem::copy(m_setup.building.chrootDir, chrootDirWorkingCopyPath, std::filesystem::copy_options::recursive);
    std::filesystem::create_directories(rootChrootWorkingCopyPath);
    std::filesystem::copy(chrootSkelPath, rootChrootWorkingCopyPath, std::filesystem::copy_options::recursive);
    m_setup.building.chrootDir = chrootDirWorkingCopyPath.string(); // assign the created chroot directory
    writeFile(progressFile.native(), progressData); // reset "build-progress.json" so the new chroot directory is actually used

    // run without having makepkg actually producing any packages
    runBuildAction("conduct build without producing any packages");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no packages produced: results in failure", BuildActionResult::Failure, m_buildAction->result);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no packages produced: result data states affected packages", "failed to build packages: boost"s,
        std::get<std::string>(m_buildAction->resultData));
    internalData = internalBuildAction<ConductBuild>();
    TESTUTILS_ASSERT_LIKE("no packages produced: package-level error message",
        "not all.*packages exist.*boost-1.73.0-1.src.tar.gz.*boost-libs-1\\.73\\.0-1-x86_64\\.pkg\\.tar\\.zst.*boost-1\\.73\\.0-1-x86_64\\.pkg\\.tar\\.zst"s,
        internalData->m_buildProgress.progressByPackage["boost"].error);
    CPPUNIT_ASSERT_MESSAGE(
        "no packages produced: package considered finished", !internalData->m_buildProgress.progressByPackage["boost"].finished.isNull());
    CPPUNIT_ASSERT_MESSAGE("no packages produced: package not added to repo", !internalData->m_buildProgress.progressByPackage["boost"].addedToRepo);

    // prepare some actual packages
    std::filesystem::copy(testFilePath("test-config/fake-build-artefacts/boost-1.73.0-1.src.tar.gz"),
        buildDir / "build-data/conduct-build-test/boost/pkg/boost-1.73.0-1.src.tar.gz");
    std::filesystem::copy(testFilePath("test-config/fake-build-artefacts/boost-1.73.0-1-x86_64.pkg.tar.zst"),
        buildDir / "build-data/conduct-build-test/boost/pkg/boost-1.73.0-1-x86_64.pkg.tar.zst");
    std::filesystem::copy(testFilePath("test-config/fake-build-artefacts/boost-libs-1.73.0-1-x86_64.pkg.tar.zst"),
        buildDir / "build-data/conduct-build-test/boost/pkg/boost-libs-1.73.0-1-x86_64.pkg.tar.zst");

    // conduct build without staging
    runBuildAction("conduct build without staging");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no staging needed: success", BuildActionResult::Success, m_buildAction->result);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no staging needed: no result data present", ""s, std::get<std::string>(m_buildAction->resultData));
    internalData = internalBuildAction<ConductBuild>();
    CPPUNIT_ASSERT_MESSAGE("no staging needed: rebuild list empty", internalData->m_buildProgress.rebuildList.empty());
    CPPUNIT_ASSERT_MESSAGE(
        "no staging needed: package considered finished", !internalData->m_buildProgress.progressByPackage["boost"].finished.isNull());
    CPPUNIT_ASSERT_MESSAGE("no staging needed: package added to repo", internalData->m_buildProgress.progressByPackage["boost"].addedToRepo);

    // check whether log files have been created accordingly
    CPPUNIT_ASSERT_EQUAL_MESSAGE("no staging needed: download log", "fake makepkg: -f --nodeps --nobuild --source\n"s,
        readFile("building/build-data/conduct-build-test/boost/pkg/download.log"));
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "no staging needed: updpkgsums log", "fake updatepkgsums: \n"s, readFile("building/build-data/conduct-build-test/boost/pkg/updpkgsums.log"));
    TESTUTILS_ASSERT_LIKE("no staging needed: build log", "fake makechrootpkg: -c -u -C  -r .*chroot-dir/arch-x86_64 -l buildservice --\n"s,
        readFile("building/build-data/conduct-build-test/boost/pkg/build.log"));
    TESTUTILS_ASSERT_LIKE("no staging needed: repo-add log",
        "fake repo-add: boost.db.tar.zst boost(-libs)?-1\\.73\\.0-1-x86_64.pkg.tar.zst boost(-libs)?-1\\.73\\.0-1-x86_64.pkg.tar.zst\n"s,
        readFile("building/build-data/conduct-build-test/boost/pkg/repo-add.log"));

    // check whether packages have actually been added to repo
    CPPUNIT_ASSERT_MESSAGE(
        "no staging needed: package added to repo (0)", std::filesystem::is_regular_file("repos/boost/os/src/boost-1.73.0-1.src.tar.gz"));
    CPPUNIT_ASSERT_MESSAGE(
        "no staging needed: package added to repo (1)", std::filesystem::is_regular_file("repos/boost/os/x86_64/boost-1.73.0-1-x86_64.pkg.tar.zst"));
    CPPUNIT_ASSERT_MESSAGE("no staging needed: package added to repo (2)",
        std::filesystem::is_regular_file("repos/boost/os/x86_64/boost-libs-1.73.0-1-x86_64.pkg.tar.zst"));

    // add packages needing a rebuild to trigger auto-staging
    m_setup.config.loadAllPackages(false);
    auto *const boostDb = m_setup.config.findDatabase("boost"sv, "x86_64"sv);
    auto *const miscDb = m_setup.config.findDatabase("misc"sv, "x86_64"sv);
    CPPUNIT_ASSERT_MESSAGE("boost database present", boostDb);
    CPPUNIT_ASSERT_MESSAGE("misc database present", miscDb);
    auto &boostLibsPackage = boostDb->packages["boost-libs"];
    boostLibsPackage->libprovides = { "elf-x86_64::libboost_regex.so.1.72.0" };
    boostLibsPackage->libdepends = { "elf-x86_64::libstdc++.so.6" };
    boostDb->forceUpdatePackage(boostLibsPackage);
    auto &sourceHighlightPackage = miscDb->packages["source-highlight"];
    sourceHighlightPackage->libprovides = { "elf-x86_64::libsource-highlight.so.4" };
    sourceHighlightPackage->libdepends
        = { "elf-x86_64::libboost_regex.so.1.72.0", "elf-x86_64::libsource-highlight.so.4", "elf-x86_64::libstdc++.so.6" };
    miscDb->forceUpdatePackage(sourceHighlightPackage);
    m_setup.printDatabases();
    logTestSetup();

    // conduct build with staging
    writeFile(progressFile.native(), progressData); // reset "build-progress.json" so the package is re-considered
    runBuildAction("conduct build with staging");
    //CPPUNIT_ASSERT_EQUAL_MESSAGE("staging needed: success", BuildActionResult::Success, m_buildAction->result);
    CPPUNIT_ASSERT_EQUAL_MESSAGE("staging needed: no result data present", ""s, std::get<std::string>(m_buildAction->resultData));
    internalData = internalBuildAction<ConductBuild>();
    const auto &rebuildList = internalData->m_buildProgress.rebuildList;
    const auto rebuildInfoForMisc = rebuildList.find("misc");
    CPPUNIT_ASSERT_EQUAL_MESSAGE("staging needed: rebuild list contains 1 database", 1_st, rebuildList.size());
    CPPUNIT_ASSERT_MESSAGE("staging needed: rebuild info for misc present", rebuildInfoForMisc != rebuildList.end());
    const auto rebuildInfoForSourceHighlight = rebuildInfoForMisc->second.find("source-highlight");
    const auto expectedLibprovides = std::vector<std::string>{ "elf-x86_64::libboost_regex.so.1.72.0" };
    CPPUNIT_ASSERT_MESSAGE(
        "staging needed: rebuild info for source-highlight present", rebuildInfoForSourceHighlight != rebuildInfoForMisc->second.end());
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "staging needed: libprovides for source-highlight present", expectedLibprovides, rebuildInfoForSourceHighlight->second.libprovides);

    // check whether log files have been created accordingly
    TESTUTILS_ASSERT_LIKE("no staging needed: repo-add log",
        "fake repo-add: boost-staging.db.tar.zst boost(-libs)?-1\\.73\\.0-1-x86_64.pkg.tar.zst boost(-libs)?-1\\.73\\.0-1-x86_64.pkg.tar.zst\n"s,
        readFile("building/build-data/conduct-build-test/boost/pkg/repo-add.log"));

    // check whether package have been added to staging repo
    CPPUNIT_ASSERT_MESSAGE(
        "staging needed: package added to repo (0)", std::filesystem::is_regular_file("repos/boost-staging/os/src/boost-1.73.0-1.src.tar.gz"));
    CPPUNIT_ASSERT_MESSAGE("staging needed: package added to repo (1)",
        std::filesystem::is_regular_file("repos/boost-staging/os/x86_64/boost-1.73.0-1-x86_64.pkg.tar.zst"));
    CPPUNIT_ASSERT_MESSAGE("staging needed: package added to repo (2)",
        std::filesystem::is_regular_file("repos/boost-staging/os/x86_64/boost-libs-1.73.0-1-x86_64.pkg.tar.zst"));
}