//验证 Socket::Recv 对四种情况给出可区分的返回值：
//  >0 实际数据长度   0 本次无数据可读   -1 对端已关闭写端(EOF)   -2 接收出错
//
//其中 case 3 是本次修复的核心：旧实现用 `if (ret <= 0)` 把 recv()==0(EOF) 和
//EAGAIN 归成同一个返回值 0，并且依赖可能残留的 errno 做判断。下面的用例先制造
//一次 EAGAIN 把 errno 置位，再让对端半关闭，从而确定性地暴露这个误判。
#include "socket.hpp"

#include <cstdio>
#include <sys/socket.h>

static int g_failed = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) {                                         \
            printf("  [PASS] %s\n", msg);                   \
        } else {                                            \
            printf("  [FAIL] %s\n", msg);                   \
            g_failed++;                                     \
        }                                                   \
    } while (0)

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        return 2;
    }
    Socket reader(fds[0]);
    Socket writer(fds[1]);
    reader.NonBlock();
    writer.NonBlock();

    char buf[64];

    printf("case 1: 缓冲区无数据\n");
    ssize_t ret = reader.NonBlockRecv(buf, sizeof(buf));
    CHECK(ret == 0, "无数据可读返回 0（EAGAIN 语义）");

    printf("case 2: 缓冲区有数据\n");
    writer.Send("hello", 5);
    ret = reader.NonBlockRecv(buf, sizeof(buf));
    CHECK(ret == 5, "读到 5 字节，返回实际长度");

    printf("case 3: 对端关闭写端（先制造 EAGAIN，使 errno 残留为 EAGAIN）\n");
    //这一步会把 errno 置为 EAGAIN，重现真实场景——连接一旦发生过写阻塞(EAGAIN)
    //或 eventfd 空读，errno 就会残留 EAGAIN，此后对端关闭会被误判为"暂时无数据"
    reader.NonBlockRecv(buf, sizeof(buf));
    if (shutdown(writer.Fd(), SHUT_WR) != 0) {
        perror("shutdown");
        return 2;
    }
    ret = reader.NonBlockRecv(buf, sizeof(buf));
    CHECK(ret == -1, "对端半关闭返回 -1(EOF)，与 case 1 的 0 区分开");

    printf("case 4: 描述符非法\n");
    Socket bad(-1);
    ret = bad.NonBlockRecv(buf, sizeof(buf));
    CHECK(ret == -2, "接收出错返回 -2，与 EOF 的 -1 区分开");

    printf("\n结果: %s\n", g_failed == 0 ? "全部通过" : "存在失败用例");
    return g_failed == 0 ? 0 : 1;
}
