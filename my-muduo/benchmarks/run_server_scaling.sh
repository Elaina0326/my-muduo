#!/usr/bin/env bash
# run_server_scaling.sh —— 固定客户端配置，只扫服务端 IO 线程数
#
# 为什么需要它：
#   bench_repeat.sh 默认让客户端线程数跟着服务端变（1/1/2/4/8），
#   于是 batch = conns/ct 也从 100 掉到 12.5，QPS 和 P99 都无法单独归因给服务端。
#   这里把客户端钉死：ct=8、conns=800 → batch 恒为 100，只有 io 线程数在变。
#
# 用法：
#   bash benchmarks/run_server_scaling.sh
#   REPEAT=3 bash benchmarks/run_server_scaling.sh          # 少跑几次
#   IO_LIST="1 4 8" bash benchmarks/run_server_scaling.sh   # 只跑三档
#
# 产出：
#   results/<date>/repeat-io<N>-conns800-size1024.md   每档的明细（由 bench_repeat.sh 生成）
#   results/<date>/scaling-summary.md                  汇总表（含相对 io=0 的倍数、单条 P99）
#
# 注意：io=0 时是 8 个客户端线程压 1 个服务端线程，这一档会被客户端"过度施压"，
#       它就是敏感性实验，不是服务端能力的上限。

set -u
cd "$(dirname "$0")/.." || exit 1

REPEAT=${REPEAT:-5}
CT=${CT:-8}
CONNS=${CONNS:-800}
MSGS=${MSGS:-1000}
PAYLOAD=${PAYLOAD:-1024}
IO_LIST=${IO_LIST:-"0 1 2 4 8"}
GAP=${GAP:-0.3}

DATE=$(date +%Y-%m-%d)
OUT=benchmarks/results/$DATE
BATCH=$((CONNS / CT))

[ -x "./build-rel/echo_server_mt" ] || { echo "缺少 build-rel/echo_server_mt"; exit 1; }
[ -x "./build-rel/bench_client" ]   || { echo "缺少 build-rel/bench_client"; exit 1; }

mkdir -p "$OUT"
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

echo "客户端固定 : $CT 线程 / $CONNS 连接 / batch $BATCH"
echo "负载       : $MSGS 条/连接 × ${PAYLOAD}B"
echo "重复       : 每档 $REPEAT 次，每次重启服务端"
echo "扫描       : 服务端 IO 线程 = $IO_LIST"
echo

for io in $IO_LIST; do
    echo "=============================================================="
    echo " 服务端 IO 线程 = $io"
    echo "=============================================================="

    CONNS="$CONNS" MSGS="$MSGS" PAYLOAD="$PAYLOAD" RESTART=run GAP="$GAP" \
        bash benchmarks/bench_repeat.sh "$REPEAT" "$io" "$CT"

    f=$OUT/repeat-io${io}-conns${CONNS}-size${PAYLOAD}.md
    if [ -f "$f" ]; then
        line=$(grep -E "^\| $CT \|" "$f" | head -1)
        q=$(printf '%s' "$line" | awk -F'|' '{gsub(/ /,"",$4); print $4}')
        rng=$(printf '%s' "$line" | awk -F'|' '{gsub(/^ +| +$/,"",$5); print $5}')
        p=$(printf '%s' "$line" | awk -F'|' '{gsub(/ /,"",$6); print $6}')
        printf '%s\t%s\t%s\t%s\n' "$io" "${q:--}" "${rng:--}" "${p:--}" >> "$TMP"
    else
        printf '%s\t-\t-\t-\n' "$io" >> "$TMP"
    fi
    echo
done

SUM=$OUT/scaling-summary.md
{
    echo "# 服务端扩展性（客户端配置固定）· $(date '+%F %T')"
    echo
    echo "- 客户端固定：**$CT 线程 / $CONNS 连接 / batch $BATCH**"
    echo "- 负载：$MSGS 条/连接 × ${PAYLOAD}B，loopback"
    echo "- 每档重复 **$REPEAT** 次，**每次重启服务端**，取中位数"
    echo "- CPU：$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //') / $(nproc) 线程"
    echo
    echo "| 服务端 IO | QPS 中位数 | QPS 范围 | 相对 io=0 | P99 batch (µs) | 单条 P99 约 (µs) |"
    echo "|---|---|---|---|---|---|"
    awk -F'\t' -v b="$BATCH" '
        $1 == 0 { base = $2 }
        { io[NR] = $1; q[NR] = $2; rng[NR] = $3; p[NR] = $4 }
        END {
            for (i = 1; i <= NR; i++) {
                ratio = (base + 0 > 0 && q[i] + 0 > 0) ? sprintf("%.2f×", q[i] / base) : "-"
                one   = (p[i] + 0 > 0) ? sprintf("%.1f", p[i] / b) : "-"
                printf "| %s | %s | %s | %s | %s | %s |\n", io[i], q[i], rng[i], ratio, p[i], one
            }
        }' "$TMP"
    echo
    echo "> **P99 是 batch RTT（一轮 $BATCH 条）**，单条 P99 由它除以 $BATCH 估算，"
    echo "> 只用于判断尾延迟随线程数的趋势，不是严格的分位数换算。"
    echo ">"
    echo "> io=0 这一档是 8 客户端线程压 1 服务端线程，属于敏感性对照，"
    echo "> **不代表服务端在单线程下的能力上限**。"
} > "$SUM"

echo "=============================================================="
echo "汇总：$SUM"
echo "=============================================================="
cat "$SUM"
