#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "Headers.h"
#include <sys/time.h>
#include<string>
#include<map>
#include<iostream>
#include "./mysqlpool/sqlconnpool.h"



const unsigned int MAX_FD = 65536;
const unsigned int MAX_EVENT_NUMBER = 10000;
const unsigned int TIMESLOT = 15;


//预先定义事件类别
class sort_timer_lst;
class util_timer;

//创建时间事件
static sort_timer_lst timer_list;
// 添加文件描述符
extern void addfd( int epollfd, int fd, bool one_shot );
extern void removefd( int epollfd, int fd );
static int TimerPipe[2];


void AlarmHandler(int Signal){
    //我接受到了信号，要往信号通 信管道写东西，通知主循环，有事件过期了
    // printf("Receive an Sigalarm \n");
    char writeBuf[] = "SIGALARM";
    send(TimerPipe[1],writeBuf,strlen(writeBuf)+1,0);
    return;
}

void addsig(int sig, void( handler )(int)){// 注册信号处理函数
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );  //清空这段内存
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );  //注册的时候屏蔽信号
    sigaction(sig,&sa,NULL);
}

int main( int argc, char* argv[] ) {
    //server loop 
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }
    //创建一个管道，用于监听事件过期信号 
    int suc = socketpair(AF_UNIX,SOCK_STREAM,0,TimerPipe);

    if(suc==-1){
        perror("socketpair");
        exit(-1);
    }


    int port = atoi( argv[1] );
    addsig( SIGPIPE, SIG_IGN );  //忽略SIGPIPE信号 ，往一个已经关闭的socket写东西时候会产生,因为这个信号默认会关闭进程
    addsig(SIGALRM,AlarmHandler);   //注册信号，让程序接收到定时闹钟的信号的时候处理


    threadpool< http_conn >* pool = NULL;  //线程池对象
    try {
        pool = new threadpool<http_conn>;   //http_conn是线程要处理的请求对象 创建一个线程池  把线程池放在堆里，可以与其它线程共享
    } 
    catch(...) {
        return -1;
    }
    //  提前创建好连接
    http_conn* users = new http_conn[ MAX_FD ];  //创建好连接对象以免链接来了才新建，对象池

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );  //创建socket

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 设置端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) ); //设置端口复用
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events_arr[ MAX_EVENT_NUMBER ];  //用来装epoll_wait返回的事件
    int epollfd = epoll_create( 5 );  //5没什么意义
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false );  //反正也只有主线程读取 不需要EPOLLONESHOT
    addfd(epollfd,TimerPipe[0],false);   //注册提示信号，让发来的跟事件一起 统一在循环中处理
    http_conn::m_epollfd = epollfd;  //让连接对象记录epoll表的索引
    bool timeout = false;

    //开始计时
    struct itimerval interval;
    interval.it_interval.tv_sec = 15;
    interval.it_value.tv_sec = 20;
    setitimer(ITIMER_REAL,&interval,NULL);

    //初始化连接对象的数据库连接
    connection_pool* conn_pool = connection_pool::GetInstance();
    conn_pool->init("localhost","webserver","123321","serverdata",0,5);
    connectionRAII(&http_conn::mysqlConn,conn_pool);
    http_conn::init_mysqlConn();
    //这里采用的是reactor模仿proactor的IO处理模式，主程序读取数据以后，再由工作线程处理数据
    while(true) {
        
        int number = epoll_wait( epollfd, events_arr, MAX_EVENT_NUMBER, -1 );  //阻塞,等待可读消息
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );  //出错了
            break;
        }
        if( number<0 && errno == EINTR )
            continue;

        for ( int i = 0; i < number; i++ ) {  //
            int sockfd = events_arr[i].data.fd;  //拿到可以读写的socket
            
            if( sockfd == listenfd ) {
                //这里注意， listenfd是边沿触发，可能有了多个访客过来，所以需要用while循环来不断读取客户
                while(1){
                    struct sockaddr_in client_address;
                    socklen_t size = sizeof( client_address );
                    int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &size );

                    if ( connfd < 0 ) {
                        // perror("accept");
                        break;
                    }
                                            //如果链接数目已经满了
                    if( http_conn::m_user_count >= MAX_FD ) {
                        close(connfd); //这个socket就不要了
                        break;
                    }

                    // printf("create a new connection,fd:%d ,IP:%s, Port:%d\n",connfd,inet_ntoa(client_address.sin_addr),ntohs(client_address.sin_port));
                    //这里用了个哈希，让fd直接映射到对应的请求对象
                    users[connfd].init( connfd, client_address);  //把这个socket封装成请求对象
                    //给这个对象列入非活跃系统
                    util_timer* newTimer = new util_timer(users+connfd,time(NULL)+TIMESLOT);  //把当前的请求对象加入处理非活跃连接的链表
                    timer_list.add_timer(newTimer);
                }
            }

            else if( sockfd == TimerPipe[0]){
                //接收到SIGALARM信号了
                char readBuf[BUFFER_SIZE] = {0};
                recv(TimerPipe[0],readBuf,BUFFER_SIZE,0);
                if(strcasecmp(readBuf,"SIGALARM") == 0){  //如果收到了定期检查的信号
                    timeout = true;  //在这个循环结束时检查非活跃用户
                }

            }
            else if( events_arr[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) { //来自请求信息，它出错了

                //这些不是由数据发来，而是有问题发生
                timer_list.del_timer(users[sockfd].timer);  //从定时队列里面删除这个fd
                users[sockfd].close_conn();//所以直接关闭这个请求好了
            }

            else if(events_arr[i].events & EPOLLIN) { //有数据来了，读请求

                if(users[sockfd].read()) {  //先获 数据 然后再加入请求队列  主线程读取完数据了，这里是用reactor模仿proactor，让主线程读完数据，再让子线程处理数据
                                //传入的是地址
                    //更新定时器
                    time_t Now = time(NULL);
                    users[sockfd].timer->expire = Now+TIMESLOT; //设置
                    timer_list.adjust_timer(users[sockfd].timer);

                    pool->append(users + sockfd);  //如果是读信息 ，让线程完成，加入请求队列 让线程去处理数据
                }
                else {  //读失败了，那么关闭这个连接
                    timer_list.del_timer(users[sockfd].timer);
                    users[sockfd].close_conn();
                }

            }  

            else if( events_arr[i].events & EPOLLOUT ) {  //写请求  我在write里面自己设置了是否可写的监听
                // printf("这里开始写东西了\n");
                if( !users[sockfd].write() ) {  //主线程写东西
                    // 如果write返回false，则关闭连接
                    timer_list.del_timer(users[sockfd].timer);
                    users[sockfd].close_conn();
                    
                }
            }
        }

        //每检查一轮数据 开始更新响应队列
        if(timeout){
            timer_list.tick();
            timeout = false;
        }
    }

    
    //关闭服务器
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}