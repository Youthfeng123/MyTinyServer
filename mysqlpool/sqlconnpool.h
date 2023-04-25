#ifndef SQLPOOL
#define SQLPOOL

#include<mysql/mysql.h>
#include<iostream>
#include<string>
#include<list>
#include<errno.h>
#include<string.h>
#include<stdio.h>
#include "../threadpool/locker.h"




class connection_pool{  //连接池只能是单例模式实现
public:
    static connection_pool* GetInstance();



    void init(const std::string& url ,const std::string& User, const std::string& PassWord, 
                            const std::string& DBName, int Port, unsigned int MaxConn);
//                                                  连接的端口号          最大连接数目
    MYSQL* GetConnection();  //返回一个连接至mysql的连接
    bool ReleaseConnection(MYSQL* conn); //释放一个连接 
    bool DestroyPool(); //关闭所有连接，清空链表，重置成员变量
    int GetFreeConn(); //获取当前空闲的连接数


private:
//构造函数和析构函数
    connection_pool();
    ~connection_pool();
    locker mutex;  //连接链表是多线程的临界资源，需要上锁
    sem reserve; //用于描述还剩下多少个连接
    //连接池链表
    std::list<MYSQL *> ConnList;

    //成员变量
    int m_MaxConn;
    int m_CurConn;  //当前正在被使用的连接数目
    int m_FreeConn;  //剩余的连接数目
    std::string m_url;  //登录主机的地址
    unsigned int  m_Port;    //端口
    std::string m_User; //用户名
    std::string m_PassWord; //密码
    std::string m_DatabaseName;  //数据库名称
};


//对连接池再进行一次封装  有什么用吗？

class connectionRAII{
public:
    connectionRAII(MYSQL** conn, connection_pool* connPool);

    ~connectionRAII();

private:
    MYSQL *conRAII;
    connection_pool* poolRAII;
};


#endif