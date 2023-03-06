#ifndef THREADPOOL_H
#define THREADPOOL_H
#include<pthread.h>
#include<list>

#include<stdio.h>
#include "locker.h"
//线程池类，模板是为了代码的复用 T就是任务类
template<typename T>
class threadpool{

public:         //事先创建好的线程个数      最大请求数目
    threadpool(int thread_number=8,int max_requests = 10000);

    ~threadpool();

    bool append(T* request);
private:
    static void* worker(void* arg);
    void run();
private:
    //线程的数量
    int m_thread_number;

    //线程池组; 大小是m_thread_number
    pthread_t* m_threads;  //线程池

    //请求队列中最多允许请求的数量
    int m_max_requests;
    
    //请求队列
    std::list<T*> m_workqueue;   //队列： push_back  pop_front

    //互斥锁
    locker m_queuelocker;

    //信号量，用于判断是否有任务

    sem m_queuestat;  //任务是信号量  是临界资源  初始值为0

    //是否结束线程
    bool m_stop;

};

template<typename T>
threadpool<T>::threadpool(int thread_number,int max_requests):
    m_thread_number(thread_number),m_max_requests(max_requests),m_stop(false),m_threads(NULL){

        if((thread_number<=0)||max_requests<=0){
            throw std::exception();
        }  //非法输入

        m_threads = new pthread_t[m_thread_number];  //动态申请内存
        if(!m_threads){
            throw std::exception();
        }

        //创建 m_thread_number个线程，并设置为线程脱离
        for(int i =0; i< m_thread_number;i++){
            printf("create the %dth thread\n",i);        //传入this是为了让线程能够访问到内存池的数据
            if(pthread_create(&m_threads[i],NULL,worker,this)!=0){  //c++里面 worker必须是一个静态的函数
                delete[] m_threads;
                throw std::exception();
            }
            //设置线程分离，因为构造函数会结束
            if(pthread_detach(m_threads[i])!=0){
                delete[] m_threads;
                throw std::exception();
            }
        }


    }




template<typename T>

threadpool<T>::~threadpool(){
    delete[] m_threads;  //释放资源
    m_stop = true; //让子线程停止  线程的生命周期和对象一致


}


template<typename T>
bool threadpool<T>::append(T* request){
    m_queuelocker.lock(); //调用临界资源之前要上锁
    if(m_workqueue.size()>m_max_requests){
        m_queuelocker.unlock();
        return false;   //不处理这个请求
    }

    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//添加信号量  唤醒阻塞的线程来处理请求

    return true;
}


template<typename T>
void* threadpool<T>::worker(void* arg){  //这个必须是静态函数 所以要多以层静态worker
    
    threadpool *pool = (threadpool*) arg;  //这里拿到了对象的资源
    pool->run();
    return pool;  //没用
}

template<typename T>
void threadpool<T>::run(){  //线程干的活  调用的是对象的成员方法
    while(!m_stop){
        m_queuestat.wait();  //先看看有没有资源
        m_queuelocker.lock();  //上锁
        if(m_workqueue.empty()){
            m_queuelocker.unlock();  //这里已经用信号量了 按理来说是有的
            continue;
        }
        T* request = m_workqueue.front(); //取出请求
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if(!request){  //如果请求为空
            continue;
        }

        request->process();  //调用请求的方法
    }   
}
#endif