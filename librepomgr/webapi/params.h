#ifndef LIBREPOMGR_WEBAPI_HELPER_H
#define LIBREPOMGR_WEBAPI_HELPER_H

#include "../global.h"

#include "./session.h"
#include "./typedefs.h"

#include <unordered_map>

namespace std {

template <> struct hash<boost::beast::string_view> {
    std::size_t operator()(boost::beast::string_view str) const noexcept
    {
        return std::_Hash_impl::hash(str.data(), str.length());
    }
};

} // namespace std

namespace LibRepoMgr {

struct ServiceSetup;

namespace WebAPI {

struct LIBREPOMGR_EXPORT BadRequest : std::runtime_error {
    BadRequest(const char *message);
};

inline BadRequest::BadRequest(const char *message)
    : std::runtime_error(message)
{
}

struct LIBREPOMGR_EXPORT Url {
    Url(const Request &request);
    std::string_view path;
    std::string_view hash;
    std::vector<std::pair<std::string_view, std::string_view>> params;

    bool hasFlag(std::string_view paramName) const;
    bool hasPrettyFlag() const;
    std::string_view value(std::string_view paramName) const;
    std::vector<std::string> decodeValues(std::string_view paramName) const;
    static std::string decodeValue(std::string_view value);
    static std::string encodeValue(std::string_view value);
};

inline bool Url::hasPrettyFlag() const
{
    return hasFlag("pretty");
}

struct LIBREPOMGR_EXPORT Params {
    Params(ServiceSetup &setup, Session &session);
    ServiceSetup &setup;
    Session &session;
    const Url target;

    template <typename FieldType> boost::beast::string_view headerValue(FieldType field) const;
    const Request &request() const;
};

inline Params::Params(ServiceSetup &setup, Session &session)
    : setup(setup)
    , session(session)
    , target(session.request())
{
}

inline const Request &Params::request() const
{
    return session.request();
}

template <typename FieldType> boost::beast::string_view Params::headerValue(FieldType field) const
{
    const auto fieldIterator(request().find(field));
    if (fieldIterator != request().cend()) {
        return fieldIterator->value();
    }
    return boost::beast::string_view();
}

} // namespace WebAPI
} // namespace LibRepoMgr

#endif // LIBREPOMGR_WEBAPI_HELPER_H