#include "sqlconnpool.h"



//单例模式下获取实例
connection_pool* connection_pool::GetInstance(){
    static connection_pool instance;  //懒汉模式 ，定义一个静态对象
    return &instance;
}
connection_pool::connection_pool():m_CurConn(0),m_FreeConn(0),m_MaxConn(0)  {
}

void connection_pool::init(const std::string& url ,const std::string& User, const std::string& PassWord, const std::string& DBName, int Port, unsigned int MaxConn){
    this->m_url = url;
    this->m_User = User;
    this->m_PassWord = PassWord;
    this->m_DatabaseName = DBName;
    this->m_Port = Port;


    mutex.lock();

    for(int i = 0 ;i<MaxConn;i++){  //尝试循环建立这么多连接 保存在链表里面
        MYSQL* conn = mysql_init(nullptr);
        if(!conn){
            std::cout<<"Mysql init Error!"<<std::endl;
            exit(0);
        }
        conn = mysql_real_connect(conn,m_url.c_str(),m_User.c_str(),m_PassWord.c_str(),m_DatabaseName.c_str(),Port,NULL,0);
        if(!conn){
            std::cout<<"Mysql Connection Error!"<<std::endl;
            exit(0);
        }
        ConnList.push_back(conn);  //保存到链表里面
        m_FreeConn++;
    }
    mutex.unlock();  //其实不上锁也可以

    std::cout<<"Successfully create "<<m_FreeConn<<" connections"<<std::endl;
    reserve = sem(m_FreeConn);
    m_MaxConn = m_FreeConn;
}

int connection_pool::GetFreeConn(){
    return this->m_FreeConn;
}

MYSQL* connection_pool::GetConnection(){
    MYSQL* Conn = nullptr;
    if(GetFreeConn() == 0){
        return nullptr;
    }
    //访问临界资源之前，先确认是否还剩余资源
    reserve.wait();
    mutex.lock();

    Conn = ConnList.front();
    ConnList.pop_front();
    --m_FreeConn;
    ++m_CurConn;
    
    mutex.unlock();
    //访问完成以后解锁
    return Conn;
}

bool connection_pool::ReleaseConnection(MYSQL* con){
    if(!con){
        return false;
    }
    mutex.lock();

    ConnList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;

    mutex.unlock();
    reserve.post(); //唤醒正在等待资源的线程

    return true;
}

bool connection_pool::DestroyPool(){
    if(m_CurConn != m_MaxConn)
        return false;  //还是确保连接全部收回来再释放

    mutex.lock();
    if(ConnList.size()>0){
        std::list<MYSQL*>::iterator it;
        for(it = ConnList.begin();it!=ConnList.end();it++){
            MYSQL* conn = *it;
            mysql_close(conn);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        m_MaxConn = 0;
        ConnList.clear();
    }
    mutex.lock();
    return true;
}

connection_pool::~connection_pool(){
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL** conn, connection_pool* connPool){
    *conn = connPool->GetConnection();

    conRAII = *conn;
    poolRAII = connPool;

}

connectionRAII::~connectionRAII(){
    poolRAII->ReleaseConnection(conRAII);
}