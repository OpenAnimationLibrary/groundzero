#include "automation_activity.h"

#ifdef __APPLE__
#include <Foundation/Foundation.h>
#endif

namespace GroundzeroAutomation {

void preventAppNap() {
#ifdef __APPLE__
  // Held, never ended: the process exits with the activity in place.
  static id activity = [[[NSProcessInfo processInfo]
      beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep |
                               NSActivityLatencyCritical
                        reason:@"Automation process answering its protocol"]
      retain];
  (void)activity;
#endif
}

}  // namespace GroundzeroAutomation
