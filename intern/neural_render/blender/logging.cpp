/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "blender/NR_api.h"
#include "util/log.h"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"neural_render"};

void NR_log_init()
{
  /* Set callback to pass log messages to CLOG. */
  nr::log_init(
      [](const nr::LogLevel level, const char *file_line, const char *func, const char *msg) {
        const CLG_LogType *log_type = CLOG_ENSURE(&LOG);
        switch (level) {
          case nr::LOG_LEVEL_FATAL:
          case nr::LOG_LEVEL_DFATAL:
            CLG_log_str(log_type, CLG_LEVEL_FATAL, file_line, func, msg);
            return;
          case nr::LOG_LEVEL_ERROR:
          case nr::LOG_LEVEL_DERROR:
            CLG_log_str(log_type, CLG_LEVEL_ERROR, file_line, func, msg);
            return;
          case nr::LOG_LEVEL_WARNING:
          case nr::LOG_LEVEL_DWARNING:
            CLG_log_str(log_type, CLG_LEVEL_WARN, file_line, func, msg);
            return;
          case nr::LOG_LEVEL_INFO:
          case nr::LOG_LEVEL_INFO_IMPORTANT:
            CLG_log_str(log_type, CLG_LEVEL_INFO, file_line, func, msg);
            return;
          case nr::LOG_LEVEL_DEBUG:
            CLG_log_str(log_type, CLG_LEVEL_DEBUG, file_line, func, msg);
            return;
          case nr::LOG_LEVEL_TRACE:
          case nr::LOG_LEVEL_UNKNOWN:
            CLG_log_str(log_type, CLG_LEVEL_TRACE, file_line, func, msg);
            return;
        }
      });

  /* Map log level from CLOG. */
  const CLG_LogType *log_type = CLOG_ENSURE(&LOG);
  switch (log_type->level) {
    case CLG_LEVEL_FATAL:
      nr::log_level_set(nr::LOG_LEVEL_FATAL);
      break;
    case CLG_LEVEL_ERROR:
      nr::log_level_set(nr::LOG_LEVEL_ERROR);
      break;
    case CLG_LEVEL_WARN:
      nr::log_level_set(nr::LOG_LEVEL_WARNING);
      break;
    case CLG_LEVEL_INFO:
      nr::log_level_set(nr::LOG_LEVEL_INFO);
      break;
    case CLG_LEVEL_DEBUG:
      nr::log_level_set(nr::LOG_LEVEL_DEBUG);
      break;
    case CLG_LEVEL_TRACE:
      nr::log_level_set(nr::LOG_LEVEL_TRACE);
      break;
  }
}

}  // namespace blender
