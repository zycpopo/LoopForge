#ifndef TCP_SERVER_EVENT_LOOP_HPP
#define TCP_SERVER_EVENT_LOOP_HPP

#include "poller.hpp"
#include "timer.hpp"

class EventLoop {
    private:
        using Functor = std::function<void()>;
        std::thread::id _thread_id;//线程ID
        int _event_fd;//eventfd唤醒IO事件监控有可能导致的阻塞
        std::unique_ptr<Channel> _event_channel;
        Poller _poller;//进行所有描述符的事件监控
        std::vector<Functor> _tasks;//任务池
        std::mutex _mutex;//实现任务池操作的线程安全
        TimerWheel _timer_wheel;//定时器模块
    public:
        //执行任务池中的所有任务
        void RunAllTask() {
            std::vector<Functor> functor;
            {
                std::unique_lock<std::mutex> _lock(_mutex);
                _tasks.swap(functor);
            }
            for (auto &f : functor) {
                f();
            }
            return ;
        }
        static int CreateEventFd() {
            int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (efd < 0) {
                ERR_LOG("CREATE EVENTFD FAILED!!");
                abort();//让程序异常退出
            }
            return efd;
        }
        void ReadEventfd() {
            uint64_t res = 0;
            int ret = read(_event_fd, &res, sizeof(res));
            if (ret < 0) {
                //EINTR -- 被信号打断；   EAGAIN -- 表示无数据可读
                if (errno == EINTR || errno == EAGAIN) {
                    return;
                }
                ERR_LOG("READ EVENTFD FAILED!");
                abort();
            }
            return ;
        }
        void WeakUpEventFd() {
            uint64_t val = 1;
            int ret = write(_event_fd, &val, sizeof(val));
            if (ret < 0) {
                if (errno == EINTR) {
                    return;
                }
                ERR_LOG("READ EVENTFD FAILED!");
                abort();
            }
            return ;
        }
    public:
        EventLoop():_thread_id(std::this_thread::get_id()), 
                    _event_fd(CreateEventFd()), 
                    _event_channel(new Channel(this, _event_fd)),
                    _timer_wheel(this) {
            //给eventfd添加可读事件回调函数，读取eventfd事件通知次数
            _event_channel->SetReadCallback(std::bind(&EventLoop::ReadEventfd, this));
            //启动eventfd的读事件监控
            _event_channel->EnableRead();
        }
        //三步走--事件监控-》就绪事件处理-》执行任务
        void Start() {
            while(1) {
                //1. 事件监控， 
                std::vector<Channel *> actives;
                _poller.Poll(&actives);
                //2. 事件处理。 
                for (auto &channel : actives) {
                    channel->HandleEvent();
                }
                //3. 执行任务
                RunAllTask();
            }
        }
        //用于判断当前线程是否是EventLoop对应的线程；
        bool IsInLoop() {
            return (_thread_id == std::this_thread::get_id());
        }
        void AssertInLoop() {
            assert(_thread_id == std::this_thread::get_id());
        }
        //判断将要执行的任务是否处于当前线程中，如果是则执行，不是则压入队列。
        void RunInLoop(const Functor &cb) {
            if (IsInLoop()) {
                return cb();
            }
            return QueueInLoop(cb);
        }
        //将操作压入任务池
        void QueueInLoop(const Functor &cb) {
            {
                std::unique_lock<std::mutex> _lock(_mutex);
                _tasks.push_back(cb);
            }
            //唤醒有可能因为没有事件就绪，而导致的epoll阻塞；
            //其实就是给eventfd写入一个数据，eventfd就会触发可读事件
            WeakUpEventFd();
        }
        //添加/修改描述符的事件监控
        void UpdateEvent(Channel *channel) { return _poller.UpdateEvent(channel); }
        //移除描述符的监控
        void RemoveEvent(Channel *channel) { return _poller.RemoveEvent(channel); }
        void TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb) { return _timer_wheel.TimerAdd(id, delay, cb); }
        void TimerRefresh(uint64_t id) { return _timer_wheel.TimerRefresh(id); }
        void TimerCancel(uint64_t id) { return _timer_wheel.TimerCancel(id); }
        bool HasTimer(uint64_t id) { return _timer_wheel.HasTimer(id); }
};

void Channel::Remove() { return _loop->RemoveEvent(this); }
void Channel::Update() { return _loop->UpdateEvent(this); }
void TimerWheel::TimerAdd(uint64_t id, uint32_t delay, const TaskFunc &cb) {
    _loop->RunInLoop(std::bind(&TimerWheel::TimerAddInLoop, this, id, delay, cb));
}
//刷新/延迟定时任务
void TimerWheel::TimerRefresh(uint64_t id) {
    _loop->RunInLoop(std::bind(&TimerWheel::TimerRefreshInLoop, this, id));
}
void TimerWheel::TimerCancel(uint64_t id) {
    _loop->RunInLoop(std::bind(&TimerWheel::TimerCancelInLoop, this, id));
}

#endif


