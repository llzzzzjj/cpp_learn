import select
import socket
import sys
import time

board_ip, mode = sys.argv[1:3]
if mode not in ("read", "pause"):
    raise SystemExit("用法：python3 slow_client_a.py 板子IP read|pause")

with socket.create_connection((board_ip, 8888), timeout=3) as client:
    # 先确认订阅成功。不预读，避免提前读走后续的 DATA。
    with client.makefile("rb", buffering=0) as reader:
        client.sendall(b"SUB gyro\n")
        reply = reader.readline()
        if reply != b"OK SUB gyro\n":
            raise RuntimeError(f"订阅失败：{reply!r}")

    print(f"A 本地端口={client.getsockname()[1]}，模式={mode}，开始 30 秒测试",
          flush=True)

    if mode == "pause":
        time.sleep(30)  # 保持 TCP 连接打开，期间完全不调用 recv()。
    else:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            # 有数据再读，最多等到本轮结束。
            remaining = max(0, deadline - time.monotonic())
            readable, _, _ = select.select([client], [], [], remaining)
            if readable and not client.recv(4096):
                raise RuntimeError("服务端提前关闭了连接")

print("A 已断开，请查看板端 [tx] 汇总")