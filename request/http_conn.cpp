// #include "http_conn.h"
#include "../Headers.h"

// 类的静态值是需要初始化的
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";


//静态变量的定义
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;
const char *http_conn::rootDir = "./root"; 
MYSQL* http_conn::mysqlConn = nullptr;
std::map<std::string,std::string> http_conn::usr2psswd;
locker http_conn::m_sqlLock;

// 设置文件描述符非阻塞
void setnonblocking(int fd)
{
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK; // 设置一个非阻塞属性
    fcntl(fd, F_SETFL, new_flag);
}

// 添加文件描述符到epoll中 和从 epoll中删除
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP ; //对方异常断开，就通知我

    if (one_shot)
    {
        event.events = event.events|EPOLLONESHOT;   //如果ONESHOT了，那么epoll通知过一次后，就移出监听队列了，所以还得重新注册
    }

    event.events |= EPOLLET; // 设置成边沿触发
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞  因为ET模式一次把数据读完
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0); // 取消监听
    close(fd);                                // 关闭socket
}

// 修改文件描述符  要重置fd的上的ONESHOT时间，确保下一次可读的时候，EPOLLIN事件能够被触发
void modfd(int epollfd, int fd, int env)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = env | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::init_mysqlConn(){
    MYSQL* conn = http_conn::mysqlConn;
    if(!conn){
        std::cout<<"Mysql connection init failly"<<std::endl;
        exit(-1);
    }
    MYSQL_RES* res;
    MYSQL_ROW row;
    //成功执行的时候返回0
    http_conn::m_sqlLock.lock();
    if(mysql_query(conn,"SELECT username, passwd FROM user")){
        std::cout<<"Fail to execute a command to MYSQL"<<" "<<mysql_error(conn)<<std::endl;
        exit(-1);
    }   
    res = mysql_use_result(conn);

    while((row = mysql_fetch_row(res))!=nullptr){
        usr2psswd.insert({row[0],row[1]});
    }

    mysql_free_result(res);
    http_conn::m_sqlLock.unlock();

    std::cout<<"here is the users and passwords"<<std::endl;
    for(auto& x: usr2psswd){
        std::cout<<"user:"<<x.first<<" "<<"password:"<<x.second<<std::endl;
    }
}


void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 设置sockfd的端口复用，复用监听端口
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    // 添加到epoll对象
    addfd(m_epollfd, sockfd, true); // 让epoll监听这个文件描述符
    m_user_count++;                 // 数目自增

    init(); // 初始化一些其它的数据  同名重载
}

// 关闭连接
void http_conn::close_conn()
{
    if (m_sockfd != -1)
    {
        // printf("connection closed: %d\n",m_sockfd);
        removefd(m_epollfd, m_sockfd);  //取消epoll监听
        m_sockfd = -1;  //当前对象设为未使用
        m_user_count--;
        
    }
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始化状态为解析请求首行  这个是主状态机的初始状态
    m_checked_index = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;
    bzero(m_read_buf, READ_BUFFER_SIZE);
    m_content_length = 0;
    bzero(&m_stat,sizeof(m_stat));
    memset(m_resources_dir,0,100);
    bzero(m_write_buf,WRITE_BUFFER_SIZE);
    m_resource_addr = NULL;
    m_iv_count = 0;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_host = 0;
    m_write_idx = 0;
    cgi = false;

}

// 循环读取客户的数据，直到客户没有数据或者对方关闭连接，因为采用的是et工作模式
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {       
        return false; // 缓冲已经满了 直接返回（上一次的处理还没处理完呢）
    }

    int bytes_read = 0;  //记录已经读的字节数
    while (true)
    { // 读到已经读的后面           开始写入的位置              剩余的写入空间
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            { // 没有数据了
                break;
            }
            return false;
        }
        else if (bytes_read == 0)
        {
            // 对方关闭连接
            return false;
        }
        // 读到数据了
        m_read_idx += bytes_read;
    }
    return true;
}



