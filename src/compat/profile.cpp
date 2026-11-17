#include "compat/symbols.hpp"
#include "hook/native.hpp"
#include <hde64.h>
#include <array>
#include <cstring>
#include <mutex>

namespace tabtail::compat {
namespace {
constexpr SymbolSpec specs[]{
    {"frame_vtable","??_7CExplorerFrame@@6BCImpWndProc@@@",false},
    {"host_vtable","??_7CBrowserHost@@6BDWebBrowserEvents2@@@",false},
    {"host_interface","??_7CBrowserHost@@6BIBrowserHost@@@",false},
    {"host_window_interface","??_7CBrowserHost@@6BIOleWindow@@@",false},
    {"frame_ctor","??0CExplorerFrame@@IEAA@PEAUIETHREADPARAM@@@Z",true},
    {"host_ctor","??0CBrowserHost@@IEAA@PEAVCExplorerFrame@@AEBU_GUID@@@Z",true},
    {"tab_lookup","?GetBrowserTabFromId@CExplorerFrame@@AEBAJU_GUID@@AEAUFEBrowserTab@@@Z",true},
    {"host_window","?GetWindow@CBrowserHost@@UEAAJPEAPEAUHWND__@@@Z",true},
    {"location","?GetLocationItem@CBrowserHost@@UEAAJAEBU_GUID@@PEAPEAX@Z",true},
    {"title","?SetFrameTitle@CExplorerFrame@@QEAAXPEBGU_GUID@@@Z",true},
    {"actions_vtable","??_7CExplorerFrame@@6BIFileExplorerTabActions2@@@",false},
    {"select","?SelectTab@CExplorerFrame@@UEAAJU_GUID@@@Z",true},
    {"remove","?RemoveTab@CExplorerFrame@@UEAAJU_GUID@@@Z",true},
    {"close","?OnClose@CBrowserHost@@AEAAJ_N@Z",true},
    {"add","?AddTab@CExplorerFrame@@UEAAJPEBU_ITEMIDLIST_ABSOLUTE@@W4NewTabEntryPoint@@PEAU_GUID@@@Z",true},
    {"add_index","?AddTabAtIndex@CExplorerFrame@@UEAAJPEBU_ITEMIDLIST_ABSOLUTE@@W4NewTabEntryPoint@@HPEAU_GUID@@@Z",true},
    {"get_browser","?GetBrowser@CBrowserHost@@UEAAJAEBU_GUID@@PEAPEAX@Z",true},
    {"browser_ctor","??0CShellBrowser@@IEAA@I@Z",true},
    {"browser_vtable","??_7CShellBrowser@@6BCFSNotifyHelper@@@",false},
    {"browser_interface","??_7CShellBrowser@@6BIShellBrowser@@@",false},
    {"navigate","?_NavigateToPidl@CShellBrowser@@AEAAJPEBU_ITEMIDLIST_ABSOLUTE@@KK@Z",true},
    {"move_selected","?MoveSelectedTabToPosition@CExplorerFrame@@QEAAJI@Z",true},
    {"reorder_iid","_GUID_bc05f781_2a99_4dfd_9a30_5ca3c8ecd904",false},
    {"reorder_service","_GUID_c54b9fde_73ce_4a21_88df_6d3d4a1a736b",false}
};
struct Instruction {uint32_t rva;hde64s decoded;std::vector<unsigned char> bytes;};
std::vector<Instruction> instructions(const Image& image,uint32_t rva,size_t limit,bool stop_call=false){
    std::vector<Instruction> out;
    for(size_t off=0;off<limit;){
        // HDE may read up to 15 bytes. Do not give it a truncated file buffer.
        auto code=image.at(rva+static_cast<uint32_t>(off),16);hde64s h{};hde64_disasm(code.data(),&h);
        if(!h.len||(h.flags&F_ERROR))throw Error(3,"Unrecognized x64 instruction in layout contract");
        if(stop_call&&(h.opcode==0xc3||h.opcode==0xc2||h.opcode==0xe9||h.opcode==0xeb||h.opcode==0xe8||(h.opcode==0xff&&h.modrm_reg==2)))break;
        if(stop_call&&(h.opcode>=0x70&&h.opcode<=0x7f||h.opcode==0x0f&&h.opcode2>=0x80&&h.opcode2<=0x8f))break;
        out.push_back({rva+static_cast<uint32_t>(off),h,{code.begin(),code.begin()+h.len}});off+=h.len;
    }return out;
}
bool equal(std::span<const unsigned char> bytes,std::initializer_list<unsigned char> expected){return bytes.size()==expected.size()&&std::equal(bytes.begin(),bytes.end(),expected.begin());}
void vtable_initialized(const Image& image,uint32_t ctor,uint32_t vtable,uint32_t offset){
    auto code=instructions(image,ctor,512,true);unsigned matches=0;
    for(size_t i=0;i+1<code.size();++i){
        auto& lea=code[i];auto& store=code[i+1];auto& h=lea.decoded;
        if(h.len!=7||!equal(std::span(lea.bytes).first(3),{0x48,0x8d,0x05}))continue;
        auto target=int64_t(lea.rva)+h.len+int32_t(h.disp.disp32);
        if(target!=vtable)continue;
        auto& b=store.bytes;uint32_t displacement=0;
        if(equal(b,{0x48,0x89,0x01}))displacement=0;
        else if(b.size()==4&&b[0]==0x48&&b[1]==0x89&&b[2]==0x41)displacement=b[3];
        else if(b.size()==7&&b[0]==0x48&&b[1]==0x89&&b[2]==0x81)std::memcpy(&displacement,b.data()+3,4);
        else continue;
        if(displacement==offset)++matches;
    }
    if(matches!=1)throw Error(3,"Constructor vtable placement changed or ambiguous");
}
Profile cached(const Image& image,const fs::path& cache){
    auto path=manifest_path(cache,image);
    if(!fs::exists(path)){Profile p;p.identity=image.identity();p.reason="No verified symbol profile; run symbols prepare";return p;}
    if(fs::file_size(path)>65536)throw Error(3,"Symbol manifest exceeds size limit");
    return load_manifest(image,Json::parse(read_file(path)));
}
std::mutex profile_mutex;
std::shared_ptr<const Profile> selected;
uint64_t retry_after=0;
}
std::span<const SymbolSpec> symbol_specs(){return specs;}
Profile validate_profile(const Image& image,const Rvas& rvas){
    Profile p;p.identity=image.identity();p.rvas=rvas;p.source="symbols + independent x64 contracts v2";
    try{
        for(auto& spec:std::span(specs).first(9)){
            if(!rvas.contains(spec.id))throw Error(3,std::string("Missing symbol: ")+spec.id);
            auto rva=rvas.at(spec.id);image.at(rva,spec.code?16:8);
            if(spec.code?!image.executable(rva):!image.readonly_data(rva))throw Error(3,std::string("Symbol section mismatch: ")+spec.id);
        }
        vtable_initialized(image,p.rva("frame_ctor"),p.rva("frame_vtable"),0);
        vtable_initialized(image,p.rva("host_ctor"),p.rva("host_vtable"),0);
        vtable_initialized(image,p.rva("host_ctor"),p.rva("host_interface"),0x10);
        vtable_initialized(image,p.rva("host_ctor"),p.rva("host_window_interface"),0x70);
        if(!equal(image.at(p.rva("host_window"),11),{0x48,0x8b,0x41,0x18,0x48,0x89,0x02,0x33,0xc0,0xc3,0xcc}) &&
           !equal(image.at(p.rva("host_window"),10),{0x48,0x8b,0x41,0x18,0x48,0x89,0x02,0x33,0xc0,0xc3}))throw Error(3,"Host window accessor layout changed");
        unsigned begin=0,end=0,low=0,high=0,host=0,stride=0;
        for(auto& ins:instructions(image,p.rva("tab_lookup"),image.function_size(p.rva("tab_lookup")))){
            auto& b=ins.bytes;
            begin+=equal(b,{0x48,0x8b,0x99,0xf0,0x02,0,0});end+=equal(b,{0x4c,0x8b,0x81,0xf8,0x02,0,0});
            low+=equal(b,{0x48,0x2b,0x43,0x10});high+=equal(b,{0x48,0x2b,0x43,0x18});host+=equal(b,{0x48,0x8b,0x33});stride+=equal(b,{0x48,0x83,0xc3,0x28});
        }
        if(begin!=1||end!=1||low!=1||high!=1||host!=1||stride!=1)throw Error(3,"Tab table layout contract changed (bounds / GUID / stride)");
        if(!equal(image.at(p.rva("location"),11),{0x48,0x83,0xc1,0x28,0x48,0x8b,0x01,0x48,0x8b,0x40,0x58}))throw Error(3,"Location thunk ABI changed");
        auto tail=image.at(p.rva("location")+11,5);int32_t relative{};std::memcpy(&relative,tail.data()+1,4);
        auto destination=int64_t(p.rva("location"))+16+relative;
        if(tail[0]!=0xe9||destination<0||destination>UINT32_MAX||!image.executable(static_cast<uint32_t>(destination)))throw Error(3,"Location thunk dispatch changed");
        unsigned location_slots=0;
        for(size_t i=0;i<32;++i){uint64_t slot{};auto data=image.at(p.rva("host_interface")+static_cast<uint32_t>(i*8),8);std::memcpy(&slot,data.data(),8);location_slots+=slot==image.preferred_base+p.rva("location");}
        if(location_slots!=1)throw Error(3,"Location vtable membership changed or ambiguous");
        p.inventory=true;
        if(!rvas.contains("title"))throw Error(3,"Missing title symbol (inventory remains available)");
        if(!image.executable(p.rva("title")))throw Error(3,"Title symbol section mismatch (inventory remains available)");
        if(!equal(image.at(p.rva("title"),16),{0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}))throw Error(3,"Title function ABI/prologue contract changed (inventory remains available)");
        p.title=true;p.reason="Symbol identity and layout contract accepted; target module and objects still require runtime validation";
    }catch(const std::exception& ex){p.reason=ex.what();}
    if(p.inventory)validate_features(image,p);
    return p;
}
Json make_manifest(const Image& image,const Rvas& rvas,std::string_view pdb_hash){
    Json j{{"schema",cache_schema},{"contract_revision",2},{"identity",image.identity()},{"rvas",rvas},{"pdb_sha256",pdb_hash},{"resolver","DbgHelp exact symbols; independent layout contracts v2"}};
    j["checksum"]=sha256(j.dump());return j;
}
Profile load_manifest(const Image& image,const Json& manifest,Module module){
    auto j=manifest;
    if(j.at("schema")!=cache_schema||j.at("identity")!=image.identity())throw Error(3,"Stale symbol cache: schema or image/PDB identity mismatch");
    auto checksum=j.at("checksum").get<std::string>();j.erase("checksum");
    if(checksum!=sha256(j.dump()))throw Error(3,"Corrupt symbol cache checksum");
    auto entries=j.at("rvas");if(!entries.is_object())throw Error(3,"Invalid symbol RVA map");
    for(auto it=entries.begin();it!=entries.end();++it){
        auto allowed=symbol_specs(module);auto known=std::any_of(allowed.begin(),allowed.end(),[&](auto& spec){return it.key()==spec.id;});
        if(!known||!it.value().is_number_integer()||it.value()<1||it.value()>UINT32_MAX)throw Error(3,"Unknown symbol or invalid RVA integer");
    }
    auto rvas=entries.get<Rvas>();
    if(rvas.size()>symbol_specs(module).size())throw Error(3,"Unexpected symbol entries");
    return module==Module::frame?validate_profile(image,rvas):validate_xaml_profile(image,rvas,module);
}
std::shared_ptr<const Profile> current_profile(){
    std::lock_guard guard(profile_mutex);
    if(selected&&((selected->title&&selected->selection&&selected->close_guard&&selected->opening&&selected->navigation_guard&&selected->reorder)||GetTickCount64()<retry_after))return selected;
    Profile p;
    try{
        Image image(read_file(frame_path()));
        if(image.sha256==supported_frame_sha256){p.identity=image.identity();p.inventory=p.title=p.baseline=true;p.source="built-in exact SHA256";p.reason="Known image; target module and objects still require runtime validation";
            p.rvas={{"frame_vtable",frame_vtable_rva},{"host_vtable",host_vtable_rva},{"host_interface",host_interface_vtable_rva},{"title",set_frame_title_rva},{"location",location_item_rva}};
            p.selection=p.close_guard=p.opening=p.navigation_guard=p.reorder=true;
            p.rvas.insert({{"actions_vtable",0x24ce90},{"select",0x132970},{"remove",0x1a6be0},{"close",0x125da4},{"add",0x19e0c0},{"add_index",0x19e0f0},
                {"get_browser",0x567d0},{"navigate",0x137b08},{"browser_ctor",0x136af0},{"browser_vtable",0x23fc78},{"browser_interface",0x23ff40},{"reorder_iid",0x26f5e0},{"reorder_service",0x26f5f0}});
            p.fields={{"actions",0xe8},{"active_guid",0x318},{"browser_interface",0x10},{"reorder_provider",0x2b0},{"reorder_method",5}};
        }else p=cached(image,cache_directory());
    }catch(const std::exception& ex){p.reason=ex.what();}
    retry_after=GetTickCount64()+5000;
    if(selected&&selected->inventory&&!compatible_extension(*selected,p))return selected;
    selected=std::make_shared<Profile>(std::move(p));return selected;
}
bool compatible_extension(const Profile& previous,const Profile& next){
    if(previous.identity!=next.identity)return false;
    for(auto& [key,value]:previous.rvas)if(!next.rvas.contains(key)||next.rvas.at(key)!=value)return false;
    for(auto& [key,value]:previous.fields)if(!next.fields.contains(key)||next.fields.at(key)!=value)return false;
    const bool old_flags[]{previous.inventory,previous.title,previous.selection,previous.close_guard,previous.opening,previous.navigation_guard,previous.reorder,previous.xaml_identity,previous.reorder_extension};
    const bool new_flags[]{next.inventory,next.title,next.selection,next.close_guard,next.opening,next.navigation_guard,next.reorder,next.xaml_identity,next.reorder_extension};
    for(size_t i=0;i<std::size(old_flags);++i)if(old_flags[i]&&!new_flags[i])return false;
    return true;
}
void refresh_profile(){std::lock_guard guard(profile_mutex);selected.reset();retry_after=0;}
Json status(const fs::path& cache){
    Json result{{"cache_directory",utf8(cache.wstring())},{"network_used",false},{"schema",cache_schema}};
    try{
        Image image(read_file(frame_path()));result["image"]=image.identity();result["manifest"]=utf8(manifest_path(cache,image).wstring());
        Profile p;p.identity=image.identity();
        try{p=cached(image,cache);result["cache_valid"]=p.inventory;}catch(const std::exception& ex){p.reason=ex.what();result["cache_valid"]=false;}
        result["symbol_profile"]=profile_report(p);
        bool baseline=image.sha256==supported_frame_sha256;
        result["capabilities"]={{"com_observation",true},{"native_inventory",baseline||p.inventory},{"title",baseline||p.title},{"protection",baseline||p.close_guard},{"tab_selection",baseline||p.selection},
            {"directory_lock",baseline||p.navigation_guard},{"open_tab",baseline||p.opening},{"reorder_frame",baseline||p.reorder},
            {"tab_icons_and_text","Independent XAML module identity and runtime readback required"},{"tab_width",baseline||p.inventory},
            {"reorder","Frame contract plus separate extension method identity and actual order readback required"}};
        result["xaml_modules"]=xaml_status(cache);
        auto& xaml=result["xaml_modules"];bool appearance=(baseline||p.inventory)&&xaml["FileExplorerExtensions.dll"].value("identity_ready",false)&&xaml["Windows.UI.FileExplorer.dll"].value("identity_ready",false);
        result["capabilities"]["tab_icons_and_text"]=appearance;
        result["capabilities"]["tab_background"]=appearance;
        result["capabilities"]["reorder"]=(baseline||p.reorder)&&xaml["Windows.UI.FileExplorer.dll"].value("reorder_ready",false);
        result["runtime_validation"]="Contract readiness only; live module/object identity and actual UI readback are still required";
        result["adapter"]=baseline?"built-in exact SHA256":p.source;result["ready"]=baseline||p.title;
        result["message"]=baseline?"内置适配可离线使用；符号缓存状态独立显示。":p.reason;
    }catch(const std::exception& ex){result["ready"]=false;result["error"]=ex.what();result["message"]=ex.what();}
    return result;
}
}
