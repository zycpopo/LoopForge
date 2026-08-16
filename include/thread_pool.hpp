#ifndef TCP_SERVER_THREAD_POOL_HPP
#define TCP_SERVER_THREAD_POOL_HPP

#include "event_loop.hpp"

class LoopThread {
    private:
        /*用于实现_loop获取的同步关系，避免线程创建了，但是_loop还没有实例化之前去获取_loop*/
        std::mutex _mutex;          // 互斥锁
        std::condition_variable _cond;   // 条件变量
        EventLoop *_loop;       // EventLoop指针变量，这个对象需要在线程内实例化
        std::thread _thread;    // EventLoop对应的线程
    private:
        /*实例化 EventLoop 对象，唤醒_cond上有可能阻塞的线程，并且开始运行EventLoop模块的功能*/
        void ThreadEntry() {
            EventLoop loop;
            {
                std::unique_lock<std::mutex> lock(_mutex);//加锁
                _loop = &loop;
                _cond.notify_all();
            }
            loop.Start();
        }
    public:
        /*创建线程，设定线程入口函数*/
        LoopThread():_loop(NULL), _thread(std::thread(&LoopThread::ThreadEntry, this)) {}
        /*返回当前线程关联的EventLoop对象指针*/
        EventLoop *GetLoop() {
            EventLoop *loop = NULL;
            {
                std::unique_lock<std::mutex> lock(_mutex);//加锁
                _cond.wait(lock, [&](){ return _loop != NULL; });//loop为NULL就一直阻塞
                loop = _loop;
            }
            return loop;
        }
};

class LoopThreadPool {
    private:
        int _thread_count;
        int _next_idx;
        EventLoop *_baseloop;
        std::vector<LoopThread*> _threads;
        std::vector<EventLoop *> _loops;
    public:
        LoopThreadPool(EventLoop *baseloop):_thread_count(0), _next_idx(0), _baseloop(baseloop) {}
        void SetThreadCount(int count) { _thread_count = count; }
        void Create() {
            if (_thread_count > 0) {
                _threads.resize(_thread_count);
                _loops.resize(_thread_count);
                for (int i = 0; i < _thread_count; i++) {
                    _threads[i] = new LoopThread();
                    _loops[i] = _threads[i]->GetLoop();
                }
            }
            return ;
        }
        EventLoop *NextLoop() {
            if (_thread_count == 0) {
                return _baseloop;
            }
            _next_idx = (_next_idx + 1) % _thread_count;
            return _loops[_next_idx];
        }
};

#endif


