/* 
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { HTTP_CL_CHUNKED = -3, HTTP_CL_KNOWN = -2, HTTP_CL_NONE = -1, HTTP_CL_REAL = 0 };

/* Mode 0 works the best because it is still in memory and lasts forever because there 
 * are very little risks that a player request super old ranges (over 8MB) so it's 
 * almost virtually as a fisdk. But I don' know, some players might have weird requests 
 * like (to receieve the same track since the begining but using HTTP only */

enum { HTTP_CACHE_MEM = 0, HTTP_CACHE_INFINITE, HTTP_CACHE_DISK };

char* makeDLNA_ORG(const char* codec, bool fullCache, bool live);

#define HTTP_BASE_URL "/spotupnp"

#ifdef __cplusplus
}
#endif
