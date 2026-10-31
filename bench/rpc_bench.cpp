// rpc_bench.cpp: RPC 往返性能压测（同进程：RpcServer + 若干 RpcClient）
//
// 三个场景（全部走同一套帧协议、序列化与线程池调度）：
//   1) 串行     ：单连接、1 个在途请求，测单次往返延迟分布；
//   2) 流水线   ：单连接、滑动窗口多个在途请求，测单连接吞吐上限；
//   3) 多连接   ：K 个客户端同时压，观察服务端自有线程池的扩展性。
//
// 用法：rpc_bench [--policy inline|pool] [--serial N] [--conc N] [--window W] [--clients K]
//                 [--threads T] [--timeout MS]
//   --policy   服务端执行策略：inline（默认）/ pool
//   --serial   串行阶段请求数（默认 2000）
//   --conc     并发的总请求数（默认 20000，按客户端均分）
//   --window   每个连接允许的在途请求数（默认 64）
//   --clients  并发阶段的客户端数量（默认 4）
//   --threads  服务端自有线程池线程数（默认 4）
//   --timeout  请求级超时，0 表示关闭（默认 0，避免定时器开销污染测量）

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <qrpc/rpc_client.h>
#include <qrpc/rpc_server.h>

// 被测方法：业务本身不做任何事，量到的就是框架往返开销
int ping(int value)
{
    return value;
}

namespace {

constexpr auto kServerName = "rpc_bench_server";

struct Config
{
    int serial = 2000;
    int conc = 20000;
    int window = 64;
    int clients = 4;
    int threads = 4;
    int timeoutMs = 0;
    QString policy = QStringLiteral("inline");
};

RpcServer::ExecPolicy toPolicy(const QString& name)
{
    if (name.compare(QLatin1String("pool"), Qt::CaseInsensitive) == 0)
        return RpcServer::ExecPolicy::ThreadPool;
    return RpcServer::ExecPolicy::Inline;
}

struct Sample
{
    std::vector<qint64> latencies;   // ns
    qint64 wallNs = 0;
    int errors = 0;
};

void report(QTextStream& out, const QString& title, const Sample& s)
{
    out << title << Qt::endl;

    if (s.latencies.empty()) {
        out << QStringLiteral("  no sample (errors=%1)").arg(s.errors) << Qt::endl << Qt::endl;
        return;
    }

    std::vector<qint64> v = s.latencies;
    std::sort(v.begin(), v.end());

    qint64 sum = 0;
    for (qint64 ns : v)
        sum += ns;

    const auto toUs = [](qint64 ns) { return double(ns) / 1000.0; };
    const auto pct = [&v](double p) {
        return v[static_cast<std::size_t>(p * double(v.size() - 1))];
    };

    const double wallMs = double(s.wallNs) / 1e6;
    const double qps = s.wallNs > 0 ? double(v.size()) * 1e9 / double(s.wallNs) : 0.0;

    out << QStringLiteral("  requests=%1  errors=%2  wall=%3 ms  throughput=%4 req/s")
               .arg(v.size()).arg(s.errors).arg(wallMs, 0, 'f', 1).arg(qps, 0, 'f', 0)
        << Qt::endl;
    out << QStringLiteral("  latency(us): avg=%1  p50=%2  p95=%3  p99=%4  max=%5")
               .arg(toUs(sum / qint64(v.size())), 0, 'f', 1)
               .arg(toUs(pct(0.50)), 0, 'f', 1)
               .arg(toUs(pct(0.95)), 0, 'f', 1)
               .arg(toUs(pct(0.99)), 0, 'f', 1)
               .arg(toUs(v.back()), 0, 'f', 1)
        << Qt::endl << Qt::endl;
}

// 一个连接上的负载：窗口内补位发送，直到 total 个请求全部完成
class LoadRunner
{
public:
    LoadRunner(RpcClient& client, int total, int window, Sample& sample,
               std::function<void()> onDone)
        : m_client(client)
        , m_total(total)
        , m_window(std::max(1, window))
        , m_sample(sample)
        , m_onDone(std::move(onDone))
    {
    }

