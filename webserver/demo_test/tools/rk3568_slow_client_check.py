import argparse
import selectors
import socket
import time


def subscribe(connection):
    connection.sendall(b"SUB gyro\n")
    reply = bytearray()
    # Read exactly one line before starting the stop-reading trial.
    while not reply.endswith(b"\n"):
        part = connection.recv(1)
        if not part or len(reply) >= 256:
            raise RuntimeError(f"invalid subscription response: {reply!r}")
        reply.extend(part)
    if reply != b"OK SUB gyro\n":
        raise RuntimeError(f"invalid subscription response: {reply!r}")


def run(mode, endpoint, seconds):
    with socket.create_connection(endpoint, timeout=3) as client_b, \
            socket.create_connection(endpoint, timeout=3) as client_a, \
            selectors.DefaultSelector() as ready:
        subscribe(client_b)
        subscribe(client_a)
        ready.register(client_b, selectors.EVENT_READ, "B")
        if mode == "read":
            ready.register(client_a, selectors.EVENT_READ, "A")

        print(f"{mode}: A port={client_a.getsockname()[1]}, B port={client_b.getsockname()[1]}", flush=True)
        start = time.monotonic()
        end = start + seconds
        next_query = start + 5
        outstanding = None
        latency_ms = []
        queries = 0
        timed_out = False
        gyro_count = 0
        receive_buffer = b""

        while True:
            now = time.monotonic()
            if outstanding is not None and now - outstanding >= 2:
                # Do not attribute a late response to a new query.
                timed_out = True
                outstanding = None
            if now >= end:
                break
            if not timed_out and outstanding is None and next_query < end and now >= next_query:
                client_b.sendall(b"STATUS\n")
                outstanding = time.monotonic()
                queries += 1
                next_query += 5

            # A is deliberately absent from the selector in the pause trial.
            for key, _ in ready.select(min(0.1, max(0, end - time.monotonic()))):
                block = key.fileobj.recv(8192)
                if not block:
                    raise RuntimeError(f"client {key.data} disconnected early in {mode}")
                if key.data == "A":
                    continue
                lines = (receive_buffer + block).split(b"\n")
                receive_buffer = lines.pop()
                for line in lines:
                    if line.startswith(b"DATA gyro "):
                        gyro_count += 1
                    elif line.startswith(b"STATUS ") and outstanding is not None:
                        latency_ms.append(1000 * (time.monotonic() - outstanding))
                        outstanding = None

        latencies = ", ".join(f"{latency:.1f}" for latency in latency_ms) or "n/a"
        print(f"{mode}: B DATA={gyro_count}, STATUS={len(latency_ms)}/{queries}, "
              f"timeout={int(timed_out)}, latency_ms=[{latencies}]", flush=True)
        return int(timed_out or not gyro_count or not latency_ms)


def main():
    parser = argparse.ArgumentParser(description="Compare two normal-rate RK3568 TCP client trials")
    parser.add_argument("board_ip")
    parser.add_argument("--port", type=int, default=8888)
    parser.add_argument("--seconds", type=int, default=30)
    options = parser.parse_args()
    if options.seconds < 8:
        parser.error("use at least 8 seconds to include a STATUS query")

    endpoint = (options.board_ip, options.port)
    read_result = run("read", endpoint, options.seconds)
    time.sleep(1)
    pause_result = run("pause", endpoint, options.seconds)
    return max(read_result, pause_result)


if __name__ == "__main__":
    raise SystemExit(main())
