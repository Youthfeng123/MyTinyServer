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

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 15
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
    printf("Receive an Sigalarm \n");
    char writeBuf[] = "SIGALARM";
    send(TimerPipe[1],writeBuf,strlen(writeBuf)+1,0);
    return;
}

void addsig(int sig, void( handler )(int)){// 注册信号处理函数
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    sigaction(sig,&sa,NULL);
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }
    //创建一个管道，用于监听信号 
    int suc = socketpair(AF_UNIX,SOCK_STREAM,0,TimerPipe);
    if(suc==-1){
        perror("socketpair");
        exit(-1);
    }
    int port = atoi( argv[1] );
    addsig( SIGPIPE, SIG_IGN );  //忽略SIGPIPE信号 ，往一个已经关闭的socket写东西时候会产生
    addsig(SIGALRM,AlarmHandler);   //注册信号，让程序接收到定时闹钟的信号的时候处理
    threadpool< http_conn >* pool = NULL;  //线程池对象
    try {
        pool = new threadpool<http_conn>;   //http_conn是线程要处理的请求对象
    } catch(...) {
        return -1;
    }

    http_conn* users = new http_conn[ MAX_FD ];  //创建好连接对象以免链接来了才新建，对象池

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );  //创建socket

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
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
    
    http_conn::m_epollfd = epollfd;
    bool timeout = false;

    //开始计时
    struct itimerval interval;
    interval.it_interval.tv_sec = 15;
    interval.it_value.tv_sec = 10;
    setitimer(ITIMER_REAL,&interval,NULL);

    while(true) {
        
        int number = epoll_wait( epollfd, events_arr, MAX_EVENT_NUMBER, -1 );  //阻塞,等待可读消息
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {  //
            int sockfd = events_arr[i].data.fd;  //拿到可以读写的socket
            
            if( sockfd == listenfd ) {
                
                struct sockaddr_in client_address;
                socklen_t size = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &size );
                printf("create a new connection,fd:%d\n",connfd);
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    perror("accept");
                    continue;
                } 
                                        //如果链接数目已经满了
                if( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd); //这个socket就不要了
                    continue;
                }
                //这里用了个哈希，让fd直接映射到对应的请求对象
                users[connfd].init( connfd, client_address);  //把这个socket封装成请求对象
                util_timer* newTimer = new util_timer(users+connfd);
                users[connfd].timer = newTimer;
                timer_list.add_timer(newTimer);

            } 
            else if( sockfd == TimerPipe[0]){
                //接收到SIGALARM信号了
                char readBuf[BUFFER_SIZE] = {0};
                recv(TimerPipe[0],readBuf,BUFFER_SIZE,0);
                if(strcasecmp(readBuf,"SIGALARM") == 0){
                    timeout = true;
                }

            }
            else if( events_arr[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) { //来自请求信息
                                            //这些不是由数据发来，而是有问题发生
                timer_list.del_timer(users[sockfd].timer);
                users[sockfd].close_conn();//所以直接关闭这个请求好了

            } 
            else if(events_arr[i].events & EPOLLIN) { //有数据来了

                if(users[sockfd].read()) {  //先获取数据 然后再加入请求队列
                                //传入的是地址
                    //更新定时器
                    time_t Now = time(NULL);
                    users[sockfd].timer->expire = Now+TIMESLOT;
                    timer_list.adjust_timer(users[sockfd].timer);
                    pool->append(users + sockfd);  //如果是读信息 ，让线程完成，加入请求队列
                } 
                else {
                    timer_list.del_timer(users[sockfd].timer);
                    users[sockfd].close_conn();
                }

            }  
            else if( events_arr[i].events & EPOLLOUT ) {  //写请求
                // printf("这里开始写东西了\n");
                if( !users[sockfd].write() ) {  //主线程写东西
                    // printf("这里写失败了\n");
                    timer_list.del_timer(users[sockfd].timer);
                    users[sockfd].close_conn();
                    
                }
            }
        }
        if(timeout){
            timer_list.tick();
            timeout = false;
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}