#ifndef LST_TIMER
#define LST_TIMER
#include <arpa/inet.h>
#include<stdio.h>
#include<time.h>


#define BUFFER_SIZE 64

class util_timer;

struct client_data{
    struct sockaddr_in address;  //用户的地址
    int sockfd;
    char buf[BUFFER_SIZE];
    util_timer* timer;  //这个用户专属的计时器
};
//这个是定时器链表中的元素

class util_timer{
public:
    util_timer():prev(NULL),next(NULL){}
    

    time_t expire;  //结束的时间点
    void (*cb_func)(client_data*);  //回调函数，时间到了以后调用
    client_data* user_data;  //  指向用户的指针
    util_timer* prev;
    util_timer* next;
};


//一个定时链表，它是一个按过期时间排序的双向链表，带有头尾指针

class sort_timer_lst{
public:
    sort_timer_lst():head(NULL),tail(NULL){}
    //析构函数 要销毁所有的定时器
    ~sort_timer_lst(){
        while (head)
        {
            util_timer* tmp = head;
            head = head->next;
            delete tmp;
        }
        
    }

    void add_timer(util_timer* timer);  //把一个计时器加入链表
    void adjust_timer(util_timer* timer); //链表里面的元素的过期时间只会延长，所以只需要考虑向后移动的情形
    void del_timer(util_timer* timer);  //删除这个指针指向的节点
    void tick();  //主循环收到 SIGALARM以后，调用链表对象的这个方法，链表从头开始处理过期的事件

private:
    void add_timer(util_timer* timer, util_timer* lst_head);  //辅助函数，意思是往lst_head后面添加节点
util_timer* head;
util_timer* tail;
};


#endif