// 由线程池中的工作线程调用，这个是处理HTTP请求的入口函数，调用这个函数的时候已经把数据读取到m_read_buf里面了
void http_conn::process()
{
    // 解析HTTP请求       //返回解析结果 对应前面的HTTP_CODE
    HTTP_CODE read_ret = process_read(); //  这个是有限状态机  顺序调用 会分别解析请求行 请求体 和 数据包，把对应的数据存入对象变量里面
    if (read_ret == NO_REQUEST)
    { // 请求的数据不完整
        // 需要重新读取数据
        modfd(m_epollfd, m_sockfd, EPOLLIN); // 因为设置了EPOLLONESHOT 需要重新设置flag
        //如果得到的请求数据都不完整，是不是别写了？
        //return ;//?
    }
    //read_ret 如果是FILE_REQUEST 那么把m_resorce_addr 发送出去
    // 生成响应
    bool write_ret = process_write(read_ret);   //这个只是把数据准备好了
    if(!write_ret){
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);  //响应已经生成了，让内核检查这个描述符能不能写

}



// 解析请求的函数
http_conn::HTTP_CODE http_conn::process_read()
{ // 主状态机
    // 这里的buffer里面 已经有一个完整的数据报了吧
    // 对于读来的数据 需要一行一行地解析
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char *text = NULL;
    // 一行一行地去解析
    // 检查请求体 说明不用一行一行解析了
    while ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK) || (line_status = parse_line()) == LINE_OK)
    {
        // 这一行没问题
        // 解析到了请求体或者解析到了一条完整的数据
        // 获取一行数据
        text = get_line();//text指向当前一行的开始
        m_start_line = m_checked_index; // 下一次解析的起点
        
        HTTP_CODE ret;
        switch (m_check_state)  //肯定是从请求行开始的
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text); // 解析请求头  只要返回不是BAD  就能继续
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }

        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text); // 这里只解析一行 解析所有行以后
                                       // 返回GET_REQUEST 处理请求头
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {                        // 已经得到了完整的请求，请求信息存在m_url m_method等等里面了
                return do_request(); // 处理具体请求的信息  如果成功处理文件 那么返回FILEREQUEST
            }
            break;
        }

        case CHECK_STATE_CONTENT:  //这里是不会运行到的
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }

        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }

    return NO_REQUEST;  //能运行到这里说明数据不完整，需要继续监听描述符
}

//请求方法  url  版本
//GET /562f25980001b1b106000338.jpg HTTP/1.1  请求行
//Host:img.mukewang.com  请求头
//User-Agent:Mozilla/5.0 (Windows NT 10.0; WOW64)
//AppleWebKit/537.36 (KHTML, like Gecko) Chrome/51.0.2704.106 Safari/537.36
//Accept:image/webp,image/*,*/*;q=0.8
//Referer:http://www.imooc.com/
//Accept-Encoding:gzip, deflate, sdch
//Accept-Language:zh-CN,zh;q=0.8
//空行
//请求数据为空
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 解析http请求行，获得请求方法 请求URL 和HTTP版本
    //  用正则表达式也可以  内容： GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 返回第一个空格的地方
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0'; // 切断

    char *method = text;  //方法在开头
    if (strcasecmp(method, "GET") == 0)
    {   
        m_method = GET;
    }
    else if(strcasecmp(method,"POST")==0){
        m_method = POST;
        cgi = true;
    }
    else
    {
        return BAD_REQUEST; // 因为不支持其它方法
    }
    //  /index.html HTTP/1.1  找到http前面那个空格
    m_version = strpbrk(m_url, " \t"); 
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    //  /index.html\0HTTP/1.1
    *m_version++ = '\0';  //其实这个版本没在其它地方用了
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST; // 只支持HTTP/1.1
    }
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/'); // /index.html  找到第一个字符是/的子字符串
    }

    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    // 处理资源路径
    if(strlen(m_url) == 1){
        strcat(m_url,"judge.html");
    }

    m_check_state = CHECK_STATE_HEADER; //改变状态机的状态 下一次检查请求头

    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 解析一行header
    // 主要解析几个东西： 是否保持连接？m_linger  content-length 和host

    if (!strlen(text) || text[0] == '\0')  //读到空行了
    {
        // printf("数据头解析完毕！\n");
        if(m_content_length>0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;  //还有请求体需要分析
        }
        return GET_REQUEST;
    }

    if (strncasecmp(text, "Connection:", 11) == 0)
    { // 检查到了Connection字段
        text += 11;
        text += strspn(text, " \t");
        // printf("Receive data:%s\n", text);
        if (strcasecmp(text, "keep-alive") == 0)
            m_linger = true; // 说明要保持连接
    }

    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        // printf("Receive data:%s\n", text);
        m_content_length = (unsigned)atol(text);
        
    }

    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
        
    }
   

    return NO_REQUEST;   //运行到这里，说明没有读到空行，那么请求头部分就没有解析完毕，需要继续解析
}

