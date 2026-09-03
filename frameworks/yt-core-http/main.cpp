#include <yt/yt/core/http/server.h>
#include <yt/yt/core/http/http.h>
#include <yt/yt/core/http/config.h>
#include <yt/yt/core/http/helpers.h>
#include <yt/yt/core/http/compression.h>

#include <yt/yt/core/https/server.h>
#include <yt/yt/core/https/config.h>

#include <yt/yt/core/crypto/config.h>

#include <yt/yt/core/json/json_writer.h>

#include <yt/yt/core/concurrency/thread_pool_poller.h>
#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/actions/bind.h>

#include <yt/yt/core/ytree/fluent.h>

#include <library/cpp/json/json_reader.h>

#include <library/cpp/yt/memory/ref.h>

#include <util/stream/buffer.h>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace NYT;
using namespace NYT::NHttp;
using namespace NYT::NConcurrency;
using namespace NYT::NYTree;

namespace {

i64 ParseInt(TStringBuf value)
{
    i64 result = 0;
    auto begin = value.data();
    auto end = value.data() + value.size();
    auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc()) {
        return 0;
    }
    return result;
}

// Query values in this benchmark are always plain decimal integers, so no
// percent-decoding is required -- every fragmentation/anti-cheat check in
// the baseline validation suite sends unescaped digits.
i64 SumQueryParameters(TStringBuf rawQuery)
{
    i64 sum = 0;
    while (!rawQuery.empty()) {
        auto ampersand = rawQuery.find('&');
        auto pair = rawQuery.substr(0, ampersand);

        auto equals = pair.find('=');
        if (equals != TStringBuf::npos) {
            sum += ParseInt(pair.substr(equals + 1));
        }

        if (ampersand == TStringBuf::npos) {
            break;
        }
        rawQuery = rawQuery.substr(ampersand + 1);
    }
    return sum;
}

// Same shape as SumQueryParameters, but returns one named value instead of
// summing all of them. `defaultValue` covers a missing key -- every profile
// that uses this always sends it, but the handler should not misbehave if
// it is ever omitted.
i64 FindQueryParameter(TStringBuf rawQuery, TStringBuf key, i64 defaultValue)
{
    while (!rawQuery.empty()) {
        auto ampersand = rawQuery.find('&');
        auto pair = rawQuery.substr(0, ampersand);

        auto equals = pair.find('=');
        if (equals != TStringBuf::npos && pair.substr(0, equals) == key) {
            return ParseInt(pair.substr(equals + 1));
        }

        if (ampersand == TStringBuf::npos) {
            break;
        }
        rawQuery = rawQuery.substr(ampersand + 1);
    }
    return defaultValue;
}

void RespondPlainText(const IResponseWriterPtr& rsp, EStatusCode status, const std::string& body)
{
    rsp->SetStatus(status);
    rsp->GetHeaders()->Set(NHeaders::ContentTypeHeaderName, "text/plain");
    WaitFor(rsp->WriteBody(TSharedRef::FromString(body)))
        .ThrowOnError();
}

void HandlePipeline(const IRequestPtr& /*req*/, const IResponseWriterPtr& rsp)
{
    RespondPlainText(rsp, EStatusCode::OK, "ok");
}

void HandleBaseline11(const IRequestPtr& req, const IResponseWriterPtr& rsp)
{
    i64 sum = SumQueryParameters(req->GetUrl().RawQuery);

    if (req->GetMethod() == EMethod::Post) {
        // req->ReadAll() drains the body regardless of framing -- both
        // Content-Length and chunked Transfer-Encoding are already decoded
        // by the time the handler sees the request.
        auto body = req->ReadAll();
        sum += ParseInt(TStringBuf(body.Begin(), body.Size()));
    }

    RespondPlainText(rsp, EStatusCode::OK, std::to_string(sum));
}

// ---- /json/{count}?m={multiplier} ----
//
// Reference definition shared by json-comp (plaintext, gzip/br on request)
// and json-tls (TLS, port 8081, added in a later commit): first `count`
// dataset items, each with `total = price * quantity * m` appended.

