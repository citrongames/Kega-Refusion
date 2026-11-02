#include <libco.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
constexpr unsigned StackBytes=512*1024;
thread_local cothread_t caller;
thread_local unsigned visits;
void entry() {for(;;){++visits;co_switch(caller);}}
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void checkThread(){
 caller=co_active();
#ifdef _WIN32
 require((reinterpret_cast<std::uintptr_t>(caller)&15)==0,"Win64 primary TLS context is not 16-byte aligned (movaps)");
#endif
 for(unsigned round=0;round<4;++round){
  cothread_t peers[2]{co_create(StackBytes,entry),co_create(StackBytes,entry)};
  try {
   for(auto peer:peers){
    require(peer!=nullptr,"context allocation failed");
#ifdef _WIN32
    require((reinterpret_cast<std::uintptr_t>(peer)&15)==0,"Win64 peer context is not 16-byte aligned");
    const auto savedRsp=*static_cast<std::uintptr_t*>(peer);
    const auto end=reinterpret_cast<std::uintptr_t>(peer)+((StackBytes+512)&~15u);
    require(savedRsp+48<=end,"Win64 initial stack lacks 32-byte argument home space");
    require(((savedRsp+8)&15)==8,"Win64 entry stack alignment is invalid");
#endif
   }
   visits=0;
   for(unsigned i=0;i<1024;++i){co_switch(peers[0]);co_switch(peers[1]);}
   require(visits==2048,"context switches lost a visit");
   require(co_active()==caller,"caller context was not restored");
  }catch(...){for(auto peer:peers)if(peer)co_delete(peer);throw;}
  for(auto peer:peers)co_delete(peer);
 }
}
}
int main(){
#ifdef _WIN32
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#endif
 std::atomic<unsigned> failed{0};
 auto run=[&]{try{checkThread();}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());++failed;}};
 run();
 // MinGW emulated TLS allocates independently on each worker. Exercise fresh
 // workers as well as the main thread; no Qt, display, ROM or user settings.
 for(unsigned i=0;i<8;++i){std::thread worker(run);worker.join();}
 if(failed)return 1;
 std::puts("PASS: 9 thread lifetimes, aligned contexts, valid Win64 home space, 73728 switches, all peer stacks released");return 0;
}
