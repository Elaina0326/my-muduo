#!/usr/bin/env bash
# bench_repeat.sh —— 同一条件重复测量，输出中位数与波动范围
#
# 为什么需要它：单次压测在同一配置上能有 7% 的组间波动（2026-10-28 实测），
# 小于这个量级的差距不能当结论。重复 N 次取中位数，能看出"这个数稳不稳"。
#
# 用法：
#   bash benchmarks/bench_repeat.sh                      # 3 次，服务端 0 IO，客户端 1 与 4 线程
#   bash benchmarks/bench_repeat.sh 5                    # 5 次
#   bash benchmarks/bench_repeat.sh 5 4 "4 8"            # 5 次，服务端 4 IO，客户端 4 与 8 线程
#   CONNS=500 MSGS=1500 bash benchmarks/bench_repeat.sh 3 4 "4"
#   RESTART=group bash benchmarks/bench_repeat.sh 5 8 "8"   # 每组前重启服务端
#   RESTART=run   bash benchmarks/bench_repeat.sh 3 8 "8"   # 每次前重启服务端
#
# 重启策略（RESTART）：
#   off / 0   —— 服务端只启动一次，组间不重启（默认；测"同进程内重复性"）
#   group / 1 —— 每组开始前重启服务端（去掉上一组的热分配器/热缓存）
#   run       —— 每次测量前都重启（最干净，也最慢；跨进程方差来自这里）
#
# 输出：
#   benchmarks/results/<date>/repeat-io<服务端IO>-conns<连接>-size<大小>.md
#   终端同步打印每组每次的原始数字

set -u
cd "$(dirname "$0")/.." || exit 1

REPEAT=${1:-3}
SRV_IO=${2:-0}
CLIENT_LIST=${3:-"1 4"}
RESTART=${RESTART:-0}

case "$RESTART" in
    0|off)     RESTART=off   ;;
    1|group)   RESTART=group ;;
    run|each)  RESTART=run   ;;
    *)
        echo "RESTART 只支持 0/off、1/group、run/each，收到：$RESTART" >&2
        exit 1
        ;;
esac

BUILD=${BUILD:-./build-rel}
SERVER=$BUILD/echo_server_mt
CLIENT=$BUILD/bench_client
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-8888}
CONNS=${CONNS:-100}
MSGS=${MSGS:-5000}
PAYLOAD=${PAYLOAD:-1024}
GAP=${GAP:-0.3}

DATE=$(date +%Y-%m-%d)
OUT=benchmarks/results/$DATE
REPORT=$OUT/repeat-io${SRV_IO}-conns${CONNS}-size${PAYLOAD}.md

[ -x "$SERVER" ] || { echo "缺少 $SERVER，先构建 release 版"; exit 1; }
[ -x "$CLIENT" ] || { echo "缺少 $CLIENT"; exit 1; }

mkdir -p "$OUT"

SERVER_PID=""
SERVER_STARTS=0

kill_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=""
    fi
    pkill -x echo_server_mt 2>/dev/null
}

cleanup() {
    kill_server
}
trap cleanup EXIT

start_server() {
    kill_server
    sleep 0.3
    "$SERVER" "$PORT" "$SRV_IO" > /tmp/repeat_srv.log 2>&1 &

    # 等端口真的能连上再开始，比固定 sleep 0.5 可靠（重启后尤其重要）
    local ready=0
    for _ in $(seq 1 30); do
        if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if [ "$ready" -eq 0 ]; then
        echo "服务端 3 秒内未就绪，/tmp/repeat_srv.log："
        sed 's/^/    /' /tmp/repeat_srv.log
        exit 1
    fi

    SERVER_PID=$(pgrep -x echo_server_mt | head -1)
    if [ -z "$SERVER_PID" ]; then
        echo "服务端启动失败，/tmp/repeat_srv.log："
        sed 's/^/    /' /tmp/repeat_srv.log
        exit 1
    fi
    SERVER_STARTS=$((SERVER_STARTS + 1))
    echo "  服务端启动 #$SERVER_STARTS (pid $SERVER_PID)"
}

case "$RESTART" in
    off)   RESTART_DESC="不重启（同一服务端进程）" ;;
    group) RESTART_DESC="每组前重启" ;;
    run)   RESTART_DESC="每次测量前重启" ;;
esac

