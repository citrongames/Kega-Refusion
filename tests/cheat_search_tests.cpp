#include "core/CheatSearch.h"

#include <QByteArray>
#include <QString>

#include <cstdio>
#include <initializer_list>
#include <stdexcept>

namespace {
void require(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}
QByteArray bytes(std::initializer_list<unsigned> values) {
    QByteArray result;
    result.reserve(qsizetype(values.size()));
    for (unsigned value : values) result.append(char(value));
    return result;
}
void match(const Cheats::Search& search, int index, quint32 address,
           quint32 previous, quint32 value) {
    const auto list = search.matches(index+1);
    require(list.size() > index && list[index].address == address &&
            list[index].previous == previous && list[index].value == value,
            "search match address or previous/current value");
}

void exactAndLimits() {
    Cheats::Search search; QString error;
    require(search.filter(bytes({5,2,5,5,9}),0x1000,false,1,
                          Cheats::Search::Exact,5,true,error) && error.isEmpty(),
            "start exact byte search");
    require(search.started() && search.count()==3 && search.matches(0).isEmpty() &&
            search.matches(2).size()==2,"initial exact count or result limit");
    match(search,0,0x1000,5,5);
    match(search,1,0x1002,5,5);
    match(search,2,0x1003,5,5);
    require(search.filter(bytes({6,2,6,7,9}),0x1000,false,1,
                          Cheats::Search::Exact,6,false,error) && error.isEmpty(),
            "manual exact refinement");
    require(search.count()==2,"manual exact search did not narrow 3 to 2");
    match(search,0,0x1000,5,6);
    match(search,1,0x1002,5,6);
}

void relativeComparisons() {
    Cheats::Search search; QString error;
    require(search.filter(bytes({4,8,12,16}),0x8000,false,1,
                          Cheats::Search::Unknown,0,true,error) && search.count()==4,
            "unknown initial scan did not retain every candidate");
    require(search.filter(bytes({5,8,11,16}),0x8000,false,1,
                          Cheats::Search::Changed,0,false,error) && search.count()==2,
            "changed scan");
    match(search,0,0x8000,4,5);
    match(search,1,0x8002,12,11);
    require(search.filter(bytes({5,9,10,16}),0x8000,false,1,
                          Cheats::Search::Unchanged,0,false,error) && search.count()==1,
            "unchanged scan did not compare against latest snapshot");
    match(search,0,0x8000,5,5);
    require(search.filter(bytes({6,9,10,16}),0x8000,false,1,
                          Cheats::Search::Increased,0,false,error) && search.count()==1,
            "increased scan");
    match(search,0,0x8000,5,6);
    require(search.filter(bytes({3,9,10,16}),0x8000,false,1,
                          Cheats::Search::Decreased,0,false,error) && search.count()==1,
            "decreased scan");
    match(search,0,0x8000,6,3);
    require(search.filter(bytes({3,9,10,16}),0x8000,false,1,
                          Cheats::Search::Unknown,0,false,error) && search.count()==1,
            "unknown continuation changed candidate set");
}

void widthsAndAlignment() {
    Cheats::Search search; QString error;
    require(search.filter(bytes({0x12,0x34,0xaa,0xbb,0x12,0x34,0x99}),
                          0x1000,true,2,Cheats::Search::Exact,0x1234,true,error) &&
            search.count()==2,"big-endian 16-bit even-address scan");
    match(search,0,0x1000,0x1234,0x1234);
    match(search,1,0x1004,0x1234,0x1234);
    require(search.filter(bytes({0x12,0x34,0xaa,0xbb,0x12,0x34,0x99}),
                          0x1000,false,2,Cheats::Search::Exact,0x3412,true,error) &&
            search.count()==2,"little-endian 16-bit unaligned scan");
    match(search,1,0x1004,0x3412,0x3412);
    require(search.filter(bytes({0xff,0x12,0x34}),0x1001,true,2,
                          Cheats::Search::Exact,0x1234,true,error) &&
            search.count()==1,"big-endian word did not align the CPU address");
    match(search,0,0x1002,0x1234,0x1234);
    require(search.filter(bytes({1,2,3,4,5,6}),0x2000,true,4,
                          Cheats::Search::Exact,0x03040506,true,error) &&
            search.count()==1,"big-endian 32-bit stride or last boundary");
    match(search,0,0x2002,0x03040506,0x03040506);
    require(search.filter(bytes({1,2,3,4,5}),0x3000,false,4,
                          Cheats::Search::Exact,0x05040302,true,error) &&
            search.count()==1,"little-endian 32-bit unaligned last boundary");
    match(search,0,0x3001,0x05040302,0x05040302);
    require(search.filter(bytes({0x78,0x56}),0xfffffffeu,false,2,
                          Cheats::Search::Exact,0x5678,true,error) &&
            search.count()==1,"last two CPU address bytes");
    match(search,0,0xfffffffeu,0x5678,0x5678);
}

void invalidRequestsAndClear() {
    Cheats::Search search; QString error;
    require(!search.filter(bytes({1,2}),0,false,1,
                           Cheats::Search::Changed,0,false,error) && !error.isEmpty() &&
            !search.started(),"relative search without baseline");
    require(!search.filter(bytes({1,2}),0,false,1,
                           Cheats::Search::Changed,0,true,error) && !error.isEmpty() &&
            !search.started(),"relative restart without prior snapshot");
    require(search.filter(bytes({1,2,1}),0x4000,false,1,
                          Cheats::Search::Exact,1,true,error) && search.count()==2,
            "validation test baseline");
    const auto unchanged=[&] {
        require(search.started() && search.count()==2,"invalid search changed candidate state");
        match(search,0,0x4000,1,1);
        match(search,1,0x4002,1,1);
    };
    const auto reject=[&](const QByteArray& memory,quint32 base,bool bigEndian,int width,
                          Cheats::Search::Comparison comparison,bool restart) {
        error.clear();
        require(!search.filter(memory,base,bigEndian,width,comparison,0,restart,error) &&
                !error.isEmpty(),"invalid search request was accepted");
        unchanged();
    };
    reject(bytes({1,2,1}),0x4000,false,3,Cheats::Search::Exact,false);
    reject(bytes({1,2,1}),0x4000,false,0,Cheats::Search::Exact,false);
    reject(bytes({1,2,1}),0x4001,false,1,Cheats::Search::Exact,false);
    reject(bytes({1,2,1}),0x4000,true,1,Cheats::Search::Exact,false);
    reject(bytes({1,2,1}),0x4000,false,2,Cheats::Search::Exact,false);
    reject(bytes({1,2}),0x4000,false,1,Cheats::Search::Exact,false);
    reject(bytes({1,2,1}),0x4000,false,1,
           static_cast<Cheats::Search::Comparison>(99),false);
    reject(QByteArray(1024*1024+1,char(0)),0x4000,false,1,
           Cheats::Search::Exact,true);
    reject(bytes({1,2,1}),0xfffffffeu,false,1,Cheats::Search::Exact,true);
    reject(bytes({1,2,1}),0x4000,false,1,Cheats::Search::Increased,true);
    error.clear();
    require(!search.filter(bytes({1,2,1}),0x4000,false,1,
                           Cheats::Search::Exact,0x101,false,error) && !error.isEmpty(),
            "overflowing byte search value was accepted");
    unchanged();
    error.clear();
    require(!search.filter(bytes({1,2,1}),0x4000,false,2,
                           Cheats::Search::Exact,0x10000,true,error) && !error.isEmpty(),
            "overflowing word search value was accepted");
    unchanged();
    search.clear();
    require(!search.started() && search.count()==0 && search.matches().isEmpty(),
            "clear did not discard search state");
}
} // namespace

int main() {
    try {
        exactAndLimits();
        relativeComparisons();
        widthsAndAlignment();
        invalidRequestsAndClear();
        std::puts("PASS: bounded manual cheat search, comparisons, endian widths, validation and clear");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr,"FAIL: %s\n",failure.what());
        return 1;
    }
}
