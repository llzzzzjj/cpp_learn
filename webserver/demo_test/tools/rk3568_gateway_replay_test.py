"""在 Ubuntu 临时副本里，用固定速率回放测试网关的慢客户端行为。"""

import argparse
import contextlib
import json
import os
import re
import selectors
import shutil
import signal
import socket
import statistics
import subprocess
import tempfile
import threading
import time
from pathlib import Path


SERVER_DIR = Path("webserver/demo_test")
SENSOR_DIR = Path("rk3568_dual_acquisition")
SERVER_BUFFER = 4096
SLOW_RECEIVE_BUFFER = 4096
REPLAY_PERIOD_MS = 2

REPLAY_SOURCE = r'''
#include "sensor_acquisition.hpp"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <poll.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

void capture_dual_sensors(int stop_fd, const GyroSampleHandler&, const LightSampleHandler& light) {
    const char* gate = std::getenv("RK3568_TEST_GATE");
    if (!gate) {throw std::runtime_error("missing replay gate");}
    pollfd watched{stop_fd, POLLIN, 0};
    const auto stopped = [&watched](int timeout) {
        int ready;
        do {
            ready = ::poll(&watched, 1, timeout);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) {throw std::runtime_error("replay poll failed");}
        return (watched.revents & POLLIN) != 0;
    };

    // 两个客户端都订阅成功后，才从相同起点开始回放。
    while (::access(gate, F_OK) != 0) {
        if (stopped(2)) {return;}
    }
    auto due = std::chrono::steady_clock::now();
    const auto period = std::chrono::milliseconds(REPLAY_MS);
    for (std::int64_t sequence = 0;; ++sequence) {
        if (stopped(0)) {return;}
        light(LightSample{sequence * REPLAY_MS * 1000, static_cast<int>(sequence % 8)});
        due += period;
        if (std::chrono::steady_clock::now() > due + period) {
            // 调度延误后不突发补发，保持回放速率可控。
            due = std::chrono::steady_clock::now() + period;
        }
        std::this_thread::sleep_until(due);
    }
}
'''


def build_fixture(repo, staging, port):
    shutil.copytree(repo / SERVER_DIR, staging / SERVER_DIR, ignore=shutil.ignore_patterns("build*"))
    for shared in ("threadpool", "common"):
        (staging / shared).symlink_to(repo / shared, target_is_directory=True)
    (staging / SENSOR_DIR).mkdir()
    for name in ("sensor_acquisition.hpp", "bounded_queue.hpp", "fd.hpp"):
        shutil.copyfile(repo / SENSOR_DIR / name, staging / SENSOR_DIR / name)
    replay = REPLAY_SOURCE.replace("REPLAY_MS", str(REPLAY_PERIOD_MS))
    (staging / SENSOR_DIR / "sensor_acquisition.cpp").write_text(replay, encoding="utf-8")

    # 只改临时副本：绑定本机回环，缩小发送缓冲区以便稳定复现积压。
    server_path = staging / SERVER_DIR / "epoll_server.cpp"
    source = server_path.read_text(encoding="utf-8")
    listen = '    handle_error("listen", ::listen(listener.get(), 1024));'
    for marker in ("constexpr int kPort = 8888;", "htonl(INADDR_ANY)", listen):
        if source.count(marker) != 1:
            raise RuntimeError(f"找不到预期的源码位置：{marker}")
    source = source.replace("constexpr int kPort = 8888;", f"constexpr int kPort = {port};")
    source = source.replace("htonl(INADDR_ANY)", "htonl(INADDR_LOOPBACK)")
    source = source.replace(listen, f'''    const int buffer = {SERVER_BUFFER};
    handle_error("test SO_SNDBUF", ::setsockopt(listener.get(), SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer)));
{listen}''')
    server_path.write_text(source, encoding="utf-8")
    build = staging / "build"
    subprocess.run(["cmake", "-S", staging / SERVER_DIR, "-B", build], check=True, timeout=40)
    subprocess.run(["cmake", "--build", build, "--target", "epoll_server", "-j", "2"], check=True, timeout=90)
    return build / "epoll_server"


def connect(port, receive_buffer=None):
    peer = socket.socket()
    try:
        peer.settimeout(2)
        if receive_buffer is not None:
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        peer.connect(("127.0.0.1", port))
        return peer
    except BaseException:
        peer.close()
        raise


def subscribe(peer):
    peer.sendall(b"SUB light\n")
    with peer.makefile("rb", buffering=0) as replies:
        response = replies.readline(128)
    if response != b"OK SUB light\n":
        raise RuntimeError(f"订阅失败：{response!r}")


def sample_sendq(port, a_port, b_port, stop, observations):
    while not stop.is_set():
        sample = subprocess.run(["ss", "-Htn", f"sport = :{port}"], capture_output=True,
                                text=True, check=True, timeout=2)
        queues = {fields[4].rsplit(":", 1)[-1]: int(fields[2])
                  for line in sample.stdout.splitlines()
                  if (fields := line.split()) and fields[0] == "ESTAB" and len(fields) >= 5}
        if str(a_port) in queues and str(b_port) in queues:
            observations.append((queues[str(a_port)], queues[str(b_port)]))
        stop.wait(0.35)