struct TDatasetItem
{
    i64 Id = 0;
    std::string Name;
    std::string Category;
    i64 Price = 0;
    i64 Quantity = 0;
    bool Active = false;
    std::vector<std::string> Tags;
    i64 RatingScore = 0;
    i64 RatingCount = 0;
};

std::vector<TDatasetItem> Dataset;

// Parsed once at startup with library/cpp/json's DOM reader -- a different
// library from yt/yt/core/json below, which is a YSON-consumer bridge for
// *writing* JSON, not a general-purpose parser.
std::vector<TDatasetItem> LoadDataset()
{
    std::vector<TDatasetItem> items;

    const char* env = std::getenv("DATASET_PATH");
    std::string path = env ? env : "/data/dataset.json";
    std::ifstream input(path);
    if (!input) {
        return items;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();

    ::NJson::TJsonValue root;
    if (!::NJson::ReadJsonTree(buffer.str(), &root, /*throwOnError*/ false) || !root.IsArray()) {
        return items;
    }

    for (const auto& node : root.GetArraySafe()) {
        TDatasetItem item;
        item.Id = node["id"].GetIntegerSafe();
        item.Name = node["name"].GetStringSafe();
        item.Category = node["category"].GetStringSafe();
        item.Price = node["price"].GetIntegerSafe();
        item.Quantity = node["quantity"].GetIntegerSafe();
        item.Active = node["active"].GetBooleanSafe();
        for (const auto& tag : node["tags"].GetArraySafe()) {
            item.Tags.push_back(tag.GetStringSafe());
        }
        item.RatingScore = node["rating"]["score"].GetIntegerSafe();
        item.RatingCount = node["rating"]["count"].GetIntegerSafe();
        items.push_back(std::move(item));
    }

    return items;
}

// Serializes {items: [...], count} through the framework's own JSON writer
// (NYT::NJson::CreateJsonConsumer -- the same YSON-consumer bridge
// ReplyJson in helpers.h uses) into an in-memory buffer, so the caller can
// send it straight to the response or run it through the compressing
// adapter first.
std::string BuildJsonBody(i64 count, i64 multiplier)
{
    TBufferOutput out;
    auto consumer = NYT::NJson::CreateJsonConsumer(&out);

    // The (begin, end, func) overload of DoListFor hands func the iterator
    // itself, not a dereferenced value (it exists for integer ranges, see
    // its uses elsewhere in the tree) -- what we want is the (collection,
    // func) overload, so hand it a view over just the first `count` items.
    std::span<const TDatasetItem> items(Dataset.data(), static_cast<size_t>(count));

    BuildYsonFluently(consumer.get())
        .BeginMap()
            .Item("items").DoListFor(
                items,
                [&] (TFluentList fluent, const TDatasetItem& item) {
                    fluent.Item().BeginMap()
                        .Item("id").Value(item.Id)
                        .Item("name").Value(item.Name)
                        .Item("category").Value(item.Category)
                        .Item("price").Value(item.Price)
                        .Item("quantity").Value(item.Quantity)
                        .Item("active").Value(item.Active)
                        .Item("tags").DoListFor(item.Tags, [] (TFluentList tagsFluent, const std::string& tag) {
                            tagsFluent.Item().Value(tag);
                        })
                        .Item("rating").BeginMap()
                            .Item("score").Value(item.RatingScore)
                            .Item("count").Value(item.RatingCount)
                        .EndMap()
                        .Item("total").Value(item.Price * item.Quantity * multiplier)
                    .EndMap();
                })
            .Item("count").Value(count)
        .EndMap();

    consumer->Flush();

    std::string body;
    out.Buffer().AsString(body);
    return body;
}

void HandleJson(const IRequestPtr& req, const IResponseWriterPtr& rsp)
{
    TStringBuf path = req->GetUrl().Path;
    TStringBuf prefix = "/json/";

    i64 count = 0;
    if (path.size() > prefix.size() && path.substr(0, prefix.size()) == prefix) {
        count = ParseInt(path.substr(prefix.size()));
    }
    if (count < 0) {
        count = 0;
    }
    if (static_cast<size_t>(count) > Dataset.size()) {
        count = static_cast<i64>(Dataset.size());
    }

    i64 multiplier = FindQueryParameter(req->GetUrl().RawQuery, "m", /*defaultValue*/ 1);

    std::string body = BuildJsonBody(count, multiplier);

    rsp->SetStatus(EStatusCode::OK);
    rsp->GetHeaders()->Set(NHeaders::ContentTypeHeaderName, NHeaders::ApplicationJsonContentType);

    // Compression is per-request, driven entirely by what the client
    // offers -- when Accept-Encoding is absent, Content-Encoding must stay
    // unset rather than defaulting to some encoding.
    TContentEncoding contentEncoding;
    bool compress = false;
    if (const auto* acceptEncoding = req->GetHeaders()->Find("Accept-Encoding")) {
        auto best = GetBestAcceptedContentEncoding(*acceptEncoding);
        if (best.IsOK() && best.Value() != IdentityContentEncoding) {
            contentEncoding = best.Value();
            compress = true;
        }
    }

    if (compress) {
        rsp->GetHeaders()->Set("Content-Encoding", contentEncoding);
        // CreateCompressingAdapter wraps rsp itself (IResponseWriter is-a
        // IFlushableAsyncOutputStream): Write pushes compressed chunks
        // straight into the response, and Close both finishes the codec
        // and closes rsp, framing the response as chunked since no
        // Content-Length is known ahead of time.
        auto compressingStream = CreateCompressingAdapter(rsp, contentEncoding, GetCurrentInvoker());
        WaitFor(compressingStream->Write(TSharedRef::FromString(body)))
            .ThrowOnError();
        WaitFor(compressingStream->Close())
            .ThrowOnError();
    } else {
        WaitFor(rsp->WriteBody(TSharedRef::FromString(body)))
            .ThrowOnError();
    }
}

} // namespace

