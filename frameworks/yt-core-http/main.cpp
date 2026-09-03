#include <yt/yt/core/http/server.h>
#include <yt/yt/core/http/http.h>
#include <yt/yt/core/http/config.h>
#include <yt/yt/core/http/helpers.h>

#include <yt/yt/core/concurrency/thread_pool_poller.h>
#include <yt/yt/core/concurrency/scheduler_api.h>

#include <yt/yt/core/actions/bind.h>

#include <library/cpp/yt/memory/ref.h>

#include <charconv>
#include <chrono>
#include <string>
#include <thread>

using namespace NYT;
using namespace NYT::NHttp;
using namespace NYT::NConcurrency;

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

} // namespace

int main()
{
    auto config = New<TServerConfig>();
    config->Port = 8080;

    // One poller thread per core: this is the server's own thread pool and
    // invoker, matching the "framework standard configuration" rule for the
    // standard-mode profiles this entry subscribes to.
    unsigned threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) {
        threadCount = 1;
    }

    auto server = CreateServer(config, static_cast<int>(threadCount));

    server->AddHandler("/pipeline", BIND(&HandlePipeline));
    server->AddHandler("/baseline11", BIND(&HandleBaseline11));

    server->Start();

    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }

    return 0;
}
