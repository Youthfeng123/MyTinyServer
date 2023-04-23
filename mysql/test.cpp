#include<mysql/mysql.h>
#include<iostream>
#include<string>
#include<vector>
#include "sqlconnpool.h"
using namespace std;

int main(){
    MYSQL_RES * result;
    MYSQL_ROW row;
    connection_pool* connpool = nullptr;
    connpool = connection_pool::GetInstance();
    connpool->init("localhost","webserver","123321","crashcourse",0,5);

    MYSQL* conn = connpool->GetConnection();

    if(mysql_query(conn,"select * from  customers")){
        cout<<"error"<<endl;
        return -1;
    }
    result = mysql_use_result(conn);

    while((row = mysql_fetch_row(result))){
        cout<<row[0]<<row[1]<<endl;
    }

    connpool->ReleaseConnection(conn);
    cout<<connpool->GetFreeConn()<<" left"<<endl;

    return 0;
}