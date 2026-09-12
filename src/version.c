/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include "version_git.h"
#include "config.h"

#ifdef VERSIONOVERRIDE
const char *appversion = VERSIONOVERRIDE;
#else
const char *appversion = VERSION_GIT;
#endif

#include "main.h"
#include <stdio.h>

/**
 * @brief Parse a semantic version string into a standardized 32-bit comparable integer.
 *
 * Deconstructs string format "major.minor.commit" using sscanf. Each component is scaled
 * by fixed decimal multipliers (10^7 for major, 10^5 for minor, 1 for commit/patch)
 * to provide a deterministic, monotonically increasing integer suitable for relational
 * version comparisons across plugin manifests and system queries.
 *
 * Algorithmic Complexity: O(1) time complexity, O(1) stack space complexity.
 *
 * @param str Null-terminated ASCII version string (e.g. "1.0", "1.0.0", "5.0.550").
 * @return 32-bit unsigned integer representing the composite version number.
 */
uint32_t
parse_version_int(const char *str)
{
  int major = 0;
  int minor = 0;
  int commit = 0;

  if(!str)
    return 0;

  /* Extract semantic components from input string */
  sscanf(str, "%d.%d.%d", &major, &minor, &commit);

  /* Compute packed decimal representation */
  return
    major * 10000000 +
    minor *   100000 +
    commit;
}

/**
 * @brief Retrieve current application version integer with backwards compatibility.
 *
 * Movian Next exposes a clean, user-facing version string ("1.0") across UI views,
 * settings screens, and GameOS PARAM.SFO metadata. To guarantee backwards compatibility
 * with existing third-party plugins that require a minimum Showtime/Movian version
 * (e.g., checking parse_version_int(pl->pl_app_min_version) <= app_get_version_int()
 * with min_version >= 5.0), this function applies a compatibility base offset when
 * the parsed major version is under 5, ensuring all legacy plugins load and install smoothly.
 *
 * Algorithmic Complexity: O(1) time complexity, O(1) stack space complexity.
 *
 * @return 32-bit unsigned integer encoding the compatible application version.
 */
uint32_t
app_get_version_int(void)
{
  /* Calculate raw integer corresponding to visible version string (e.g. 1.0 -> 10000000) */
  uint32_t ver = parse_version_int(appversion);

  /*
   * If the version integer evaluates below 50000000 (Movian 5.0 baseline),
   * apply a 50000000 compatibility offset (1.0 -> 60000000) so plugins requiring
   * Showtime/Movian 4.x or 5.x pass dependency checks without error.
   */
  if(ver < 50000000) {
    ver += 50000000;
  }

  return ver;
}

