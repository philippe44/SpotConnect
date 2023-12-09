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
enum { HTTP_CACHE_MEM = 0, HTTP_CACHE_MEMFULL, HTTP_CACHE_DISK };

char* makeDLNA_ORG(const char* codec, bool fullCache, bool live);

#define HTTP_BASE_URL "/stream?id="

#ifdef __cplusplus
}
#endif