// 这里只判断我们读入的数据包 到底有没有完整地读取数据体
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // if (m_content_length == 0)
    //     return GET_REQUEST;  
    // else{
    //     if(text + m_content_length == m_checked_index)
    //         return GET_REQUEST;
    //     else
    //         return BAD_REQUEST;
    // }
    if (m_read_idx >= (m_content_length + m_checked_index))
    {
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }


    return NO_REQUEST;
}

// 解析一行 判断的依据是\r\n
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    // 遍历已经读到的字符
    for (; m_checked_index < m_read_idx; m_checked_index++)
    { // 遍历一行数据

        temp = m_read_buf[m_checked_index];

        //只处理\r和\n的地方，其余地方不管
        if (temp == '\r')
        {
            if ((m_checked_index + 1) == m_read_idx)
            {                     // 如果下一个字符就结束了
                return LINE_OPEN; // 这个不完整  因为下一个应该是\n
            }
            else if (m_read_buf[m_checked_index + 1] == '\n')
            { //  此时的内容 ****\0\0  把\r和\n隐去了
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK; // 这是完整的一行了
            }
            return LINE_BAD;  //\r后面不是\n 就说明不完整
        }
        else if (temp == '\n')  //如果是\n开始？  肯定先读到\r吧？
        { //走不到这里的吧
            if ((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r'))
            {
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;// \n前面不是\r说明不完整
        }
    }

    return LINE_OPEN; // 没读到\r\n
}

http_conn::HTTP_CODE http_conn::do_request()
{ // 获取某一个文件   可能是解析头部 也可能是解析数据体

    //走到这里 m_resources_dir是空的，m_url指向 /序号  或者/judge.html
    //如果请求的是页面的话，将会走到下面的if 分支，最后一个适用于其余的文件
    if(strcasecmp(m_url,"/judge.html") == 0){
        strcpy(m_resources_dir,rootDir);
        strcat(m_resources_dir,m_url);
    }
    else if(m_url[1] == '0'){
        strcpy(m_resources_dir,rootDir);
        strcat(m_resources_dir,"/register.html");
    }
    else if(m_url[1] == '1'){
        strcpy(m_resources_dir,rootDir);
        strcat(m_resources_dir,"/log.html");
    }            //登录界面访问的是2
    else if(cgi&&(m_url[1] == '2' || m_url[1] == '3')){   //2 和 3都需要通过数据库确认才能确认跳转页面 ，3是注册页面的请求，请求成功返回登录界面，否则返回注册失败界面
        //登录界面访问的是2，如果成功就返回welcom.html,否则logError.html

        //获取用户名和密码
        // m_string : user=qingfeng&password=123321
        char* name = strchr(m_string,'=')+1;
        char* end = strchr(name,'&');
        *end++ = '\0';
        char* pass = strchr(end,'=') +1;
        std::string user = name, passwd = pass;

        //目前先按照成功处理
        if(m_url[1] == '2'){ //登录  如果没注册 或者密码不对
            if(http_conn::usr2psswd.count(user)==0 ||usr2psswd[user] != passwd ){
                strcpy(m_resources_dir,rootDir);
                strcat(m_resources_dir,"/logError.html");
            }
            else{
                strcpy(m_resources_dir,rootDir);
                strcat(m_resources_dir,"/welcome.html");
            }
        }
        if(m_url[1] == '3'){  //注册
           
            if(http_conn::usr2psswd.count(user) != 0){  //已经被注册过了
                strcpy(m_resources_dir,rootDir);
                strcat(m_resources_dir,"/registerError.html");
            }
            else{
                
                m_sqlLock.lock();  //通过存储好的连接来存入服务器
                char command[100];
                bzero(command,100);
                sprintf(command,"INSERT INTO user (username,passwd) VALUES (\"%s\",\"%s\")",user.c_str(),passwd.c_str());

                if(mysql_query(http_conn::mysqlConn,command)){  //执行成功返回0
                    std::cout<<"Fail to insert data"<<" "<<mysql_error(http_conn::mysqlConn)<<std::endl;
                    strcpy(m_resources_dir,rootDir);
                    strcat(m_resources_dir,"/sqlError.html");
                }
                else{ //执行成功了 
                    http_conn::usr2psswd.insert({user,passwd});
                    strcpy(m_resources_dir,rootDir);
                    strcat(m_resources_dir,"/log.html");
                }

                m_sqlLock.unlock();

            }
        }
    }
    else if(m_url[1] == '5'){
        strcpy(m_resources_dir,rootDir);
        strcat(m_resources_dir,"/picture.html");
    }
    else if(m_url[1] == '6'){
        strcpy(m_resources_dir,rootDir);
        strcat(m_resources_dir,"/video.html");
    }
    else if(m_url[1] == '7'){
        strcpy(m_resources_dir,rootDir);
        strcat(m_resources_dir,"/fans.html");
    }
    else{  //除了页面以外的文件 
        strcpy(m_resources_dir,rootDir);
        strcat(m_resources_dir,m_url);
    }
    


    
    int ret = stat(m_resources_dir, &m_stat);


    if (ret == -1)
    {
        perror("stat");
        printf("lack file:%s\n",m_resources_dir);
        return NO_RESOURCE;
    }
    if (m_stat.st_mode & S_IFMT != S_IFREG || S_ISDIR(m_stat.st_mode))//不是一个可以发送的文件
    {
        return BAD_REQUEST;
    }
    if (m_stat.st_mode & S_IROTH == 0)
    {
        return FORBIDDEN_REQUEST;
    }

    int fd = open(m_resources_dir, O_RDONLY);
   
    m_resource_addr = (char *)mmap(NULL, m_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);  //把磁盘里面的文件映射到内存

    close(fd);
    return FILE_REQUEST; // 拿到了文件
}



bool http_conn::add_response(const char* format,...){  //format 里面有\n就可以了
    if(m_write_idx>=WRITE_BUFFER_SIZE)
        return false;
    

    //variatic variable list
    va_list arg;
    va_start(arg,format);  //只是为了找到第一个参数的位置
    int orgsize = WRITE_BUFFER_SIZE - m_write_idx -1;  //剩余的地方
    
    int len = vsnprintf(m_write_buf+m_write_idx,orgsize,format,arg);  //把可变参数原封不动地输入
    
    if(len >= WRITE_BUFFER_SIZE - m_write_idx -1){
        va_end(arg);
        return false;
    }

    m_write_idx += len;

    va_end(arg);

    return true;

}


 //根据解析的结果 生成响应
bool http_conn::process_write(HTTP_CODE code){
    //应对每一种解析返回值 作出相应的响应

    switch(code){ //根据不同的返回码生成不同的响应
        case INTERNAL_ERROR:
        {   
            //内部错误
            add_status_line(500,error_500_title);  //写上了回复头
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
                return false;
            //主状态机异常
            break;
        }
        
        case BAD_REQUEST:
        {
            add_status_line(404,error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
                return false;
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form))
                return false;
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line(404,error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_404_form))
                return false;
            break;
        }

        // 请求到了资源
        case FILE_REQUEST:
        {
            add_status_line(200,ok_200_title);
            add_headers(m_stat.st_size);
            //判断请求的资源是否存在
            if(m_stat.st_size>0){
                m_iov[0].iov_base = m_write_buf;  //首先发送文件头
                m_iov[0].iov_len  = m_write_idx;

                m_iov[1].iov_base = m_resource_addr;  //发送数据体
                m_iov[1].iov_len = m_stat.st_size;
                m_iv_count = 2;  //一共要发送两个缓冲区的数据

                bytes_to_send = m_iov[0].iov_len + m_iov[1].iov_len;
                return true;
            }
            else{
                //如果资源的大小为0  返回一个空文件就可以了
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                    return false;
            }
            break;
        }
        default:
            return false;
    }
    //除了FILEREQUEST  其余的情况只申请一个iov
    m_iov[0].iov_base = m_write_buf;
    m_iov[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}


//process_write完成响应报文的生成后，注册了epoolout事件。
//服务器检测到这个事件以后，调用http_conn::write函数发送报文给浏览器
//通过writev循环发送数据 并判断是否长连接 长连接重置读写缓冲区相关的参数，不长连接则注销http对象
bool http_conn::write()   //epoll 监听到EPOLLOUT 事件以后 就会调用这个函数
{
    //返回值决定是否保持连接
    ssize_t temp = 0;
    

    //如果发送的数据是0， 响应报文为空， 一般没有这种情况
    if(bytes_to_send == 0){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }
    // printf("with fd: %d ,send response of %s\n",m_sockfd,m_resources_dir);
    //循环地发送数据 直至发送完成为止

    while(1){

        // //
        // temp = writev(m_sockfd, m_iov, m_iv_count);   //这里是不完备的 没办法一次发送大文件
        // if(temp<=-1){
        //     //无法发送了
        //     if(errno == EAGAIN){
        //         //TCP没有缓存空间了，等待下一轮的EPOLLOUT事件

        //         //继续发送
                
        //         modfd(m_epollfd,m_sockfd,EPOLLOUT);
        //         return true;  //true 就是不要关闭连接
        //     }
        //     //那就是对面关闭了
        //     unmap();
        //     return false;
        // }

        // bytes_to_send -= temp;
        // bytes_have_send  += temp;
        //如果bytes_to_send大于0，会重新循环，但是文件还是从头开始发，这是个bug


        temp = writev(m_sockfd,m_iov,m_iv_count);
        if(temp == -1 ){   //缓存满了
            if(errno == EAGAIN){
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return true;   //继续监听
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if(bytes_to_send>0){//数据没发完
            if(bytes_have_send < m_iov[0].iov_len){  //报头数据都没发完
                m_iov[0].iov_base = (char*)m_iov[0].iov_base + bytes_have_send;
                m_iov[0].iov_len -= bytes_have_send;
            }
            else{  //响应报头已经发完了，数据没发完
                m_iov[0].iov_len = 0;
                m_iov[1].iov_base = (char*)m_iov[1].iov_base + (bytes_have_send - m_write_idx);  //m_iov[0].iov_len可能已经被修改过了
                m_iov[1].iov_len = bytes_to_send;
            }
        }
        else{ //bytes_to_send<=0
            //数据发送完了
            unmap();  //释放共享内存的资源
            modfd(m_epollfd,m_sockfd,EPOLLIN);  //默认继续监听这个文件描述符吧
            if(m_linger){
                init();   //保持连接 返回true   如果不长连接的话，那么说明对面就要关闭这个fd了
                return true;
            }
            else{
                return false;  //不保持连接了
            }

        }
    }
    return false;
}


bool http_conn::unmap(){
    int ret = munmap(m_resource_addr,m_stat.st_size);
    m_resource_addr = NULL;
    return ret == 0;
}