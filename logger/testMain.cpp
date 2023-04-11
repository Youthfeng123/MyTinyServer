#include "logger.h"
#include "blockedQueue.h"
#include<unistd.h>


int main(){
    Log::get_instance()->init("./logger.txt");
    int i = 1;
    while(true){
        sleep(1);
        LOG_DEBUG("This is the %dth message",i);
        i++;
    }

    return -1;
}