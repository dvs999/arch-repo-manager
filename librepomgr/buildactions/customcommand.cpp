#include "./buildactionprivate.h"
#include "./subprocess.h"

#include "../serversetup.h"

#include <c++utilities/io/ansiescapecodes.h>

#include <boost/process/search_path.hpp>
#include <boost/process/start_dir.hpp>

using namespace std;
using namespace CppUtilities;
using namespace CppUtilities::EscapeCodes;

namespace LibRepoMgr {

CustomCommand::CustomCommand(ServiceSetup &setup, const std::shared_ptr<BuildAction> &buildAction)
    : InternalBuildAction(setup, buildAction)
{
}

void CustomCommand::run()
{
    // validate and read parameter/settings
    if (auto error = validateParameter(RequiredDatabases::None, RequiredParameters::None); !error.empty()) {
        reportError(move(error));
        return;
    }
    if (m_buildAction->directory.empty()) {
        reportError("No directory specified.");
        return;
    }
    auto &metaInfo = m_setup.building.metaInfo;
    auto metaInfoLock = metaInfo.lockToRead();
    const auto &typeInfo = metaInfo.typeInfoForId(BuildActionType::CustomCommand);
    const auto commandSetting = typeInfo.settings[static_cast<std::size_t>(CustomCommandSettings::Command)].param;
    metaInfoLock.unlock();
    const auto &command = findSetting(commandSetting);
    if (command.empty()) {
        reportError("No command specified.");
        return;
    }

    // prepare working dir
    try {
        m_workingDirectory = determineWorkingDirectory(customCommandsWorkingDirectory);
        if (!std::filesystem::is_directory(m_workingDirectory)) {
            std::filesystem::create_directories(m_workingDirectory);
        }
    } catch (const std::filesystem::filesystem_error &e) {
        reportError(argsToString("Unable to create working directory: ", e.what()));
        return;
    }

    m_buildAction->appendOutput(Phrases::InfoMessage, "Running custom command: ", command, '\n');

    // launch process, pass finish handler
    auto process
        = m_buildAction->makeBuildProcess("command", m_workingDirectory + "/the.log", [this](boost::process::child &&, ProcessResult &&result) {
              if (result.errorCode) {
                  m_buildAction->appendOutput(Phrases::InfoMessage, "Unable to invoke command: ", result.errorCode.message());
                  reportError(result.errorCode.message());
                  return;
              }
              m_buildAction->appendOutput(
                  result.exitCode == 0 ? Phrases::InfoMessage : Phrases::ErrorMessage, "Command exited with return code ", result.exitCode);
              if (result.exitCode != 0) {
                  reportError(argsToString("non-zero exit code ", result.exitCode));
                  return;
              }
              const auto buildLock = m_setup.building.lockToWrite();
              reportSuccess();
          });
    process->launch(boost::process::start_dir(m_workingDirectory), boost::process::search_path("bash"), "-ec", command);
}

} // namespace LibRepoMgr