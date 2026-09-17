#!/usr/bin/env bash
# LoopForge(Evo-Socket) 修复验证脚本
#
# 用法：bash test/run_checks.sh
# 依赖：g++、make、bash（用 /dev/tcp 建连，不需要 nc/curl）
#
# 想对比"修复前"的表现，先执行 git stash，再跑本脚本。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HTTP_PORT=8085
ECHO_PORT=8500
LOG_HTTP="$ROOT/test/.http.log"
LOG_ECHO="$ROOT/test/.echo.log"
PASS=0
FAIL=0
HTTP_PID=""
ECHO_PID=""

ok()   { printf '  [PASS] %s\n' "$1"; PASS=$((PASS + 1)); }
bad()  { printf '  [FAIL] %s\n' "$1"; FAIL=$((FAIL + 1)); }
info() { printf '\n== %s ==\n' "$1"; }
alive() { kill -0 "$1" 2>/dev/null; }

cleanup() {
    [ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null
    [ -n "$ECHO_PID" ] && kill "$ECHO_PID" 2>/dev/null
    return 0
}
trap cleanup EXIT

# 发送一个原始请求，回显响应首行（状态行）
raw() { # $1=端口 $2=请求内容
    local port="$1" payload="$2" out=""
    if ! exec 4<>"/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
        printf ''
        return 1
    fi
    printf '%s' "$payload" >&4
    out=$(timeout 3 cat <&4 | tr -d '\r' | head -1)
    exec 4<&- 4>&-
    printf '%s' "$out"
}

# 读取进程累计 CPU 时间（user+sys，单位 tick），用于判断事件循环是否空转
cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || printf '0'; }

# 统计日志中某个模式出现的次数
count_log() { # $1=文件 $2=模式
    local n="0"
    [ -f "$1" ] && n=$(grep -c "$2" "$1" 2>/dev/null)
    printf '%s' "${n:-0}"
}

# ---------------------------------------------------------------- 构建
info "1. 构建"
if ! ( cd "$ROOT/source/echo" && rm -f main && make ); then
    printf 'echo 示例构建失败，终止检查\n'
    exit 1
fi
if ! ( cd "$ROOT/source/http" && rm -f main && make ); then
    printf 'http 示例构建失败，终止检查\n'
    exit 1
fi
ok "两个示例均构建成功"

# ---------------------------------------------------------------- 单元测试
info "2. Socket::Recv 返回值语义（确定性复现 EOF 与 EAGAIN 混淆）"
if ( cd "$ROOT/test" && rm -f recv_semantics_test && make check ); then
    ok "单元测试全部通过，EOF/无数据/出错三种情况已可区分"
else
    bad "单元测试存在失败用例，recv 语义修复未生效"
fi

# ---------------------------------------------------------------- 启动服务
info "3. 启动示例服务"
( cd "$ROOT/source/http" && exec ./main >"$LOG_HTTP" 2>&1 ) &
HTTP_PID=$!
( cd "$ROOT/source/echo" && exec ./main >"$LOG_ECHO" 2>&1 ) &
ECHO_PID=$!
sleep 1
if alive "$HTTP_PID"; then ok "http 服务已启动 (pid=$HTTP_PID, 端口 $HTTP_PORT)"; else bad "http 服务启动失败"; fi
if alive "$ECHO_PID"; then ok "echo 服务已启动 (pid=$ECHO_PID, 端口 $ECHO_PORT)"; else bad "echo 服务启动失败"; fi

# ---------------------------------------------------------------- 基本功能
info "4. 基本功能回归"
line=$(raw "$HTTP_PORT" $'GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n')
case "$line" in
    *200*) ok "GET /hello 返回 200" ;;
    *)     bad "GET /hello 未返回 200（实际: ${line:-无响应}）" ;;
esac

# ---------------------------------------------------------------- 畸形请求
info "5. 畸形 Content-Length 的健壮性（修复前会直接终止整个进程）"
cl_probe() { # $1=用例名 $2=Content-Length 取值 $3=期望状态码
    local name="$1" value="$2" expect="$3" payload line
    if ! alive "$HTTP_PID"; then
        bad "$name：服务进程已不在，跳过"
        return
    fi
    payload=$(printf 'POST /login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: %s\r\n\r\n' "$value")
    line=$(raw "$HTTP_PORT" "$payload")
    if ! alive "$HTTP_PID"; then
        bad "$name：服务进程退出（进程级 DoS 未修复）"
        return
    fi
    case "$line" in
        *"$expect"*) ok "$name：进程存活，返回 $expect" ;;
        *)           bad "$name：期望 $expect，实际 ${line:-无响应}" ;;
    esac
}
cl_probe "非数字 Content-Length"   "abc"                        400
cl_probe "负数 Content-Length"     "-1"                         400
cl_probe "超长数字 Content-Length" "99999999999999999999999"    400
cl_probe "超上限 Content-Length"   "99999999"                   413

# ---------------------------------------------------------------- 连接回收
info "6. 客户端关闭后的连接回收与空转检测"
before=$(count_log "$LOG_HTTP" 'CLOSE CONNECTION')
{
    exec 5<>"/dev/tcp/127.0.0.1/$HTTP_PORT" 2>/dev/null
    printf 'GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' >&5
    # 读到响应首字节，确认服务端已写出响应且保持连接（keep-alive）
    timeout 2 head -c 1 <&5 >/dev/null
    # 客户端主动关闭：服务端应据此感知 EOF 并回收连接
    exec 5<&- 5>&-
} 2>/dev/null

c1=$(cpu_ticks "$HTTP_PID")
sleep 2
c2=$(cpu_ticks "$HTTP_PID")
delta=$((c2 - c1))

sleep 0.5
after=$(count_log "$LOG_HTTP" 'CLOSE CONNECTION')
if [ "$after" -gt "$before" ]; then
    ok "客户端关闭后服务端回收了连接（CLOSE 日志 $before -> $after）"
else
    bad "客户端关闭后服务端未回收连接，存在连接泄漏"
fi

# 空转判定：2 秒空闲期内 CPU 时间增量应远小于 HZ*2（通常 HZ=100，即 200 tick）
if [ "$delta" -lt 50 ]; then
    ok "空闲 2 秒 CPU 时间增量 ${delta} tick，事件循环未空转"
else
    bad "空闲 2 秒 CPU 时间增量 ${delta} tick，疑似事件循环空转（对端已关闭仍反复触发读事件）"
fi

# ---------------------------------------------------------------- 结果
printf '\n================ 结果 ================\n'
printf '通过 %d 项，失败 %d 项\n' "$PASS" "$FAIL"
printf 'http 日志: %s\n' "$LOG_HTTP"
printf 'echo 日志: %s\n' "$LOG_ECHO"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
