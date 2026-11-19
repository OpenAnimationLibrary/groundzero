#include "automation_activity.h"

#ifdef __APPLE__
#include <QCoreApplication>

#include <AppKit/AppKit.h>
#endif

namespace GroundzeroAutomation {

void prepareHeadlessProcess() {
#ifdef __APPLE__
  // Keep Qt's cocoa plugin from turning the process into a foreground
  // application, which it does for any bundle without LSUIElement; the editor
  // needs the bundle as it is. Children (scene hosts, helpers) inherit this.
  qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
  QCoreApplication::setAttribute(Qt::AA_PluginApplication);
  [[NSApplication sharedApplication]
      setActivationPolicy:NSApplicationActivationPolicyProhibited];
  // The registration domain lasts for this process only and changes nothing
  // the application stores.
  [[NSUserDefaults standardUserDefaults] registerDefaults:@{
    @"ApplePersistenceIgnoreState" : @YES,
    @"NSQuitAlwaysKeepsWindows" : @NO
  }];
  // Held, never ended: the process exits with the activity in place.
  static id activity = [[[NSProcessInfo processInfo]
      beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                        reason:@"Automation process answering its protocol"]
      retain];
  (void)activity;
#endif
}

}  // namespace GroundzeroAutomation
