/* 
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#pragma once

enum { HTTP_CL_CHUNKED = -3, HTTP_CL_NONE = -1, HTTP_CL_REAL = 0 };

struct HTTPheaderList {
	struct HTTPheaderList* next;
	char* key, * value;
};

#define HTTP_BASE_URL "/stream?id="