    void start()
    {
        if (m_total <= 0) {
            if (m_onDone)
                m_onDone();
            return;
        }
        pump();
    }

private:
    void pump()
    {
        while (m_sent < m_total && (m_sent - m_done) < m_window)
            issue(++m_sent);
    }

    void issue(int seq)
    {
        QElapsedTimer timer;
        timer.start();

        m_client.callAsync<ping>(seq, [this, seq, timer](const RpcResult<int>& r) {
            if (!r.ok) {
                ++m_sample.errors;
            } else {
                m_sample.latencies.push_back(timer.nsecsElapsed());
                if (r.value != seq)   // 顺带校验回包是否错位
                    ++m_sample.errors;
            }

            ++m_done;
            if (m_done < m_total) {
                pump();
                return;
            }

            auto done = std::move(m_onDone);
            if (done)
                done();
        });
    }

    RpcClient& m_client;
    int m_total;
    int m_window;
    int m_sent = 0;
    int m_done = 0;
    Sample& m_sample;
    std::function<void()> m_onDone;
};

class Bench
{
public:
    explicit Bench(const Config& cfg)
        : m_cfg(cfg)
    {
    }

    bool start()
    {
        m_server.setExecPolicy(toPolicy(m_cfg.policy));
        m_server.setThreadCount(m_cfg.threads);
        m_server.registerHandler<ping>();

        if (!m_server.listen(kServerName))
            return false;

        m_client = std::make_unique<RpcClient>();
        m_client->setTimeout(m_cfg.timeoutMs);

        QObject::connect(m_client.get(), &RpcClient::connected, m_client.get(), [this] {
            m_phase = 0;
            runSerialPhase();
        });
        QObject::connect(m_client.get(), &RpcClient::socketError, m_client.get(),
                         [this](const QString& msg) {
                             fail(QStringLiteral("client socket error: ") + msg);
                         });

        m_client->connectToServer(kServerName);
        return true;
    }

    void reportAll(QTextStream& out)
    {
        out << Qt::endl
            << QStringLiteral("=== results (server policy=%1, threads=%2) ===")
                   .arg(m_cfg.policy, QString::number(m_cfg.threads))
            << Qt::endl << Qt::endl;

        report(out, QStringLiteral("1) serial: 1 in-flight, %1 requests").arg(m_cfg.serial),
               m_serial);
        report(out, QStringLiteral("2) pipelined: 1 connection, window=%1, %2 requests")
                       .arg(m_cfg.window).arg(m_cfg.conc),
               m_pipe);
        report(out, QStringLiteral("3) multi-client: %1 clients, window=%2, %3 requests total")
                       .arg(m_cfg.clients).arg(m_cfg.window).arg(m_concPerClient * m_cfg.clients),
               m_multi);
    }

private:
    void runSerialPhase()
    {
        m_phaseTimer.start();
        m_pendingRunners = 1;
        m_runners.push_back(std::make_unique<LoadRunner>(
            *m_client, m_cfg.serial, 1, m_serial, [this] { onRunnerFinished(); }));
        m_runners.back()->start();
    }

    void runPipelinedPhase()
    {
        m_phaseTimer.start();
        m_pendingRunners = 1;
        m_runners.push_back(std::make_unique<LoadRunner>(
            *m_client, m_cfg.conc, m_cfg.window, m_pipe, [this] { onRunnerFinished(); }));
        m_runners.back()->start();
    }

