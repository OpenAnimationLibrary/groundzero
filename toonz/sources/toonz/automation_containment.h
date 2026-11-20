#pragma once

namespace GroundzeroAutomation {

// Establish helper ownership before native services or tool dispatch can start.
// Return true for a verified anchor/guardian or an owned Windows Job.
// Raw POSIX returns false; malformed private supervision throws Error.
bool establishAutomationContainment();

}  // namespace GroundzeroAutomation
