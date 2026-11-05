#!/bin/bash
# test_echo.sh —— Day 21：echo 服务器联调
#
# 用法：
#   ./tests/integration/test_echo.sh ./build/echo_server [port]
#
# 覆盖：
#   · 单连接回显
#   · 多个并发连接
#   · 大流量（1MB，能逼出 write 的 EAGAIN + 输出缓冲）
#   · 对端半关闭后，服务器要把缓冲里的数据发完再关

set -u

SERVER_BIN="${1:-./build/echo_server}"
PORT="${2:-18888}"
PASS=0
FAIL=0

cleanup() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -f /tmp/echo_in.bin /tmp/echo_out.bin /tmp/echo_multi_*.txt
}
trap cleanup EXIT

# ---------- 启动服务器 ----------
"$SERVER_BIN" "$PORT" > /tmp/echo_server_test.log 2>&1 &
SERVER_PID=$!

# 等端口就绪（用 bash 的 /dev/tcp，不依赖额外工具）
READY=0
for _ in $(seq 1 50); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        exec 3<&-
        exec 3>&-
        READY=1
        break
    fi
    sleep 0.1
done

if [ "$READY" -ne 1 ] || ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "❌ 服务器启动失败，日志："
    cat /tmp/echo_server_test.log
    exit 1
fi

echo "服务器已启动 (pid=$SERVER_PID, port=$PORT)"
echo

# ---------- 1. 基础回显 ----------
echo "[1] 基础回显"

OUT=$(printf 'hello\n' | timeout 5 nc -N 127.0.0.1 "$PORT" 2>/dev/null)
if [ "$OUT" = "hello" ]; then
    echo "  ✅ hello 原样返回"
    PASS=$((PASS + 1))
else
    echo "  ❌ 期望 'hello'，实际 '$OUT'"
    FAIL=$((FAIL + 1))
fi

# 空输入：服务器应该正常关闭，不崩
OUT=$(printf '' | timeout 5 nc -N 127.0.0.1 "$PORT" 2>/dev/null)
if [ -z "$OUT" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "  ✅ 空连接正常关闭，服务器仍存活"
    PASS=$((PASS + 1))
else
    echo "  ❌ 空连接处理异常"
    FAIL=$((FAIL + 1))
fi

# ---------- 2. 多个并发连接 ----------
echo
echo "[2] 多个并发连接"

PIDS=()
for i in 1 2 3; do
    (printf 'conn%s\n' "$i" | timeout 5 nc -N 127.0.0.1 "$PORT" \
        > "/tmp/echo_multi_$i.txt" 2>/dev/null) &
    PIDS+=($!)
done

for pid in "${PIDS[@]}"; do
    wait "$pid"
done

MULTI_OK=1
for i in 1 2 3; do
    if [ "$(cat "/tmp/echo_multi_$i.txt")" != "conn$i" ]; then
        MULTI_OK=0
    fi
done

if [ "$MULTI_OK" -eq 1 ]; then
    echo "  ✅ 3 个连接各自收到正确回显"
    PASS=$((PASS + 1))
else
    echo "  ❌ 并发连接回显错乱"
    FAIL=$((FAIL + 1))
fi

# ---------- 3. 大流量：1MB ----------
#
# 1MB 远超内核 socket 发送缓冲区（约 200KB），
# 所以一定会遇到 write 的 EAGAIN —— 这条用例真正验证的是输出缓冲。
echo
echo "[3] 大流量（1MB，验证 EAGAIN 与输出缓冲）"

head -c 1000000 /dev/zero | tr '\0' 'a' > /tmp/echo_in.bin

timeout 20 nc -N 127.0.0.1 "$PORT" < /tmp/echo_in.bin > /tmp/echo_out.bin 2>/dev/null

IN_SIZE=$(wc -c < /tmp/echo_in.bin)
OUT_SIZE=$(wc -c < /tmp/echo_out.bin)

if [ "$IN_SIZE" -eq "$OUT_SIZE" ] && cmp -s /tmp/echo_in.bin /tmp/echo_out.bin; then
    echo "  ✅ ${IN_SIZE} 字节完整回显（内容一致）"
    PASS=$((PASS + 1))
else
    echo "  ❌ 回显不完整：发送 $IN_SIZE 字节，收到 $OUT_SIZE 字节"
    FAIL=$((FAIL + 1))
fi

# ---------- 4. 服务器存活 ----------
echo
echo "[4] 服务存活检查"

if kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "  ✅ 以上全部请求后服务器仍存活"
    PASS=$((PASS + 1))

    OUT=$(printf 'still alive\n' | timeout 5 nc -N 127.0.0.1 "$PORT" 2>/dev/null)
    if [ "$OUT" = "still alive" ]; then
        echo "  ✅ 仍能正常回显"
        PASS=$((PASS + 1))
    else
        echo "  ❌ 服务器不再响应"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  ❌ 服务器已崩溃"
    FAIL=$((FAIL + 1))
fi

# ---------- 结果 ----------
echo
echo "=========================================="
echo "  通过: $PASS   失败: $FAIL"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo
    echo "服务器日志："
    tail -30 /tmp/echo_server_test.log
    exit 1
fi

exit 0
