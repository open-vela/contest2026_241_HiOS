#pragma once
#include "desktop_folders.h"
#include <cerrno>
#include <deque>
#include <mutex>
#include <semaphore.h>

namespace DesktopStorage {
struct Request {
    bool layout=true,refresh=true,keep_folder=false,saved=false;
    DesktopFolders::Model model;
    std::string order;
};

// UI produces requests and owns completion/deletion. One persistent worker
// consumes them. No queue lock may be held while acquiring the LVGL lock.
class Queue {
    std::mutex lock_;
    sem_t ready_;
    bool initialized_;
    std::deque<Request*> pending_,completed_;
    Request* home_pending_=nullptr;
public:
    Queue():initialized_(sem_init(&ready_,0,0)==0){}
    ~Queue(){if(initialized_)sem_destroy(&ready_);}
    bool Ready()const{return initialized_;}
    bool Push(Request* request){
        std::lock_guard<std::mutex> guard(lock_);
        if(!initialized_)return false;
        if(!request->layout&&home_pending_){
            home_pending_->order.swap(request->order);delete request;return true;
        }
        pending_.push_back(request);
        if(sem_post(&ready_)<0){pending_.pop_back();return false;}
        if(!request->layout)home_pending_=request;
        return true;
    }
    Request* Wait(){
        int rc;do{rc=sem_wait(&ready_);}while(rc<0&&errno==EINTR);
        if(rc<0)return nullptr;
        std::lock_guard<std::mutex> guard(lock_);
        if(pending_.empty())return nullptr;
        auto* request=pending_.front();pending_.pop_front();
        if(home_pending_==request)home_pending_=nullptr;
        return request;
    }
    void Complete(Request* request){
        std::lock_guard<std::mutex> guard(lock_);completed_.push_back(request);
    }
    Request* Poll(){
        std::lock_guard<std::mutex> guard(lock_);
        if(completed_.empty())return nullptr;
        auto* request=completed_.front();completed_.pop_front();return request;
    }
};
}
