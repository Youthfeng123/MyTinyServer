#ifndef LOCKER_H
#define LOCKER_H
#include<exception>
#include<pthread.h>
#include<semaphore.h>
//线程同步机制封装类
//互斥锁类

class locker{

public:
    //初始化
    locker(){
        if(pthread_mutex_init(&m_mutext,NULL)!=0){  //初始化一个互斥锁
            throw  std::exception(); //抛出一个异常
        }

    }
    //上锁
    bool lock(){
        //上锁 如果已经被占用，则会被阻塞
        return pthread_mutex_lock(&m_mutext) == 0;
    }
    //解锁
    bool unlock(){
        //解锁 用于修改临界资源完成时 解锁让其它线程能够操作临界资源
        return pthread_mutex_unlock(&m_mutext) == 0;
    }
    //拿锁变量
    pthread_mutex_t* get(){
        return &m_mutext;
    }

    ~locker(){
        pthread_mutex_destroy(&m_mutext);
    }

private:
pthread_mutex_t m_mutext;  //访问同一个资源的时候，需要对同一个锁执行上锁
};

//条件变量类，要跟互斥锁一起使用，基本模型是：生产者-消费者模型
//要索取资源的时候 要先上锁 wait()用于确定是否有资源，如果没有资源，wait解锁mutex，让生产者能够生产，返回之前给mutex上锁
//生产者生产资源的时候 用signal 或者 boradcast 来唤醒阻塞的线程
//适用于资源数目没有限制的情况

class cond{
public:
    cond(){
        if(pthread_cond_init(&m_cond,NULL)!=0){
            throw std::exception();
        }
    }

    ~cond(){
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t* mutex){
        return pthread_cond_wait(&m_cond,mutex)==0;
    }

    bool timedwait(pthread_mutex_t* mutex,struct timespec t){
        return pthread_cond_timedwait(&m_cond,mutex,&t)==0;
    }

    bool signal(pthread_mutex_t* mutex){  //唤醒一个或者多个线程
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast(){//所有的线程都唤醒
         return pthread_cond_broadcast(&m_cond)==0;
    }   

private:
    pthread_cond_t m_cond;

};

//信号量类
//生产者-消费者模型，一般需要两个锁量描述生产者和消费者各自的资源
//消费者用wait来索取资源，生产者用post生产资源

class sem{
public:
    sem(){
        if(sem_init(&m_sem,0,0)!=0){
            throw std::exception();
        }
    }
    sem(int num){
        if(sem_init(&m_sem,0,num) != 0){
            throw std::exception();
        }
    }

    ~sem(){
        sem_destroy(&m_sem);
    }

    bool wait(){
        return sem_wait(&m_sem)==0;
    }

    bool post(){
        return sem_post(&m_sem)== 0;
    }

private:

    sem_t m_sem;
};

#endif