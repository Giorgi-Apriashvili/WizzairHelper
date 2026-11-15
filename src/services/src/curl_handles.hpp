#pragma once
// RAII owners for libcurl handles (06 §1: RAII for every handle).

#include <curl/curl.h>

#include <memory>

namespace wizz::curl {

struct EasyDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};
struct MultiDeleter {
    void operator()(CURLM* handle) const noexcept { curl_multi_cleanup(handle); }
};
struct HeaderListDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};
struct UrlDeleter {
    void operator()(CURLU* url) const noexcept { curl_url_cleanup(url); }
};
struct StringDeleter {
    void operator()(char* text) const noexcept { curl_free(text); }
};

using Easy = std::unique_ptr<CURL, EasyDeleter>;
using Multi = std::unique_ptr<CURLM, MultiDeleter>;
using HeaderList = std::unique_ptr<curl_slist, HeaderListDeleter>;
using Url = std::unique_ptr<CURLU, UrlDeleter>;
using String = std::unique_ptr<char, StringDeleter>;

} // namespace wizz::curl
