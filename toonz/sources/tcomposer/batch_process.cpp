#include "batch_process.h"

#ifdef __APPLE__
#include <Foundation/Foundation.h>

void ignoreWindowStateRestoration() {
  [[NSUserDefaults standardUserDefaults] registerDefaults:@{
    @"ApplePersistenceIgnoreState" : @YES,
    @"NSQuitAlwaysKeepsWindows" : @NO
  }];
}
#else
void ignoreWindowStateRestoration() {}
#endif
