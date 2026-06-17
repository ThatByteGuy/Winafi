#ifndef WINAFI_UPDATE_H
#define WINAFI_UPDATE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Compare two version strings in "X.Y.Z" format.
   Returns: < 0 if a < b, 0 if equal, > 0 if a > b.
   Handles missing components (e.g. "0.0.5" == "0.0.5.0").
   Returns -2 on invalid input (NULL or non-numeric). */
int update_version_compare(const char *a, const char *b);

/* Strip a leading 'v' from version string if present.
   Fills out_buf with the cleaned version, NUL-terminated.
   Returns 0 on success, -1 on NULL input or buffer too small. */
int update_strip_v(const char *version, char *out_buf, size_t out_size);

/* Build the GitHub release download URL for a given version tag.
   tag: e.g. "v4.1.0" or "4.1.0" (leading v is handled)
   out_buf: filled with URL like "https://github.com/USER/REPO/releases/tag/vX.Y.Z"
   Use: GITHUB_RELEASE_URL_FMT "https://github.com/AlphaGlider25/Winafi/releases/tag/%s"
   Returns 0 on success, -1 on error. */
int update_build_url(const char *tag, char *out_buf, size_t out_size);

/* Check for update against current version WINAFI_VERSION.
   Calls net_check_latest_version() internally.
   latest_out: filled with latest version string (caller provides, at least 64 bytes)
   Returns: 1 if update available (latest > current), 0 if up to date, -1 on network error */
int update_check(char *latest_out, size_t latest_out_size);

#ifdef __cplusplus
}
#endif
#endif
