#pragma once

// Compatibility wrapper for the older curl headers shipped with bili360.
// CPR expects curl_mime and curl/system.h from newer libcurl releases.
#include_next <curl/curl.h>

struct curl_mime;
