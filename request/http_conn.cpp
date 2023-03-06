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

int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;
const char *http_conn::rootDir = "./resources";
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
    event.events = EPOLLIN | EPOLLRDHUP ; //?????对方异常断开，就在底层处理了

    if (one_shot)
    {
        event.events = event.events|EPOLLONESHOT;
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

void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    // 设置sockfd的端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    // 添加到epoll对象
    addfd(m_epollfd, sockfd, true); // 让epoll监听这个文件描述符
    m_user_count++;                 // 数目自增

    init(); // 初始化一些其它的数据
}

// 关闭连接
void http_conn::close_conn()
{
    if (m_sockfd != -1)
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始化状态为解析请求首行
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
    

}

// 循环读取客户的数据，直到客户没有数据或者对方关闭连接
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {                 // 问题是什么时候重置m_readidx?
        return false; // 缓冲已经满了 ？ 等待下一次 一般是足够的
    }

    int bytes_read = 0;
    while (true)
    { // 读到已经读的后面
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
    // printf("读取到了数据：%s\n", m_read_buf);
    return true;
}



// 由线程池中的工作线程调用，这个是处理HTTP请求的入口函数
void http_conn::process()
{
    // 解析HTTP请求       //返回解析结果 对应前面的HTTP_CODE
    HTTP_CODE read_ret = process_read(); //  这个是有限状态机  顺序调用 会分别解析请求行 请求体 和 数据包
    if (read_ret == NO_REQUEST)
    { // 请求的数据不完整
        // 需要重新读取数据
        modfd(m_epollfd, m_sockfd, EPOLLIN); // 因为设置了EPOLLONESHOT 需要重新设置flag
    }

    // 生成响应
    bool write_ret = process_write(read_ret);
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
        text = get_line();
        m_start_line = m_checked_index; // 下一次解析的起点
        // printf("got 1 http line:%s\n", text);
        HTTP_CODE ret;
        switch (m_check_state)
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
                return do_request(); // 处理具体请求的信息
            }
            break;
        }

        case CHECK_STATE_CONTENT:
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

    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 解析http请求行，获得请求方法 请求URL 和HTTP版本
    //  用正则表达式也可以  内容： GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 返回第一个空格的地方
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0'; // 切断

    char *method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST; // 因为不支持其它方法
    }
    //  /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    //  /index.html\0HTTP/1.1
    *m_version++ = '\0';
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
    strcpy(m_resources_dir, rootDir);
    strcat(m_resources_dir, m_url);

    m_check_state = CHECK_STATE_HEADER; // 检查主状态机状态变成 检查请求头

    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 解析一行header
    // 主要解析几个东西： 是否保持连接？m_linger  content-length 和host

    if (!strlen(text) || text[0] == '\0')
    {
        // printf("数据头解析完毕！\n");
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
        // printf("Receive data:%s\n", text);
    }
    // else {
    //     printf( "oop! unknow header %s\n", text );
    // }

    return NO_REQUEST;
}

// 这里只判断我们读入的数据包 到底有没有完整地读取数据体
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_content_length == 0)
        return GET_REQUEST;
    // else{
    //     if(text + m_content_length == m_checked_index)
    //         return GET_REQUEST;
    //     else
    //         return BAD_REQUEST;
    // }
    if (m_read_idx >= (m_content_length + m_checked_index))
    {
        text[m_content_length] = '\0';
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
        if (temp == '\r')
        {
            if ((m_checked_index + 1) == m_read_idx)
            {                     // 如果下一个字符就结束了
                return LINE_OPEN; // 这个不完整  因为下一个应该是\n?
            }
            else if (m_read_buf[m_checked_index + 1] == '\n')
            { // 有的系统只有\n结尾
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK; // 这是完整的一行了
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if ((m_checked_index > 1) && (m_read_buf[m_checked_index - 1] == '\r'))
            {
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN; // 没读到\r\n
}

http_conn::HTTP_CODE http_conn::do_request()
{ // 获取某一个文件    这个也是个有限状态机 可能是解析头部 也可能是解析数据体

    // printf("处理了请求,资源符是：%s\n", m_resources_dir);
    int DirLength = strlen(m_resources_dir);
    if (m_resources_dir[DirLength - 1] == '/')
        strcat(m_resources_dir, "index.html");
        
    int ret = stat(m_resources_dir, &m_stat);
    if (ret == -1)
    {
        perror("stat");
        printf("lack file:%s\n",m_resources_dir);
        return NO_RESOURCE;
    }
    if (m_stat.st_mode & S_IFMT != S_IFREG || S_ISDIR(m_stat.st_mode))//not regular file 
    {
        return BAD_REQUEST;
    }
    if (m_stat.st_mode & S_IROTH == 0)
    {
        return FORBIDDEN_REQUEST;
    }

    int fd = open(m_resources_dir, O_RDONLY);
   
    m_resource_addr = (char *)mmap(NULL, m_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    close(fd);
    return FILE_REQUEST; // 拿到了文件
}

bool http_conn::add_response(const char* format,...){  //format 里面有\n就可以了
    if(m_write_idx>=WRITE_BUFFER_SIZE)
        return false;
    

    //variatic variable list
    va_list arg;
    va_start(arg,format);  //只是为了找到第一个参数的位置
    int orgsize = WRITE_BUFFER_SIZE - m_write_idx -1;
    
    int len = vsnprintf(m_write_buf+m_write_idx,orgsize,format,arg);
    
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

    switch(code){
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

                m_iov[1].iov_base = m_resource_addr;
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
    printf("send response of %s\n",m_resources_dir);
    if(strcasecmp(m_resources_dir,"./resources/favicon.ico")==0){
        printf("%s",m_write_buf);
    }
    //循环地发送数据 直至发送完成为止
    while(1){
        // if(bytes_have_send<m_iov[0].iov_len){
        //     temp = send(m_sockfd,m_iov[0].iov_base,m_iov[0].iov_len,0);
        // }
        // else{
        //     temp = send(m_sockfd,m_iov[1].iov_base,m_iov[1].iov_len,0);
        // }
        // temp = writev(m_sockfd,m_iov,m_iv_count);
        temp = writev(m_sockfd, m_iov, m_iv_count);   //这里是不完备的
        if(temp<=-1){
            //无法发送了
            if(errno == EAGAIN){
                //TCP没有缓存空间了，等待下一轮的EPOLLOUT事件

                //继续发送
                
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send  += temp;
        if(bytes_to_send<=0){
            //数据发送完了
            unmap();  //释放共享内存的资源
            modfd(m_epollfd,m_sockfd,EPOLLIN);  //因为是ONESHOT
            if(m_linger){
                init();   //保持连接 返回true
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