# off 模式在循环前启动一次；group/run 模式在各自的循环点启动
[ "$RESTART" = off ] && start_server

echo "服务端 : $SERVER $PORT $SRV_IO"
echo "负载   : $CONNS 连接 × $MSGS 条 × ${PAYLOAD}B"
echo "重复   : 每条件 $REPEAT 次"
echo "重启   : $RESTART_DESC"
echo

# 中位数：抗离群值，比平均值可靠
median() {
    printf '%s\n' "$@" | sort -g | awk '
        NF == 0 || $1 !~ /^[0-9]/ { next }
        { v[++n] = $1 }
        END {
            if (n == 0)      { print "-" }
            else if (n % 2)  { printf "%.0f\n", v[(n + 1) / 2] }
            else             { printf "%.0f\n", (v[n / 2] + v[n / 2 + 1]) / 2 }
        }'
}

{
    echo "# 重复测量 · $(date '+%F %T')"
    echo
    echo "- 服务端：\`$SERVER $PORT $SRV_IO\`"
    echo "- 客户端：\`$CLIENT $HOST $PORT <线程> $MSGS $PAYLOAD --connections $CONNS\`"
    echo "- 每条件重复 **$REPEAT** 次，取中位数（3 次即去掉最高最低）"
    echo "- 重启策略：**$RESTART_DESC**"
    echo "- CPU：$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //') / $(nproc) 核"
    echo "- 构建：$(grep -m1 '^CMAKE_BUILD_TYPE' "$BUILD/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
    echo
    echo "| 客户端线程 | 样本 | QPS 中位数 | QPS 范围 | P99 中位数 | P99 范围 |"
    echo "|---|---|---|---|---|---|"
} > "$REPORT"

for ct in $CLIENT_LIST; do
    [ "$RESTART" = group ] && start_server

    echo "--- 客户端线程 = $ct ---"

    qs=()
    ps=()

    for ((i = 1; i <= REPEAT; i++)); do
        [ "$RESTART" = run ] && start_server

        out=$("$CLIENT" "$HOST" "$PORT" "$ct" "$MSGS" "$PAYLOAD" --connections "$CONNS" 2>&1)

        q=$(printf '%s\n' "$out" | awk '/^QPS[ \t]*:/    {print $3; exit}')
        p=$(printf '%s\n' "$out" | awk '/^P99[ \t(]/     {print $4; exit}')
        cert=$(printf '%s\n' "$out" | awk '/^connect success/ {print $4; exit}')

        [ -n "$q" ] || q="-"
        [ -n "$p" ] || p="-"

        qs+=("$q")
        ps+=("$p")

        printf '  run%d: QPS=%-10s P99=%-6s 连接成功=%s\n' "$i" "$q" "$p" "${cert:-?}"

        [ "$i" -lt "$REPEAT" ] && sleep "$GAP"
    done

    med_q=$(median "${qs[@]}")
    med_p=$(median "${ps[@]}")

    # 范围：过滤掉失败样本（"-"）后取 min / max
    q_valid=$(printf '%s\n' "${qs[@]}" | grep -v '^-$' | sort -g)
    p_valid=$(printf '%s\n' "${ps[@]}" | grep -v '^-$' | sort -g)

    if [ -n "$q_valid" ]; then
        rng_q="$(printf '%s\n' "$q_valid" | head -1) – $(printf '%s\n' "$q_valid" | tail -1)"
    else
        rng_q="-"
    fi
    if [ -n "$p_valid" ]; then
        rng_p="$(printf '%s\n' "$p_valid" | head -1) – $(printf '%s\n' "$p_valid" | tail -1)"
    else
        rng_p="-"
    fi

    printf '  中位数: QPS=%s   (范围 %s)\n' "$med_q" "$rng_q"
    echo

    echo "| $ct | $REPEAT | $med_q | $rng_q | $med_p | $rng_p |" >> "$REPORT"
done

{
    echo
    echo "> 判读：先看**组内范围**。同一条件三次差 >10% 说明环境不稳，中位数也不可信；"
    echo "> 组内稳定时，再比较不同条件的**中位数**，且小于单次波动的差距不算差异。"
    echo
    echo "- 服务端共启动 **$SERVER_STARTS** 次（重启策略：$RESTART_DESC）"
} >> "$REPORT"

echo "=========================================="
echo "报告：$REPORT"
echo "=========================================="
