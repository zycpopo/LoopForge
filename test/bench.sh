#!/usr/bin/env bash
# LoopForge(Evo-Socket) HTTP 示例压测脚本
#
# 用法：bash test/bench.sh
# 可调：DURATION=15 LEVELS="1 10 50 100" bash test/bench.sh
# 依赖：wrk（sudo apt install -y wrk）
#
# 记录结果时一定要一并记下环境（核数 / 内存 / 虚拟机配置 / wrk 参数），
# 脱开环境的 QPS 数字在面试里没有意义。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=8085
DURATION="${DURATION:-15}"
LEVELS="${LEVELS:-1 10 50 100}"
LOG="$ROOT/test/.bench.log"

command -v wrk >/dev/null || { printf '缺少 wrk，请先安装：sudo apt install -y wrk\n'; exit 1; }

( cd "$ROOT/source/http" && make >/dev/null ) || { printf '构建失败\n'; exit 1; }
( cd "$ROOT/source/http" && exec ./main >"$LOG" 2>&1 ) &
SRV=$!
trap 'kill "$SRV" 2>/dev/null' EXIT
sleep 1

rss() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null || printf '0'; }

printf '======== 测试环境 ========\n'
printf 'CPU 核数 : %s\n' "$(nproc)"
printf 'CPU 型号 : %s\n' "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
printf '内存总量 : %s kB\n' "$(awk '/MemTotal/{print $2}' /proc/meminfo)"
printf '服务进程 : pid=%s，初始 RSS=%s kB\n' "$SRV" "$(rss "$SRV")"
printf 'wrk 参数 : -t2 -c<并发> -d%ss，每次压测前重建连接\n' "$DURATION"

bench() { # $1=路径 $2=说明
    local path="$1" label="$2" out rps avg p99 non2xx
    printf '\n======== %s  http://127.0.0.1:%s%s ========\n' "$label" "$PORT" "$path"
    printf '%-8s %-16s %-14s %-12s %s\n' 并发 QPS 平均延迟 P99延迟 非2xx响应
    for c in $LEVELS; do
        out=$(wrk -t2 -c"$c" -d"${DURATION}s" --latency "http://127.0.0.1:$PORT$path" 2>/dev/null)
        rps=$(printf '%s' "$out" | awk '/Requests\/sec:/{print $2}')
        avg=$(printf '%s' "$out" | awk '/^\s+Latency/{print $2}')
        p99=$(printf '%s' "$out" | awk '/^\s+99%/{print $2}')
        non2xx=$(printf '%s' "$out" | awk '/Non-2xx/{print $NF}')
        printf '%-8s %-16s %-14s %-12s %s\n' \
            "$c" "${rps:--}" "${avg:--}" "${p99:--}" "${non2xx:-0}"
        printf '  RSS=%s kB\n' "$(rss "$SRV")"
    done
}

bench "/hello"     "动态路由"
bench "/index.html" "静态文件"

printf '\n======== 结论 ========\n'
printf '最终 RSS=%s kB，服务进程仍在运行=%s\n' "$(rss "$SRV")" "$(kill -0 "$SRV" 2>/dev/null && printf yes || printf no)"
printf '服务端日志: %s\n' "$LOG"
