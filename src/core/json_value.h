#pragma once
#include <map>
#include <string>
#include <vector>
namespace dl {
// Bounded JSON for workspace/package metadata. Integers retain all 64 bits.
struct Json {
    enum Kind {Null,String,Integer,Boolean,Array,Object} kind=Null;
    std::string text; long long integer=0; bool boolean=false;
    std::vector<Json> array;std::map<std::string,Json> object;
    Json()=default;Json(const std::string& s):kind(String),text(s){};
    Json(const char* s):Json(std::string(s)){};
    Json(long long n):kind(Integer),integer(n){};
    static Json list(){Json j;j.kind=Array;return j;}
    static Json dict(){Json j;j.kind=Object;return j;}
    static Json flag(bool b){Json j;j.kind=Boolean;j.boolean=b;return j;}
    const Json& at(const std::string& key)const;
    std::string str()const;long long num()const;bool flag()const;
};
Json parseJson(const std::string& bytes);
std::string writeJson(const Json& value);
}
