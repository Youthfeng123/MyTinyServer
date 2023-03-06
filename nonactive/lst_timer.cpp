#include "../Headers.h"


util_timer::util_timer(http_conn* user):prev(NULL),next(NULL),user_data(user){
        expire = time(NULL);
        user_data->timer = this;
    }

void sort_timer_lst::add_timer(util_timer* timer){
    if(!timer)   //传进来什么玩意
        return;
    if(!head){  //链表为空
        timer->next = nullptr;
        timer->prev = nullptr;
        head = timer;
        tail = timer;
        return;
    }
    //走到这里，说明链表不为空，看看是不是加到表头
    if(timer->expire<head->expire){
        head->prev = timer;
        timer->prev = nullptr;
        timer->next = head;
        head = timer;
        return;
    }
    //走到这里说明不是加到表头 那么往后面加吧
    add_timer(timer,head);
}

void sort_timer_lst::add_timer(util_timer* timer,util_timer* lst_head){
    //递归的写法 好像更简单
    if(!lst_head->next){
        //如果已经遍历到最后一个节点了
        timer->next = nullptr;
        timer->prev = lst_head;
        lst_head->next = timer;
        tail = timer;
        return ;
    }
    //说明不是最后一个
    util_timer* nextNode = lst_head->next;
    if(timer->expire < nextNode->expire){
        //找到比自己更晚过期的节点了
        timer->next = nextNode;
        nextNode->prev = timer;
        timer->prev = lst_head;
        lst_head->next = timer;
        return;
    }
    else{
        //还没找到
        add_timer(timer,lst_head->next);
    }
}

void sort_timer_lst::adjust_timer(util_timer* timer){ 
    if(!timer)
        return;
     //调整输入的这个节点，时间已经改好了，而且expire只会更大
    if(!timer->next)
        return;  //已经是最后一个了
    if(timer->expire <= timer->next->expire)
        return;  //不需要调整
    //需要调整，而且不是最后一个

    if(timer==head){
        head = timer->next;
        head->prev = nullptr;
        add_timer(timer,head);
    }
    else{
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer,timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer* timer){
    if(!timer)
        return;
    if(timer==head&&timer==tail){
        //链表中只有一个元素
        delete timer;
        head = nullptr;
        tail = nullptr;
        return;
    }
    if(timer == head){
        head = timer->next;
        head->prev = nullptr;
        delete timer;
        return;
    }
    if(timer == tail){
        tail = timer->prev;
        tail->next = nullptr;
        delete timer;
        return;
    }

    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
    return;

}

void sort_timer_lst::tick(){
    //处理过期的事件
    if(!head)  //链表已经空了
        return;
    time_t cur = time(NULL); //现在的时间  就个数 别管是什么了
    //遍历所有过时的对象
    while(head&&head->expire < cur){  //如果当前的头已经过期了
        
        util_timer* CurNode = head;
        printf("close One here,fd:%d\n",CurNode->user_data->m_sockfd);
        head = head->next;
        if(head)
            head->prev = nullptr;
        CurNode->user_data->close_conn();
        delete CurNode;
    }
    if(head==nullptr){
        tail=nullptr;
    }


}