import socket

requests = b"hello\nfail\nworld\n"      # 三条以换行结尾的请求
expected = b"HELLO\nERR\nWORLD\n"        # 按请求顺序期待的响应

# 连接当前运行的服务端，为 Socket 操作设置超时。
with socket.create_connection(("127.0.0.1", 8888), timeout=3) as client:
    client.sendall(requests)
    client.shutdown(socket.SHUT_WR)    # 结束发送，继续接收响应

    received = bytearray()
    while True:
        chunk = client.recv(4096)
        if not chunk:                  # 读到 EOF，接收结束
            break
        received.extend(chunk)          # 累计字节，不依赖单次读取的长度

    # 同时检查响应的内容、顺序和长度。
    if received != expected:
        raise RuntimeError(f"预期 {expected!r}，实际 {bytes(received)!r}")

print("通过：三条响应顺序正确，异常后继续处理，并读到 EOF")