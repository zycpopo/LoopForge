#ifndef TCP_SERVER_BUFFER_HPP
#define TCP_SERVER_BUFFER_HPP

#include "server_common.hpp"

#define BUFFER_DEFAULT_SIZE 1024
//单个缓冲区的容量上限--超过即认为对端行为异常，由上层关闭连接，避免内存被耗尽
#define BUFFER_MAX_SIZE (64 * 1024 * 1024)
//容量回收阈值：缓冲区空且容量超过该值时才真正缩容，避免小请求也反复分配释放
#define BUFFER_SHRINK_THRESHOLD (1024 * 1024)
class Buffer {
    private:
        std::vector<char> _buffer; //使用vector进行内存空间管理
        uint64_t _reader_idx; //读偏移
        uint64_t _writer_idx; //写偏移
    public:
        Buffer():_reader_idx(0), _writer_idx(0), _buffer(BUFFER_DEFAULT_SIZE){}
        char *Begin() { return &*_buffer.begin(); }
        //获取当前写入起始地址, _buffer的空间起始地址，加上写偏移量
        char *WritePosition() { return Begin() + _writer_idx; }
        //获取当前读取起始地址
        char *ReadPosition() { return Begin() + _reader_idx; }
        //获取缓冲区末尾空闲空间大小--写偏移之后的空闲空间, 总体空间大小减去写偏移
        uint64_t TailIdleSize() { return _buffer.size() - _writer_idx; }
        //获取缓冲区起始空闲空间大小--读偏移之前的空闲空间
        uint64_t HeadIdleSize() { return _reader_idx; }
        //获取可读数据大小 = 写偏移 - 读偏移
        uint64_t ReadAbleSize() { return _writer_idx - _reader_idx; }
        //获取当前分配的容量
        uint64_t Capacity() { return _buffer.size(); }
        //将读偏移向后移动
        void MoveReadOffset(uint64_t len) {
            if (len == 0) return;
            //向后移动的大小，必须小于可读数据大小
            assert(len <= ReadAbleSize());
            _reader_idx += len;
            //数据全部读完后把读写偏移一起归零，避免偏移持续右移导致缓冲区反复扩容
            if (_reader_idx == _writer_idx) {
                _reader_idx = 0;
                _writer_idx = 0;
            }
        }
        //数据已全部读完、且容量远超初始值时，把容量回收到初始大小，
        //避免个别超大请求把内存长期占住
        void Shrink() {
            if (ReadAbleSize() != 0 || Capacity() <= BUFFER_SHRINK_THRESHOLD) {
                return;
            }
            std::vector<char> tmp(BUFFER_DEFAULT_SIZE);
            _buffer.swap(tmp);
            _reader_idx = 0;
            _writer_idx = 0;
        }
        //将写偏移向后移动 
        void MoveWriteOffset(uint64_t len) {
            //向后移动的大小，必须小于当前后边的空闲空间大小
            assert(len <= TailIdleSize());
            _writer_idx += len;
        }
        //确保可写空间足够（整体空闲空间够了就移动数据，否则就扩容）
        //超出容量上限时返回false，调用方应放弃本次写入并关闭连接
        bool EnsureWriteSpace(uint64_t len) {
            if (len > BUFFER_MAX_SIZE) {
                ERR_LOG("BUFFER WRITE EXCEED LIMIT:%lu", (unsigned long)len);
                return false;
            }
            //如果末尾空闲空间大小足够，直接返回
            if (TailIdleSize() >= len) { return true; }
            //末尾空闲空间不够，则判断加上起始位置的空闲空间大小是否足够, 够了就将数据移动到起始位置
            if (len <= TailIdleSize() + HeadIdleSize()) {
                //将数据移动到起始位置
                uint64_t rsz = ReadAbleSize();//把当前数据大小先保存起来
                std::copy(ReadPosition(), ReadPosition() + rsz, Begin());//把可读数据拷贝到起始位置
                _reader_idx = 0;    //将读偏移归0
                _writer_idx = rsz;  //将写位置置为可读数据大小， 因为当前的可读数据大小就是写偏移量
                return true;
            }
            //总体空间不够，需要扩容：上限以"可读数据量"为准，与读取侧的限制保持同一口径
            if (ReadAbleSize() + len > BUFFER_MAX_SIZE) {
                ERR_LOG("BUFFER RESIZE EXCEED LIMIT:%lu", (unsigned long)(ReadAbleSize() + len));
                return false;
            }
            //扩容前先把可读数据搬回起始位置，避免读偏移残留导致多余的容量增长
            uint64_t left = ReadAbleSize();
            if (left > 0 && _reader_idx > 0) {
                std::copy(ReadPosition(), ReadPosition() + left, Begin());
                _reader_idx = 0;
                _writer_idx = left;
            }
            DBG_LOG("RESIZE %ld", ReadAbleSize() + len);
            _buffer.resize(ReadAbleSize() + len);
            return true;
        }
        //写入数据，返回false表示超出容量上限、数据未被写入
        bool Write(const void *data, uint64_t len) {
            //1. 保证有足够空间，2. 拷贝数据进去
            if (len == 0) return true;
            if (EnsureWriteSpace(len) == false) {
                return false;
            }
            const char *d = (const char *)data;
            std::copy(d, d + len, WritePosition());
            return true;
        }
        bool WriteAndPush(const void *data, uint64_t len) {
            if (Write(data, len) == false) {
                return false;
            }
            MoveWriteOffset(len);
            return true;
        }
        bool WriteString(const std::string &data) {
            return Write(data.c_str(), data.size());
        }
        bool WriteStringAndPush(const std::string &data) {
            if (WriteString(data) == false) {
                return false;
            }
            MoveWriteOffset(data.size());
            return true;
        }
        bool WriteBuffer(Buffer &data) {
            return Write(data.ReadPosition(), data.ReadAbleSize());
        }
        bool WriteBufferAndPush(Buffer &data) {
            uint64_t len = data.ReadAbleSize();
            if (WriteBuffer(data) == false) {
                return false;
            }
            MoveWriteOffset(len);
            return true;
        }
        //读取数据
        void Read(void *buf, uint64_t len) {
            //要求要获取的数据大小必须小于可读数据大小
            assert(len <= ReadAbleSize());
            std::copy(ReadPosition(), ReadPosition() + len, (char*)buf);
        }
        void ReadAndPop(void *buf, uint64_t len) {
            Read(buf, len);
            MoveReadOffset(len);
        }
        std::string ReadAsString(uint64_t len) {
            //要求要获取的数据大小必须小于可读数据大小
            assert(len <= ReadAbleSize());
            std::string str;
            str.resize(len);
            Read(&str[0], len);
            return str;
        }
        std::string ReadAsStringAndPop(uint64_t len) {
            assert(len <= ReadAbleSize());
            std::string str = ReadAsString(len);
            MoveReadOffset(len);
            return str;
        }
        char *FindCRLF() {
            char *res = (char*)memchr(ReadPosition(), '\n', ReadAbleSize());
            return res;
        }
        /*通常获取一行数据，这种情况针对是*/
        std::string GetLine() {
            char *pos = FindCRLF();
            if (pos == NULL) {
                return "";
            }
            // +1是为了把换行字符也取出来。
            return ReadAsString(pos - ReadPosition() + 1);
        }
        std::string GetLineAndPop() {
            std::string str = GetLine();
            MoveReadOffset(str.size());
            return str;
        }
        //清空缓冲区
        void Clear() {
            //只需要将偏移量归0即可
            _reader_idx = 0;
            _writer_idx = 0;
        }
};

#endif


