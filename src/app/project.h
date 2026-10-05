/* app/project.h — app-facing forwarding header (Axis B4).
 *
 * Trust resolution and resources_discover consumption moved to
 * src/core/project.c so the strict no-app libagentc archive can link them.
 * This header keeps the historical include path working for the app and tests.
 */
#ifndef AGENTC_APP_PROJECT_H
#define AGENTC_APP_PROJECT_H

#include "core/project.h"

#endif /* AGENTC_APP_PROJECT_H */
