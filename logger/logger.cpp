#include "logger.h"


Log::Log():m_is_async(false),m_count(0){}


bool Log::init(const char* filename, int log_buf_size,int split_lines, int max_queue_size){
    //初始化一个日志类
    if(max_queue_size>=1){
        m_is_async = true;
        m_log_queue = new BlockedQueue<std::string>(max_queue_size);
        pthread_t tid;

        //flush_log_thread作为线程函数，这里表示要以异步的方式来写入日志  创建线程
        pthread_create(&tid,NULL,Log::flush_log_thread,NULL);
    }

    //创建缓冲区
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf,'\0',m_log_buf_size);

    //获取当前的时间
    time_t now = time(NULL);
    struct tm* sys_time;
    sys_time = localtime(&now);
    struct tm MyTime = *sys_time;

    m_split_lines = split_lines;
   

    const char* p = strrchr(filename,'/');  //从后往前 找到第一个/
    char log_full_name[300] = {0};
    if(p==NULL){
        //输入的文件名不是路径,直接以时间作为日志名
        snprintf(log_full_name,300,"%d_%02d_%02d_%s",MyTime.tm_year+1900,MyTime.tm_mon+1,MyTime.tm_mday,filename);
        strcpy(log_name,log_full_name);
    }
    else{

        strcpy(log_name,p+1);  //复制文件名
        strncpy(dir_name,filename,p-filename+1);  //复制路径名称
        snprintf(log_full_name,300,"%s%d_%02d_%02d_%s",dir_name,MyTime.tm_year+1900,MyTime.tm_mon+1,MyTime.tm_mday,log_name);
    }

    m_today = MyTime.tm_mday;
    m_fp = fopen(log_full_name,"a");  //在当前目录打开文件,返回的是一个文件流
    if(!m_fp){
        return false;
    }
    return true;
}


void Log::write_log(int level,const char* format,...){
    /*
    写入前判断当前的day是否日志创建的day，行数是不是超过了限制
        1. 如果超过创建的天，则新建，更新m_count
        2. 如果超过最大行数，在当前日志末尾加count/max_lines为后缀创建新的日志文件
    将系统的信息格式化后输出：格式化时间+内容
    */

   struct timeval now;
   int ret = gettimeofday(&now,NULL);
   
   struct tm* sys_tm = localtime(&now.tv_sec);
   struct tm MyTime = *sys_tm;
   char s[16] = {0};  //表示通知等级

   //日志分级
   switch(level){

    case 0:
    {
        strcpy(s,"[debug]:");
        break;
    }
    case 1:
    {
        strcpy(s,"[info]:");
        break;
    }
    case 2:
    {
        strcpy(s,"[warn]:");
        break;
    }
    case 3:
    {
        strcpy(s,"[erro]:");
        break;
    }
    default:
    {
        strcpy(s,"[info]:");
        break;
    }
   }

   m_mutext.lock();  //处理临界资源(日志文件) 需要上锁
   //更新现有的行数
   m_count++;

   //如果日志不是今天的，或者当前写入的日志已经是最大行数了
   //需要做一些相应的处理
   if(m_today != MyTime.tm_mday || m_count % m_split_lines == 0){

        char new_log[256] = {0};
        fflush(m_fp); //先把缓冲区的都写入
        fclose(m_fp);

        char tail[16] = {0};

        //格式化日志名字中的时间部分
        snprintf(tail,16,"%d_%02d_%02d_",MyTime.tm_year+1900, MyTime.tm_mon+1, MyTime.tm_mday);

        //如果不是今天的日志
        if(m_today != MyTime.tm_mday)
        {
            snprintf(new_log,225,"%s%s%s",dir_name,tail,log_name);
            m_today = MyTime.tm_mday;
            m_count = 0;
        }
        else{  //超过了行数
            snprintf(new_log,225,"%s%s%s.%lld",dir_name,tail,log_name,m_count/m_split_lines);
        }
        m_fp = fopen(new_log , "a");
   }

   m_mutext.unlock();

    va_list valst;
    va_start(valst,format);
    std::string log_str;
    m_mutext.lock();
    int n = snprintf(m_buf,48,"%d-%02d-%02d %02d:%02d:%02d %s",
                    MyTime.tm_year+1900,MyTime.tm_mon+1,MyTime.tm_mday,MyTime.tm_hour,MyTime.tm_min,MyTime.tm_sec,s);
    //打印用户要输出的信息
    int m = vsnprintf(m_buf+n,m_log_buf_size-1,format,valst);

    m_buf[n+m] = '\n';
    m_buf[n+m+1] = '\0';

    log_str = m_buf;
    m_mutext.unlock();   //m_buf也是一个临界区

    if(m_is_async && !m_log_queue->full()){
        m_log_queue->push(log_str);
    }
    else{
        m_mutext.lock();
        fputs(log_str.c_str(),m_fp);
        m_mutext.unlock();
    }

    va_end(valst);

}

void* Log::flush_log_thread(void*){
    Log::get_instance()->async_write_log();
}


void* Log::async_write_log(){
    std::string singleLog;
    //从阻塞队列里面取出一条日志，然后写入文件
    int i =0;
    while(m_log_queue->pop(singleLog))  //这里 队列是会阻塞的
    {   
        m_mutext.lock();
        fputs(singleLog.c_str(),m_fp);  //写入文件流
        i++;
        m_mutext.unlock();
        if(i == 1){
            flush();
            i=0;
        }
    }
    return NULL;
}

Log::~Log(){
    if(m_fp){
        fclose(m_fp);
    }
    if(m_buf){
        delete[] m_buf;
    }
    if(m_log_queue){
        delete m_log_queue;
    }

}

void Log::flush(){
    m_mutext.lock();
    fflush(m_fp);
    m_mutext.unlock();
}