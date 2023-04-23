#ifndef BLOCKEDQUEUE
#define BLOCKEDQUEUE

#include<pthread.h>
#include<stdlib.h>



//队列里面的变量都是临界资源，访问之前都需要取得锁。
template<typename T>
class BlockedQueue{
public:
    BlockedQueue(int maxSize);
    ~BlockedQueue(){
        pthread_mutex_lock(m_mutex);
        if(m_array)
            delete[] m_array;
        
        pthread_mutex_unlock(m_mutex);
    }

    bool push(const T& item);
    bool pop(T& item);
    
    //判断空或者满
    bool full(){
        int curSize = 0;
        pthread_mutex_lock(m_mutex);
        curSize = m_size;
        pthread_mutex_unlock(m_mutex);
        return curSize;
    }
    bool empty(){
        bool res = false;
        pthread_mutex_lock(m_mutex);
        if(m_size == 0)
            res = true;
        pthread_mutex_unlock(m_mutex);
        return res;
    }
    //清空队列


private:
pthread_mutex_t* m_mutex;
pthread_cond_t* m_cond;
int m_maxsize;  //队列的最大长度
T* m_array;   //队列存储的元素
int m_size; //队列当前的大小
int m_front;  //当前队头的索引
int m_back;  //当前队尾的索引

};




template<typename T>
BlockedQueue<T>::BlockedQueue(int maxSize):m_size(0),m_front(-1),m_back(-1)  {
    if(maxSize<=0)
        exit(-1);
    
    m_mutex = new pthread_mutex_t;
    m_cond = new pthread_cond_t;
    m_maxsize = maxSize;
    m_array = new T[m_maxsize];
    pthread_mutex_init(m_mutex,NULL);
    pthread_cond_init(m_cond,NULL);

}

template<typename T>
bool BlockedQueue<T>::push(const T& item){
    pthread_mutex_lock(m_mutex);  //队列里面的资源需要互斥地访问
    if(m_size > m_maxsize){
        //这时队列已经满了 提醒消费者来消费
        pthread_cond_broadcast(m_cond);   //如果先解锁，再通知，那么解锁后有一个低优先级的线程执行上锁，然后再执行到wait
        pthread_mutex_unlock(m_mutex);
        return false;
    }
    m_back = (m_back+1)%m_maxsize;
    m_array[m_back] = item;
    m_size++;

    
    pthread_cond_broadcast(m_cond);
    pthread_mutex_unlock(m_mutex);

}

template<typename T>
bool BlockedQueue<T>::pop(T& item){
    pthread_mutex_lock(m_mutex);
    
    while(m_size<=0){
        //拿到互斥锁的时候是返回0的
        //如果有资源m_size就不是0了，直接跳出循环
        if(pthread_cond_wait(m_cond,m_mutex) != 0){
            pthread_mutex_unlock(m_mutex);
            return false;   //出错了才会返回false
        }
    }
    //等到了资源
    m_front = (m_front+1)%m_maxsize;
    item = m_array[m_front];
    m_size--;
    pthread_mutex_unlock(m_mutex);
    return true;
}


#endif