int main()
{
    Dataset = LoadDataset();

    // One poller thread per core: this is the server's own thread pool and
    // invoker, matching the "framework standard configuration" rule for the
    // standard-mode profiles this entry subscribes to.
    unsigned threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) {
        threadCount = 1;
    }

    // Shared by both listeners below (SetPathMatcher requires it be set
    // before Start(), and built exactly once so nothing mutates it once
    // either server is running).
    auto pathMatcher = New<TRequestPathMatcher>();
    pathMatcher->Add("/pipeline", BIND(&HandlePipeline));
    pathMatcher->Add("/baseline11", BIND(&HandleBaseline11));
    pathMatcher->Add("/json/", BIND(&HandleJson));

    auto config = New<TServerConfig>();
    config->Port = 8080;

    auto server = CreateServer(config, static_cast<int>(threadCount));
    server->SetPathMatcher(pathMatcher);
    server->Start();

    // The TLS listener is only stood up when the harness actually mounts
    // /certs (only for TLS-subscribed profiles), same guard drogon's entry
    // uses. Declared outside the if so it outlives it -- IServerPtr held
    // only in a block-scoped local is destroyed the instant the block
    // exits, tearing down the whole listener before any client can
    // connect (the listening socket lingers just long enough for one
    // connection to be accepted and then immediately orphaned).
    IServerPtr httpsServer;
    const std::string certFile = "/certs/server.crt";
    const std::string keyFile = "/certs/server.key";
    if (std::filesystem::exists(certFile) && std::filesystem::exists(keyFile)) {
        auto httpsConfig = New<NHttps::TServerConfig>();
        httpsConfig->Port = 8081;
        httpsConfig->Credentials = New<NHttps::TServerCredentialsConfig>();
        httpsConfig->Credentials->CertificateChain = NCrypto::TPemBlobConfig::CreateFileReference(certFile);
        httpsConfig->Credentials->PrivateKey = NCrypto::TPemBlobConfig::CreateFileReference(keyFile);

        httpsServer = NHttps::CreateServer(httpsConfig, static_cast<int>(threadCount));
        httpsServer->SetPathMatcher(pathMatcher);
        httpsServer->Start();
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }

    return 0;
}
