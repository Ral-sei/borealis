/*
    Copyright 2019-2021 natinusala
    Copyright 2019 p-sam

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#pragma once

#ifndef BRLS_RESOURCES
#error BRLS_RESOURCES define missing
#endif

#include <string>

#ifdef PLATFORM_XBOX360
namespace brls {
// The Xbox 360 XDK CRT only accepts backslash path separators ("game:\..."),
// while borealis and wiliwili build resource paths with forward slashes.
// Normalize the fully concatenated path here so every BRLS_ASSET call site
// stays portable; call sites must pass the whole relative path to BRLS_ASSET
// instead of appending to its result.
inline std::string normalizeAssetPath(std::string path) {
    for (auto& c : path) {
        if (c == '/')
            c = '\\';
    }
    return path;
}
inline std::string resourcesDir() {
    static std::string s = normalizeAssetPath(BRLS_RESOURCES);
    return s;
}
inline std::string assetPath(const std::string& relativePath) {
    return normalizeAssetPath(resourcesDir() + relativePath);
}
}
#define BRLS_ASSET(_str) (::brls::assetPath(_str))
#else
#define BRLS_ASSET(_str) (std::string(BRLS_RESOURCES) + _str)
#endif