def measure(clients, mode, seconds, gate):
    a_socket, b_socket = clients
    with selectors.DefaultSelector() as watcher:
        watcher.register(b_socket, selectors.EVENT_READ, "b")
        if mode == "read":
            watcher.register(a_socket, selectors.EVENT_READ, "a")
        # 停读轮的 A 不进入监听列表，不会再执行 recv()。
        now = time.perf_counter()
        end = now + seconds
        next_query = now + 0.5
        pending = None
        sent = 0
        readings = 0
        latencies = []
        timed_out = 0
        b_fragment = b""
        gate.touch()
        while time.perf_counter() < end:
            now = time.perf_counter()
            if pending is not None and now - pending >= 2:
                timed_out += 1
                pending = None
            if not timed_out and pending is None and next_query <= now < end - 0.25:
                pending = time.perf_counter()
                b_socket.sendall(b"STATUS\n")
                sent += 1
                next_query += 1
            for key, _ in watcher.select(0.02):
                block = key.fileobj.recv(8192)
                if not block:
                    raise RuntimeError(f"客户端 {key.data} 提前断开")
                if key.data == "a":
                    continue
                lines = (b_fragment + block).split(b"\n")
                b_fragment = lines.pop()
                for line in lines:
                    if line.startswith(b"DATA light "):
                        readings += 1
                    elif line.startswith(b"STATUS ") and pending is not None:
                        latencies.append((time.perf_counter() - pending) * 1000)
                        pending = None
        return {"data_b": readings, "status_sent": sent, "status_ok": len(latencies),
                "status_timeout": timed_out, "status_pending": int(pending is not None),
                "status_median_ms": round(statistics.median(latencies), 3) if latencies else None,
                "status_max_ms": round(max(latencies), 3) if latencies else None}


def trial(binary, port, mode, seconds, staging):
    gate = staging / (mode + ".start")
    log = staging / (mode + ".log")
    observations = []
    with log.open("w", encoding="utf-8") as output:
        server = subprocess.Popen(["stdbuf", "-oL", binary, "--sensors"], stdout=output,
                                  stderr=subprocess.STDOUT, env=dict(os.environ, RK3568_TEST_GATE=str(gate)))
        try:
            with contextlib.ExitStack() as clients:
                for _ in range(40):
                    if server.poll() is not None:
                        raise RuntimeError(log.read_text())
                    try:
                        b_socket = clients.enter_context(connect(port))
                        break
                    except ConnectionRefusedError:
                        time.sleep(0.05)
                else:
                    raise RuntimeError("临时服务端没有监听")
                a_socket = clients.enter_context(connect(port, SLOW_RECEIVE_BUFFER))
                subscribe(b_socket)
                subscribe(a_socket)
                stop = threading.Event()
                sampler = threading.Thread(target=sample_sendq,
                                           args=(port, a_socket.getsockname()[1], b_socket.getsockname()[1],
                                                 stop, observations))
                sampler.start()
                try:
                    print(f"试验开始：{mode}，{seconds} 秒，B 先接入、A 后接入", flush=True)
                    measured = measure((a_socket, b_socket), mode, seconds, gate)
                finally:
                    stop.set()
                    sampler.join()
            for _ in range(30):
                if log.read_text().count("[tx] id=") >= 2:
                    break
                time.sleep(0.05)
        finally:
            if server.poll() is None:
                server.send_signal(signal.SIGINT)
            try:
                status = server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                raise
        if status != 0:
            raise RuntimeError(f"服务端退出码 {status}")

    matches = re.findall(r"^\[tx\] id=(\d+) skip_worker=(\d+) skip_pending=(\d+) "
                         r"short_writes=(\d+) would_block=(\d+)$", log.read_text(), re.MULTILINE)
    records = {int(row[0]): dict(zip(("skip_worker", "skip_pending", "short_writes", "would_block"),
                                     map(int, row[1:]))) for row in matches}
    if not observations or not {1, 2}.issubset(records):
        raise RuntimeError(f"没有采全 Send-Q 或 [tx] 记录：{observations}, {records}")
    queue_a, queue_b = zip(*observations)
    measured.update(mode=mode, send_q_a=list(queue_a), send_q_b=list(queue_b), tx_a=records[2], tx_b=records[1])
    print(json.dumps(measured, ensure_ascii=False), flush=True)
    return measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.home() / "cpp_learn")
    parser.add_argument("--seconds", type=int, default=9)
    options = parser.parse_args()
    if not 6 <= options.seconds <= 60:
        parser.error("试验时长须在 6 到 60 秒之间")
    if not shutil.which("ss"):
        parser.error("需要 Linux 上的 ss 命令采集 Send-Q")
    with socket.socket() as temporary_socket:
        temporary_socket.bind(("127.0.0.1", 0))
        port = temporary_socket.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="rk3568-replay-") as directory:
        staging = Path(directory)
        binary = build_fixture(options.repo.resolve(), staging, port)
        print(f"配置：回放周期 {REPLAY_PERIOD_MS} ms，服务端 SO_SNDBUF 和 A 的 SO_RCVBUF 均请求 4096 字节", flush=True)
        trial(binary, port, "read", options.seconds, staging)
        trial(binary, port, "pause", options.seconds, staging)


if __name__ == "__main__":
    main()