    void runMultiClientPhase()
    {
        const int clients = std::max(1, m_cfg.clients);
        m_concPerClient = std::max(1, m_cfg.conc / clients);

        m_phaseTimer.start();
        m_pendingRunners = clients;
        m_clients.clear();
        m_clients.reserve(clients);

        for (int i = 0; i < clients; ++i) {
            auto client = std::make_unique<RpcClient>();
            client->setTimeout(m_cfg.timeoutMs);
            RpcClient* raw = client.get();
            m_clients.push_back(std::move(client));

            QObject::connect(raw, &RpcClient::connected, raw, [this, raw] {
                m_runners.push_back(std::make_unique<LoadRunner>(
                    *raw, m_concPerClient, m_cfg.window, m_multi,
                    [this] { onRunnerFinished(); }));
                m_runners.back()->start();
            });
            QObject::connect(raw, &RpcClient::socketError, raw,
                             [this](const QString& msg) {
                                 fail(QStringLiteral("client socket error: ") + msg);
                             });

            raw->connectToServer(kServerName);
        }
    }

    void onRunnerFinished()
    {
        if (m_failed || --m_pendingRunners > 0)
            return;

        const qint64 wall = m_phaseTimer.nsecsElapsed();
        if (m_phase == 0)
            m_serial.wallNs = wall;
        else if (m_phase == 1)
            m_pipe.wallNs = wall;
        else
            m_multi.wallNs = wall;

        ++m_phase;
        // 延后一拍：不要在 runner 自己的回调里销毁 runner
        QTimer::singleShot(0, qApp, [this] { nextPhase(); });
    }

    void nextPhase()
    {
        m_runners.clear();

        switch (m_phase) {
        case 1:
            runPipelinedPhase();
            break;
        case 2:
            runMultiClientPhase();
            break;
        default: {
            QTextStream out(stdout);
            reportAll(out);
            QCoreApplication::quit();
            break;
        }
        }
    }

    void fail(const QString& message)
    {
        if (m_failed)
            return;
        m_failed = true;
        QTextStream(stdout) << message << Qt::endl;
        QCoreApplication::exit(1);
    }

    Config m_cfg;
    RpcServer m_server;
    std::unique_ptr<RpcClient> m_client;
    std::vector<std::unique_ptr<RpcClient>> m_clients;
    std::vector<std::unique_ptr<LoadRunner>> m_runners;
    Sample m_serial;
    Sample m_pipe;
    Sample m_multi;
    QElapsedTimer m_phaseTimer;
    int m_phase = 0;
    int m_pendingRunners = 0;
    int m_concPerClient = 0;
    bool m_failed = false;
};

Config parseArgs(int argc, char* argv[])
{
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (i + 1 >= argc)
            break;

        const QString value = QString::fromLocal8Bit(argv[i + 1]);
        if (arg == QLatin1String("--policy"))
            cfg.policy = value;
        else if (arg == QLatin1String("--serial"))
            cfg.serial = value.toInt();
        else if (arg == QLatin1String("--conc"))
            cfg.conc = value.toInt();
        else if (arg == QLatin1String("--window"))
            cfg.window = value.toInt();
        else if (arg == QLatin1String("--clients"))
            cfg.clients = value.toInt();
        else if (arg == QLatin1String("--threads"))
            cfg.threads = value.toInt();
        else if (arg == QLatin1String("--timeout"))
            cfg.timeoutMs = value.toInt();
        else
            continue;

        ++i;
    }

    return cfg;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    const Config cfg = parseArgs(argc, argv);

    QTextStream out(stdout);
    out << QStringLiteral("rpc_bench: policy=%1 serial=%2 conc=%3 window=%4 clients=%5 threads=%6 timeout=%7ms")
               .arg(cfg.policy).arg(cfg.serial).arg(cfg.conc).arg(cfg.window)
               .arg(cfg.clients).arg(cfg.threads).arg(cfg.timeoutMs)
        << Qt::endl;

    Bench bench(cfg);
    if (!bench.start()) {
        out << "server listen failed" << Qt::endl;
        return 1;
    }

    // 兜底：异常情况下也别挂死
    QTimer::singleShot(180000, &app, &QCoreApplication::quit);

    return app.exec();
}
