#!/usr/bin/env bash
# run_benchmarks.sh —— 按固定矩阵跑压测，原始输出落盘
#
# 用法：
#   bash benchmarks/run_benchmarks.sh                  # 全量（约 2 分钟）
#   QUICK=1 bash benchmarks/run_benchmarks.sh          # 跳过 60 秒长稳（约 45 秒）
#   ONLY=iothreads bash benchmarks/run_benchmarks.sh   # 只跑一类：delay|iothreads|conns|size|stability
#   PORT=9999 bash benchmarks/run_benchmarks.sh        # 换端口
#
# 产出（benchmarks/results/<YYYY-MM-DD>/）：
#   env.txt          环境快照：CPU / 核数 / 内核 / 编译器 / 构建类型 / ulimit / git commit
#   01-<label>.txt   每组的完整 stdout，文件头写明所用命令
#   summary.md       汇总表：QPS / P50 / P99 / P999 / 错误 / 服务端 CPU / μs 每条
#
# 本脚本只负责「跑 + 落盘」，不做结论。观察和结论自己写进 docs/performance.md。

set -u
cd "$(dirname "$0")/.." || exit 1

BUILD=${BUILD:-./build-rel}
SERVER=$BUILD/echo_server_mt
CLIENT=$BUILD/bench_client
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-8888}
DATE=$(date +%Y-%m-%d)
OUT=benchmarks/results/$DATE
META=${TMPDIR:-/tmp}/bench_meta.$$.tsv

[ -x "$SERVER" ] || {
    echo "缺少 $SERVER"
    echo "先构建：cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j\$(nproc)"
    exit 1
}
[ -x "$CLIENT" ] || { echo "缺少 $CLIENT"; exit 1; }

mkdir -p "$OUT"
: > "$META"

if ulimit -n 100000 2>/dev/null; then
    echo "ulimit -n -> $(ulimit -n)"
else
    echo "警告：ulimit -n 提高失败（当前 $(ulimit -n)），conns-1000 组可能失败"
fi

# ------------------------------------------------------------------ 环境快照
{
    echo "date        : $(date '+%F %T %Z')"
    echo "cpu         : $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')"
    echo "nproc       : $(nproc)"
    echo "kernel      : $(uname -r)"
    echo "os          : $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
    echo "compiler    : $(g++ --version 2>/dev/null | head -1)"
    echo "build_type  : $(grep -m1 '^CMAKE_BUILD_TYPE' "$BUILD/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
    echo "ulimit -n   : $(ulimit -n)"
    echo "git commit  : $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "governor    : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
} > "$OUT/env.txt"

# ------------------------------------------------------------------ 服务端
SERVER_PID=""

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
    fi
    pkill -x echo_server_mt 2>/dev/null
    sleep 0.3
    SERVER_PID=""
}

start_server() {
    stop_server
    "$SERVER" "$PORT" "$1" > /tmp/bench_srv.log 2>&1 &
    sleep 0.5
    SERVER_PID=$(pgrep -x echo_server_mt | head -1)
    [ -n "$SERVER_PID" ]
}

trap 'stop_server; rm -f "$META"' EXIT

# /proc/PID/stat 的 utime+stime，单位 tick（USER_HZ，通常 100/s）
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

# ------------------------------------------------------------------ 单组
IDX=0

run_case() {
    local label=$1 sthreads=$2 cthreads=$3 conns=$4 msgs=$5 payload=$6
    shift 6
    local extra="$*"

    IDX=$((IDX + 1))
    local file
    file=$(printf "%s/%02d-%s.txt" "$OUT" "$IDX" "$label")

    printf '[%02d] %-18s io=%-2s client=%-2s conns=%-5s msgs=%-6s size=%-6s %s\n' \
        "$IDX" "$label" "$sthreads" "$cthreads" "$conns" "$msgs" "$payload" "$extra"

    if ! start_server "$sthreads"; then
        {
            echo "# label  : $label"
            echo "# server : $SERVER $PORT $sthreads"
            echo "服务端启动失败，日志："
            sed 's/^/    /' /tmp/bench_srv.log
        } > "$file"
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$IDX" "$label" "$sthreads" "$cthreads" "$conns" "$msgs" "$payload" "FAIL" "0" >> "$META"
        return
    fi

    local t0 t1 rc
    t0=$(cpu_ticks "$SERVER_PID")

    {
        echo "# label  : $label"
        echo "# server : $SERVER $PORT $sthreads"
        echo "# client : $CLIENT $HOST $PORT $cthreads $msgs $payload --connections $conns $extra"
        echo
        # shellcheck disable=SC2086
        "$CLIENT" "$HOST" "$PORT" "$cthreads" "$msgs" "$payload" --connections "$conns" $extra 2>&1
    } > "$file"
    rc=$?

    t1=$(cpu_ticks "$SERVER_PID")

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$IDX" "$label" "$sthreads" "$cthreads" "$conns" "$msgs" "$payload" "$rc" "$((t1 - t0))" >> "$META"

    stop_server
}

