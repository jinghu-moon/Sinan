#include "compat/symbols.hpp"
#include <hde64.h>
#include <cstring>
#include <algorithm>
#include <functional>

namespace sinan::compat {
namespace {
struct Ins {uint32_t rva;hde64s h;std::vector<unsigned char> b;};
bool prefix(std::span<const unsigned char> b,std::initializer_list<unsigned char> p){return b.size()>=p.size()&&std::equal(p.begin(),p.end(),b.begin());}
uint32_t u32(const unsigned char* b){uint32_t v{};std::memcpy(&v,b,4);return v;}
std::vector<Ins> decode(const Image& image,uint32_t rva,size_t limit,bool constructor=false){
    std::vector<Ins> out;
    for(size_t off=0;off<limit;){auto bytes=image.at(rva+static_cast<uint32_t>(off),16);hde64s h{};hde64_disasm(bytes.data(),&h);
        if(!h.len||(h.flags&F_ERROR))throw Error(3,"Invalid x64 contract instruction at RVA "+std::to_string(rva+off));
        if(h.len>limit-off)break;
        if(constructor&&(h.opcode==0xe8||h.opcode==0xe9||h.opcode==0xeb||h.opcode==0xc3||h.opcode==0xc2||
            (h.opcode==0xff&&(h.modrm_reg==2||h.modrm_reg==4))||(h.opcode>=0x70&&h.opcode<=0x7f)||(h.opcode==0x0f&&h.opcode2>=0x80&&h.opcode2<=0x8f)))break;
        out.push_back({rva+static_cast<uint32_t>(off),h,{bytes.begin(),bytes.begin()+h.len}});off+=h.len;
    }return out;
}
uint32_t unique(const std::vector<uint32_t>& values,const char* name){if(values.size()!=1)throw Error(3,std::string("Missing/ambiguous contract: ")+name);return values[0];}
uint32_t branch(const Ins& i){auto target=int64_t(i.rva)+i.h.len+static_cast<int32_t>(u32(i.b.data()+i.h.len-4));if(target<0||target>UINT32_MAX)throw Error(3,"Contract branch overflow");return static_cast<uint32_t>(target);}
uint32_t constructor_offset(const Image& image,uint32_t ctor,uint32_t vt){
    auto code=decode(image,ctor,(std::min)(image.function_size(ctor),uint32_t(768)),true);std::vector<uint32_t> matches;
    for(size_t i=0;i<code.size();++i){if(code[i].b.size()!=7||!prefix(code[i].b,{0x48,0x8d,0x05})||branch(code[i])!=vt)continue;
        for(size_t j=i+1;j<code.size()&&j<=i+3;++j){auto& b=code[j].b;
            if(b.size()==3&&prefix(b,{0x48,0x89,0x01})){matches.push_back(0);break;}
            if(b.size()==4&&prefix(b,{0x48,0x89,0x41})){if(b[3]<128)matches.push_back(b[3]);break;}
            if(b.size()==7&&prefix(b,{0x48,0x89,0x81})){matches.push_back(u32(b.data()+3));break;}
            // Permit interleaved stores to unrelated RCX fields, as found in
            // CShellBrowser's ctor. No intervening register writes/calls.
            if(!(code[j].h.opcode==0x89&&code[j].h.modrm_mod!=3&&code[j].h.modrm_rm==1&&!code[j].h.rex_b))break;
        }
    }
    auto offset=unique(matches,"constructor interface placement");if(offset>0x1000||offset%8)throw Error(3,"Implausible interface offset");return offset;
}
void symbol(const Image& image,const Profile& p,const char* key,bool code=true){
    if(!p.rvas.contains(key))throw Error(3,std::string("Missing symbol: ")+key);
    auto rva=p.rva(key);image.at(rva,code?16:8);
    if(code?!image.executable(rva):!image.readonly_data(rva))throw Error(3,std::string("Symbol section mismatch: ")+key);
}
void entry(const Image& image,const Profile& p,const char* key,std::initializer_list<unsigned char> bytes){symbol(image,p,key);if(!prefix(image.at(p.rva(key),bytes.size()),bytes))throw Error(3,std::string("Function entry contract changed: ")+key);}
void slot(const Image& image,uint32_t vt,uint32_t function,size_t index){uint64_t target{};auto bytes=image.at(vt+static_cast<uint32_t>(index*8),8);std::memcpy(&target,bytes.data(),8);if(target!=image.preferred_base+function)throw Error(3,"Action vtable method mismatch");}
bool has(const std::vector<Ins>& code,std::initializer_list<unsigned char> bytes){return std::any_of(code.begin(),code.end(),[&](auto& i){return i.b.size()==bytes.size()&&prefix(i.b,bytes);});}
void actions(const Image& image,Profile& p){symbol(image,p,"actions_vtable",false);auto offset=constructor_offset(image,p.rva("frame_ctor"),p.rva("actions_vtable"));if(offset>=0x2f0)throw Error(3,"Frame actions offset overlaps tab table");p.fields["actions"]=offset;}
void attempt(Profile& p,const char* key,bool& flag,const std::function<void()>& f){try{f();flag=true;}catch(const std::exception& ex){flag=false;p.failures[key]=ex.what();}}
}
void validate_features(const Image& image,Profile& p){
    // Each group has an independent admission. A missing navigation symbol
    // cannot disable a validated close guard, selection, inventory or title.
    attempt(p,"selection",p.selection,[&]{
        actions(image,p);entry(image,p,"select",{0x4c,0x8b,0xdc,0x49,0x89,0x5b,0x18,0x49,0x89,0x73,0x20});
        slot(image,p.rva("actions_vtable"),p.rva("select"),5);
        auto code=decode(image,p.rva("select"),image.function_size(p.rva("select")));std::vector<uint32_t> active;
        if(!has(code,{0x48,0x8b,0xd9}))throw Error(3,"SelectTab this register contract changed");
        for(size_t i=0;i+7<code.size();++i){auto& a=code[i];auto& record=code[i+3];
            if(a.b.size()!=7||!prefix(a.b,{0x4c,0x8d,0xb3})||static_cast<int32_t>(u32(a.b.data()+3))!=-static_cast<int32_t>(p.field("actions")))continue;
            if(record.b.size()!=7||!prefix(record.b,{0x48,0x8d,0xb3})||!prefix(code[i+4].b,{0x4c,0x8b,0xc6})||!prefix(code[i+6].b,{0x49,0x8b,0xce})||code[i+7].h.opcode!=0xe8||branch(code[i+7])!=p.rva("tab_lookup"))continue;
            auto field=uint64_t(p.field("actions"))+u32(record.b.data()+3)+16;
            if(field<0x300||field>0x1000||field%8)throw Error(3,"Implausible selected record location");
            active.push_back(static_cast<uint32_t>(field));
        }
        p.fields["active_guid"]=unique(active,"SelectTab -> GetBrowserTabFromId output");
        // Check that SelectTab sees the same table as the independent reader.
        bool begin=false,end=false;
        for(auto& i:code){if(i.b.size()!=7)continue;if(prefix(i.b,{0x48,0x2b,0xbb}))begin|=u32(i.b.data()+3)+p.field("actions")==0x2f0;
            if(prefix(i.b,{0x48,0x8b,0xbb}))end|=u32(i.b.data()+3)+p.field("actions")==0x2f8;}
        if(!begin||!end)throw Error(3,"SelectTab table differs from inventory contract");
    });
    attempt(p,"close_guard",p.close_guard,[&]{
        actions(image,p);entry(image,p,"remove",{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55});
        entry(image,p,"close",{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57});
        slot(image,p.rva("actions_vtable"),p.rva("remove"),4);
        auto code=decode(image,p.rva("remove"),(std::min)(image.function_size(p.rva("remove")),uint32_t(256)));bool begin=false,end=false;
        if(!has(code,{0x48,0x8b,0xf1}))throw Error(3,"RemoveTab this register contract changed");
        for(auto& i:code){if(i.b.size()!=7)continue;if(prefix(i.b,{0x48,0x2b,0x86}))begin|=u32(i.b.data()+3)+p.field("actions")==0x2f0;
            if(prefix(i.b,{0x48,0x8b,0x86}))end|=u32(i.b.data()+3)+p.field("actions")==0x2f8;}
        if(!begin||!end)throw Error(3,"RemoveTab table differs from inventory contract");
    });
    attempt(p,"opening",p.opening,[&]{
        actions(image,p);entry(image,p,"add",{0x48,0x83,0xec,0x38,0x48,0x8b,0x01,0x4c,0x89,0x4c,0x24,0x20,0x41,0x83,0xc9,0xff});
        symbol(image,p,"add_index");slot(image,p.rva("actions_vtable"),p.rva("add"),3);slot(image,p.rva("actions_vtable"),p.rva("add_index"),9);
        if(!prefix(image.at(p.rva("add")+16,4),{0x48,0x8b,0x40,0x48}))throw Error(3,"AddTab dispatch slot changed");
    });
    attempt(p,"navigation_guard",p.navigation_guard,[&]{
        if(!p.opening)throw Error(3,"Navigation redirect needs a verified AddTab");
        entry(image,p,"navigate",{0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54});
        entry(image,p,"get_browser",{0x48,0x83,0xec,0x28,0x49,0x83,0x20,0x00});
        slot(image,p.rva("host_interface"),p.rva("get_browser"),5);
        symbol(image,p,"browser_ctor");symbol(image,p,"browser_vtable",false);symbol(image,p,"browser_interface",false);
        if(constructor_offset(image,p.rva("browser_ctor"),p.rva("browser_vtable"))!=0)throw Error(3,"Navigation object base changed");
        p.fields["browser_interface"]=constructor_offset(image,p.rva("browser_ctor"),p.rva("browser_interface"));
    });
    attempt(p,"reorder",p.reorder,[&]{
        if(!p.selection)throw Error(3,"Reorder needs verified selection restoration");
        entry(image,p,"move_selected",{0x89,0x54,0x24,0x10,0x53,0x57,0x48,0x83,0xec,0x38});
        symbol(image,p,"reorder_iid",false);symbol(image,p,"reorder_service",false);
        auto code=decode(image,p.rva("move_selected"),image.function_size(p.rva("move_selected")));std::vector<uint32_t> providers;unsigned calls=0,active=0;
        for(size_t i=0;i<code.size();++i){auto& a=code[i];
            if(i+3<code.size()){auto& b=code[i+1];auto& c=code[i+2];auto& d=code[i+3];
                if(a.b.size()==7&&prefix(a.b,{0x4c,0x8d,0x05})&&branch(a)==p.rva("reorder_iid")&&b.b.size()==7&&prefix(b.b,{0x48,0x8d,0x15})&&branch(b)==p.rva("reorder_service")&&c.b.size()==7&&prefix(c.b,{0x48,0x8b,0x8b})&&prefix(d.b,{0x48,0xff,0x15}))providers.push_back(u32(c.b.data()+3));}
            if(a.b.size()==7&&prefix(a.b,{0x0f,0x10,0x83})&&u32(a.b.data()+3)==p.field("active_guid"))++active;
            if(i+1<code.size()&&prefix(a.b,{0x48,0x8b,0x40,0x28})&&code[i+1].h.opcode==0xe8)++calls;
        }
        auto provider=unique(providers,"MoveSelectedTabToPosition QueryService route");if(provider>0x1000||provider%8||calls!=1||active!=1)throw Error(3,"Reorder dispatch/active record contract changed");
        p.fields["reorder_provider"]=provider;p.fields["reorder_method"]=5;
    });
}
Json profile_report(const Profile& p){return {{"inventory",p.inventory},{"title",p.title},{"selection",p.selection},{"close_guard",p.close_guard},
    {"opening",p.opening},{"navigation_guard",p.navigation_guard},{"reorder_frame",p.reorder},{"reason",p.reason},{"failures",p.failures},{"rvas",p.rvas},{"fields",p.fields}};}
Profile validate_xaml_profile(const Image& image,const Rvas& rvas,Module module){
    Profile p;p.identity=image.identity();p.rvas=rvas;p.source="symbols + independent XAML module contracts v2";
    if(module==Module::view){attempt(p,"xaml_identity",p.xaml_identity,[&]{
        entry(image,p,"view_get",{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x8b,0xc1,0x48,0x83,0x22,0});
        symbol(image,p,"view_vtable",false);std::vector<uint32_t> slots;
        for(uint32_t i=6;i<24;++i){uint64_t value{};auto bytes=image.at(p.rva("view_vtable")+i*8,8);std::memcpy(&value,bytes.data(),8);if(value==image.preferred_base+p.rva("view_get"))slots.push_back(i);}
        p.fields["view_get_slot"]=unique(slots,"FileExplorerTab ViewModel getter slot");
    });}
    else if(module==Module::ui){
        attempt(p,"xaml_identity",p.xaml_identity,[&]{
            symbol(image,p,"tab_ctor");symbol(image,p,"tab_vtable",false);auto iface=constructor_offset(image,p.rva("tab_ctor"),p.rva("tab_vtable"));
            auto code=decode(image,p.rva("tab_ctor"),(std::min)(image.function_size(p.rva("tab_ctor")),uint32_t(768)),true);std::vector<uint32_t> guids;
            if(!has(code,{0x4c,0x8b,0xfa})||!has(code,{0x4c,0x8b,0xe1}))throw Error(3,"FileExplorerTab constructor argument registers changed");
            // The named ctor takes FileExplorerTabData by value. Verify the
            // complete GUID copy (DWORD, WORD, WORD and eight BYTE fields).
            const std::vector<std::vector<unsigned char>> stores{{0x41,0x89,0x84,0x24},{0x66,0x41,0x89,0x8c,0x24},{0x66,0x41,0x89,0x94,0x24},
                {0x45,0x88,0x84,0x24},{0x45,0x88,0x8c,0x24},{0x45,0x88,0x94,0x24},{0x45,0x88,0x9c,0x24},{0x41,0x88,0x9c,0x24},{0x41,0x88,0xbc,0x24},{0x41,0x88,0xb4,0x24},{0x41,0x88,0xac,0x24}};
            const std::vector<std::vector<unsigned char>> loads{{0x44,0x8a,0x42,8},{0x44,0x8a,0x4a,9},{0x44,0x8a,0x52,10},{0x44,0x8a,0x5a,11},
                {0x8a,0x5a,12},{0x40,0x8a,0x7a,13},{0x40,0x8a,0x72,14},{0x40,0x8a,0x6a,15},{0x0f,0xb7,0x52,6},{0x41,0x0f,0xb7,0x4f,4},{0x41,0x8b,7}};
            const uint32_t deltas[]{0,4,6,8,9,10,11,12,13,14,15};
            for(size_t i=loads.size();i+stores.size()<=code.size();++i){bool matched=true;uint32_t base=0;
                for(size_t j=0;j<loads.size();++j)if(code[i-loads.size()+j].b!=loads[j]){matched=false;break;}
                if(!matched)continue;
                for(size_t j=0;j<stores.size();++j){auto& b=code[i+j].b;auto& op=stores[j];if(b.size()!=op.size()+4||!std::equal(op.begin(),op.end(),b.begin())){matched=false;break;}
                    auto field=u32(b.data()+op.size());if(j==0)base=field;if(uint64_t(base)+deltas[j]!=field){matched=false;break;}}
                if(matched&&base>=iface&&base-iface<=0x1000)guids.push_back(base-iface);
            }
            p.fields["tab_guid"]=unique(guids,"FileExplorerTabData GUID copy");
        });
        attempt(p,"reorder_extension",p.reorder_extension,[&]{entry(image,p,"move_tab",{0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x57,0x48,0x83,0xec,0x30,0x41,0x8b,0xf8});});
    }
    if(p.xaml_identity||p.reorder_extension)p.reason="Module symbols and applicable XAML contracts accepted; live object identity and readback required";
    else p.reason=p.failures.dump();return p;
}
}
