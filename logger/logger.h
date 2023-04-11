#ifndef LOGGER
#define LOGGER

#include "blockedQueue.h"
#include "../threadpool/locker.h"
#include <stdarg.h>
#include <string>
#include <string.h>
#include <time.h>
#include <sys/time.h>
//日志类在一个系统里面只有一个，所以是单例的
class Log{
public:
    static Log* get_instance(){
        static Log instance;
        return &instance;
    }

    //设置日志的配置，包括文件名，日志缓冲区大小，最大行数和最长日志请求队列
    bool init(const char* filename, int log_buf_size = 8192,int split_lines = 5000000, 
                    int max_queue_size = 3);

    //异步写日志的公有方法，调用里面私有方法
    static void* flush_log_thread(void* args);

    //真正写日志的函数，按照格式输出到日志文件里面，这个函数非常重要
    /*
    有四种等级，分别是
    0. DEBUG
    1. INFO
    2. WARN
    3. ERRO
    */
   //把数据加入
    void write_log(int level,const char* format,...);

    void flush(void);

private:
    Log(); //构造函数设置成私有，因为是单例模式
    virtual ~Log();  //子类可以有自己的析构方式，否则派生类就只能调用基类的虚构函数了

    //异步写日志的方法
    void * async_write_log();


//以下是成员变量
    char dir_name[128];  //路径名称
    char log_name[128];  //日志文件名称
    int m_split_lines; //日志的最大行数
    int m_log_buf_size;  //日志类缓冲区的大小
    long long m_count;  //记录当前日志的行数
    int m_today;  //按天分文件，记录今天是哪一天
    FILE* m_fp;  // 打开log的文件指针
    char *m_buf;  //要输出的内容，也就是缓冲区
    BlockedQueue<std::string> *m_log_queue; //阻塞队列
    bool m_is_async;   //设置是否同步
    locker m_mutext;
};


//这里是提供给用户使用的函数
#define LOG_DEBUG(format,...) Log::get_instance()->write_log(0,format,__VA_ARGS__)
#define LOG_INFO(format,...) Log::get_instance()->write_log(1,format,__VA_ARGS__)
#define LOG_WARN(format,...) Log::get_instance()->write_log(2,format,__VA_ARGS__)
#define LOG_ERROR(format,...) Log::get_instance()->write_log(3,format,__VA_ARGS__)

#endif