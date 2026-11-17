#include "automation.h"

#include <utility>

namespace GroundzeroAutomation {

Error::Error(QString errorCode, QString errorMessage, QJsonObject errorDetails)
    : std::runtime_error(errorMessage.toStdString())
    , code(std::move(errorCode))
    , message(std::move(errorMessage))
    , details(std::move(errorDetails)) {}

}  // namespace GroundzeroAutomation