# ------------------------------------------------------------------ 汇总
summarize() {
    local sum=$OUT/summary.md
    {
        echo "# 压测汇总 · $(date '+%F %T')"
        echo
        echo '环境见 `env.txt`。服务端 CPU 取自 `/proc/PID/stat`，**含 1 秒 warmup**，只用于同批横向比较。'
        echo
        echo '| # | label | io线程 | 客户端线程 | 连接 | 消息数 | 大小 | QPS | P50 | P99 | P999 | timeout | io_err | 服务端CPU | 退出码 | μs/条 |'
        echo '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
    } > "$sum"

    local idx label st ct cn ms sz rc ticks file line
    local qps p50 p99 p999 tout ioerr total us cpu_s

    while IFS=$'\t' read -r idx label st ct cn ms sz rc ticks; do
        file=$(printf "%s/%02d-%s.txt" "$OUT" "$idx" "$label")

        qps="-"; p50="-"; p99="-"; p999="-"; tout="-"; ioerr="-"; total=0
        if [ -f "$file" ]; then
            line=$(awk '
                /^QPS[ \t]*:/  { q = $3 }
                /^P50[ \t(]/   { a = $4 }
                /^P99[ \t(]/   { b = $4 }
                /^P999[ \t(]/  { c = $4 }
                /^completed/   {
                    n = $3
                    for (i = 1; i <= NF; i++) {
                        if ($i ~ /^timeout=/)  { t = $i; gsub(/[^0-9]/, "", t) }
                        if ($i ~ /^io_error=/) { e = $i; gsub(/[^0-9]/, "", e) }
                    }
                }
                END {
                    printf "%s %s %s %s %s %s %s\n",
                        (q==""?"-":q), (a==""?"-":a), (b==""?"-":b), (c==""?"-":c),
                        (t==""?"-":t), (e==""?"-":e), (n==""?0:n)
                }' "$file")
            read -r qps p50 p99 p999 tout ioerr total <<< "$line"
        fi

        us="-"
        if [ "${ticks:-0}" -gt 0 ] && [ "${total:-0}" -gt 0 ]; then
            us=$(awk -v t="$ticks" -v n="$total" 'BEGIN{printf "%.1f", t * 10000 / n}')
        fi

        cpu_s=$(awk -v t="${ticks:-0}" 'BEGIN{printf "%.2f s", t / 100}')

        echo "| $idx | $label | $st | $ct | $cn | $ms | $sz | $qps | $p50 | $p99 | $p999 | $tout | $ioerr | $cpu_s | $rc | $us |" >> "$sum"
    done < "$META"

    echo
    echo "=========================================="
    echo "原始输出：$OUT/"
    echo "汇总表  ：$sum"
    echo "=========================================="
}

# ------------------------------------------------------------------ 矩阵
GROUP=${ONLY:-all}
want() { [ "$GROUP" = "all" ] || [ "$GROUP" = "$1" ]; }

# 一、延迟基线：单连接串行，唯一「干净的」单条延迟数字
if want delay; then
    run_case "delay-single-64B" 0 1 1 20000   64
    run_case "delay-single-1KB" 0 1 1 20000 1024
fi

# 二、IO 线程扩展：只改服务端线程数
if want iothreads; then
    run_case "iothreads-0" 0 4 100 5000 1024
    run_case "iothreads-1" 1 4 100 5000 1024
    run_case "iothreads-2" 2 4 100 5000 1024
    run_case "iothreads-4" 4 4 100 5000 1024
    run_case "iothreads-8" 8 4 100 5000 1024
fi

# 三、连接数扩展：只改连接数（客户端线程跟着放大，避免客户端先饱和）
if want conns; then
    run_case "conns-10"   4 2   10 20000 1024
    run_case "conns-100"  4 4  100  5000 1024
    run_case "conns-500"  4 4  500  1500 1024
    run_case "conns-1000" 4 8 1000   600 1024
fi

# 四、消息大小：只改 payload
if want size; then
    run_case "size-64B"  4 4 100 5000    64
    run_case "size-1KB"  4 4 100 5000  1024
    run_case "size-16KB" 4 4 100 2000 16384
    run_case "size-64KB" 4 4 100  500 65536
fi

# 五、长稳：固定时长跑
if want stability && [ "${QUICK:-0}" != "1" ]; then
    run_case "stability-60s" 4 4 100 0 1024 --duration 60
fi

summarize
