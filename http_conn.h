#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include<sys/epoll.h>
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<sys/mman.h>
#include<stdarg.h>
#include<errno.h>
#include "locker.h"
#include<sys/uio.h>
#include<string.h>
#include <sys/stat.h>


class http_conn{ //请求事件对象

public:
     /*  主状态机的三个状态  检查请求行、请求头、请求体
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析 请求行
        CHECK_STATE_HEADER:当前正在分析 头部字段
        CHECK_STATE_CONTENT:当前正在解析 请求体
    */


    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };

    //解析某一行的时候的状态
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    //              1.读取到一个完整的行 2.行出错   3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};  //用于判断请求类型
    
    
    /*    返回给客户端的处理信号  请求行的时候确定   主要看这一行是否完整
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求  //解析完请求体才能？
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDENREQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };




    static const char* rootDir ;
    static int m_epollfd;//所有的socket上的事件都被注册到同一个epoll上
    static int m_user_count; //统计用户的数量
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 2048;

    http_conn(){}

    ~http_conn(){}

    void process();  //处理客户的请求
    void init(int sockfd, const sockaddr_in& addr);//初始化新接受的客户信息
    void close_conn();  //关闭连接
    bool read();  //非阻塞地读
    bool write(); //非阻塞地写


    



private:
    int m_sockfd; //这个http连接的socket
    sockaddr_in m_address; //通信socket地址

    int m_checked_index; //当前正在分析的字符再读缓冲区中的位置  由parse_line函数处理
    int m_start_line; //当前正在解析的行的起始位置

    CHECK_STATE m_check_state ; //主状态机当前所处的状态
    //定义读写缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx ;//标识缓冲区已经读入客户端数据的最后一个字节的下一个位置
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx ; // next position to write


    char* m_url;   // 解析出请求的URL 
    char* m_version;  //协议版本
    METHOD m_method;   //解析出请求的方法
    char * m_host; //主机名
    bool m_linger;  //http请求是不是要保持连接

    struct stat m_stat;
    unsigned long m_content_length;  //请求报文中数据体的字节数目
    char m_resources_dir[100];  //资源路径
    char* m_resource_addr; //内存映射之后的地址
    struct iovec m_iov[2];  //io向量 跟writev搭配使用
    int m_iv_count;   //发送iov的个数
    int bytes_to_send; //发送的总体字节数
    int bytes_have_send;

    void init();   //初始化连接其余的信息
    HTTP_CODE process_read(); //解析HTTP的请求  
    //解析请求首行   请求头 请求体
    HTTP_CODE parse_request_line(char *text); 
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);

    

    LINE_STATUS parse_line();  //单纯解析一行
    HTTP_CODE do_request();
    char* get_line(){
        return m_read_buf + m_start_line;//  会操作m_start_line
    }

    bool process_write(HTTP_CODE );
    bool add_response(const char* format,...);

    bool unmap();
    bool add_status_line(int status, const char* title){
        return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
    }
    bool add_headers(int content_len){
        bool ret1 = add_content_length(content_len);
        bool ret2 = add_content_type();
        bool ret3 = add_blank_line();

    }
    bool add_content_length(int content_len){
        return add_response("Content-Length:%d\r\n",content_len);
    }
    bool add_content_type(){
        return add_response("Content-Type:%s\r\n","text/html");
    }
    bool add_linger(){
        if(m_linger)
            return add_response("Connection:keep-alive\r\n");
        else
            return add_response("Connection:close\r\n");
    }
    bool add_blank_line(){
        return add_response("\r\n");
    }
    bool add_content(const char* content){
        return add_response("%s",content);
    }
};


#endif