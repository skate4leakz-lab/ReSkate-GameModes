#include "one_up_core_assets.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <span>
#include <stdexcept>

namespace dingosdk::mods {
namespace {
template<class T> T take(std::span<const std::byte>& data) {
    if(data.size()<sizeof(T))throw std::runtime_error("ReSkate core asset bundle is truncated");
    T value;std::memcpy(&value,data.data(),sizeof(T));data=data.subspan(sizeof(T));return value;
}
bool equal_file(const std::filesystem::path& path,std::span<const std::byte> expected) {
    std::error_code error;
    if(std::filesystem::file_size(path,error)!=expected.size() || error)return false;
    std::ifstream in(path,std::ios::binary);
    std::array<char,65536> block{};
    for(std::size_t at=0;at<expected.size();at+=block.size()) {
        const auto n=std::min(block.size(),expected.size()-at);
        if(!in.read(block.data(),n) || std::memcmp(block.data(),expected.data()+at,n))return false;
    }
    return true;
}
}
std::optional<Mod> one_up_core_assets(const std::filesystem::path& data_root) {
    // Unit-test roots and tools without a game installation have no native layout.
    // The catalogue separately validates its changelist before merging the bundle.
    if(!std::filesystem::is_regular_file(data_root/L"Data"/L"layout.toc"))return {};
    static std::mutex mutex;
    static std::filesystem::path verified_root;
    static std::optional<Mod> verified;
    std::lock_guard lock(mutex);
    if(verified && verified_root==data_root)return verified;
    HMODULE module=GetModuleHandleW(L"ReSkate.dll");
    bool borrowed=module!=nullptr;
    if(!module) {
        std::array<wchar_t,32768> executable{};
        auto n=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        if(!n || n>=executable.size())return {};
        const auto dll=std::filesystem::path(std::wstring(executable.data(),n)).parent_path()/L"ReSkate.dll";
        module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    }
    if(!module)return {};
    struct Release {HMODULE module;bool borrowed;~Release(){if(!borrowed)FreeLibrary(module);}} release{module,borrowed};
    const auto resource=FindResourceW(module,L"ONE_UP_NATIVE_ASSETS",RT_RCDATA);
    if(!resource)return {};
    const auto loaded=LoadResource(module,resource);
    const auto pointer=loaded?LockResource(loaded):nullptr;
    if(!pointer)throw std::runtime_error("ReSkate core assets could not be loaded");
    auto bytes=std::span(static_cast<const std::byte*>(pointer),SizeofResource(module,resource));
    if(bytes.size()<12 || std::memcmp(bytes.data(),"RS1UP001",8))throw std::runtime_error("ReSkate core asset format differs");
    bytes=bytes.subspan(8);const auto count=take<std::uint32_t>(bytes);
    if(!count || count>64)throw std::runtime_error("ReSkate core asset count differs");
    // Dot folders are private SDK caches and never appear in the mod manager.
    const auto directory=data_root/L"Mods"/L".reskate-core"/L"one-up-native-v1";
    for(unsigned i=0;i<count;++i) {
        const auto length=take<std::uint32_t>(bytes);const auto size=take<std::uint64_t>(bytes);
        if(!length || length>512 || length>bytes.size())throw std::runtime_error("Invalid core asset path");
        const std::string name(reinterpret_cast<const char*>(bytes.data()),length);bytes=bytes.subspan(length);
        const auto relative=std::filesystem::path(name);
        if(relative.is_absolute() || relative.has_root_name() || name.find(':')!=std::string::npos || name.find('\\')!=std::string::npos)
            throw std::runtime_error("Invalid core asset destination");
        for(const auto& part:relative)if(part==L".." || part==L".")throw std::runtime_error("Invalid core asset traversal");
        if(size>bytes.size())throw std::runtime_error("Truncated core asset payload");
        const auto payload=bytes.first(static_cast<std::size_t>(size));bytes=bytes.subspan(payload.size());
        const auto target=directory/relative;
        if(equal_file(target,payload))continue;
        std::filesystem::create_directories(target.parent_path());
        const auto staging=target.wstring()+L".pending";
        {std::ofstream out(staging,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(payload.data()),payload.size());
         if(!out)throw std::runtime_error("Could not write ReSkate core assets");}
        if(!MoveFileExW(staging.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not publish ReSkate core assets");
    }
    if(!bytes.empty())throw std::runtime_error("Unexpected ReSkate core asset data");
    Mod core;core.name="ReSkate Core 1-Up";core.directory=directory;core.provides_layout=true;
    core.title="1-Up native assets";core.author="ReSkate";core.version="1";
    verified_root=data_root;verified=std::move(core);return verified;
}
}
