"""Run actual pool lookup/range guards with a recording importer; no Vulkan, SDK or game data.

python nx2/test_import_windows.py [--cxx clang++]
"""
import argparse,os
from pathlib import Path
import subprocess,tempfile

ROOT=Path(__file__).resolve().parents[1]
def block(text,marker):
    start=text.index(marker);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cxx',default='clang++');a=p.parse_args()
    source=(ROOT/'gpu/vk/vk_memory.cpp').read_text(encoding='utf-8')
    guard=source[source.index('bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {'):source.index('    p.lo = lo;',source.index('bool import_pool('))]
    lookup=block(source,'Pool* pool_lookup(uint64_t guest, uint64_t size) {')
    cache=block(source,'Pool* pool_of(uint64_t guest, uint64_t size) {')
    fixture='''#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL:%d %s\\n",__LINE__,#x);std::exit(1);} }while(0)
struct Pool { uint64_t lo=0,hi=0,range_lo=0,range_hi=0,address=0; };
struct GuestPool { uint64_t lo,hi; };
std::vector<GuestPool> guest_pools;
std::map<uint64_t,Pool> imported;
Pool *last_pool=nullptr,*pool_cache[4096]{};
uint64_t max_import_size=128ull<<20;
int failures=0,st_bg_waits=0,imports=0,retires=0;
std::mutex bg_mu;std::condition_variable bg_done_cv;std::vector<uint64_t> bg_inflight;
void logf(const char*,...){} void bg_drain(){}
void guest_pages_set(uint64_t,uint64_t,uint64_t){}
void retire_pool(std::map<uint64_t,Pool>::iterator it) { imported.erase(it);last_pool=nullptr;std::fill(std::begin(pool_cache),std::end(pool_cache),nullptr);retires++; }
'''+guard+'''
    p.lo=lo;p.hi=hi;imports++;return true;
}
'''+lookup+'\n'+cache+'''
int main() {
    const uint64_t base=0x1000000000ull,extent=0x99200000ull;
    guest_pools.push_back({base,base+extent});
    const auto chunk=uint64_t(std::atoi(std::getenv("RX_VK_IMPORT_CHUNK_MB")))<<20;
    if(!chunk) {
        CHECK(!pool_lookup(base+1234,64) && imports==0);
        max_import_size=4ull<<30;
        auto p=pool_lookup(base+1234,64);CHECK(p && p->lo==base && p->hi==base+extent);
    } else {
        const uint64_t address=base+1234,expected=address/chunk*chunk;
        auto p=pool_lookup(address,64);CHECK(p && p->lo==expected && p->hi==expected+chunk && imports==1);
        CHECK(pool_lookup(address+100,64)==p && imports==1);
        p=pool_lookup(expected+4*chunk+123,64);CHECK(p && p->lo==expected+4*chunk && p->hi==expected+5*chunk && imports==2);
        p=pool_lookup(expected+chunk-16,64);CHECK(p && p->lo==expected && p->hi==expected+2*chunk && retires==1);
    }
    const int before=imports;
    CHECK(!pool_lookup(base+extent+1,1));
    CHECK(!pool_lookup(base+extent-1,2));
    CHECK(!pool_lookup(base,UINT64_MAX));
    CHECK(!pool_lookup(base,0));
    CHECK(!pool_of(base,UINT64_MAX) && !pool_of(base,0) && imports==before);
    guest_pools.push_back({UINT64_MAX-1024,UINT64_MAX});
    if(chunk) CHECK(!pool_lookup(UINT64_MAX-16,8) && imports==before);
    Pool rejected;CHECK(!import_pool(base,base,rejected) && !import_pool(base+1,base,rejected));
    CHECK(!import_pool(base,base+max_import_size+1,rejected));
    std::printf("PASS: request windows, reuse, overlap retirement, range/overflow guards, import limits (chunk %llu)\\n",(unsigned long long)chunk);
}
'''
    work=ROOT/'work';work.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='import-windows-',dir=work) as temp:
        directory=Path(temp).resolve();directory.relative_to(work.resolve());cpp=directory/'fixture.cpp';exe=directory/'fixture.exe';cpp.write_text(fixture,encoding='utf-8')
        subprocess.run([a.cxx,'-std=c++20','-O2','-D_CRT_SECURE_NO_WARNINGS',str(cpp),'-o',str(exe)],check=True)
        for chunk in ('16','3','0'):
            subprocess.run([str(exe)],env=dict(os.environ,RX_VK_IMPORT_CHUNK_MB=chunk),check=True)
if __name__=='__main__':main()
